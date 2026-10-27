/**
 * @file clock.c
 * @brief The real-time clock: set from the network by SNTP (pool.ntp.org) once WiFi is up, or by hand with `date`.
 *
 * It keeps UTC; the C library's time() and FatFs file dates (get_fattime in
 * usb_storage.c) use it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "lwip/apps/sntp.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "net.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "shell.h"

static volatile bool synced;  ///< set by SNTP at least once

void net_set_time(uint32_t sec, uint32_t us) {
    // Called in lwIP's interrupt; settimeofday only stores an offset.
    struct timeval tv = {.tv_sec = (time_t)sec, .tv_usec = (suseconds_t)us};
    settimeofday(&tv, NULL);
    synced = true;
}

void net_clock_start(void) {
    static bool started;
    if (started)
        return;
    started = true;
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    cyw43_arch_lwip_end();
}

void net_cmd_date(int argc, char **argv) {
    if (argc == 3) {
        // date YYYY-MM-DD HH:MM[:SS], in UTC
        struct tm tm = {0};
        if (sscanf(argv[1], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
            sscanf(argv[2], "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 2) {
            printf("usage: date [YYYY-MM-DD HH:MM[:SS]]   (UTC)\n");
            return;
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        struct timeval tv = {.tv_sec = mktime(&tm)};  // newlib's TZ is UTC
        settimeofday(&tv, NULL);
        synced = false;
    } else if (argc != 1) {
        printf("usage: date [YYYY-MM-DD HH:MM[:SS]]   (UTC)\n");
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        printf("The clock isn't set: it's set from the network once WiFi connects,\n"
               "or by hand with date YYYY-MM-DD HH:MM (UTC)\n");
        return;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    char text[40];
    strftime(text, sizeof text, "%a %Y-%m-%d %H:%M:%S UTC", &tm);
    printf("%s%s\n", text, synced ? "" : " (set by hand)");
}

// ---- The ntp command ----------------------------------------------------------

#define NTP_PORT        123
#define NTP_UNIX_OFFSET 2208988800u  ///< seconds from 1900 (NTP) to 1970 (Unix)
#define NTP_TIMEOUT_MS  5000
#define NTP_RESEND_MS   1500  ///< UDP can be lost: ask again this often

static volatile bool ntp_got;
static uint8_t ntp_reply[48];
static uint64_t ntp_rx_us;  ///< time_us_64() when the reply arrived

static void on_ntp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                   u16_t port) {
    if (!ntp_got && p->tot_len >= sizeof ntp_reply) {
        ntp_rx_us = time_us_64();
        pbuf_copy_partial(p, ntp_reply, sizeof ntp_reply, 0);
        ntp_got = true;
    }
    pbuf_free(p);
}

static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void put32(uint8_t *b, uint32_t v) {
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

/// An NTP timestamp (seconds since 1900, 32.32 fixed point) as Unix microseconds.
static int64_t ntp_to_us(const uint8_t *b) {
    return ((int64_t)get32(b) - NTP_UNIX_OFFSET) * 1000000 +
           (int64_t)(((uint64_t)get32(b + 4) * 1000000) >> 32);
}

static int64_t wall_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/// Prints a time difference: "12 ms" or "3.4 s" or "2 days".
static void print_span(int64_t us) {
    if (us < 1000000)
        printf("%lld ms", (long long)(us / 1000));
    else if (us < 100000000)
        printf("%lld.%lld s", (long long)(us / 1000000), (long long)(us / 100000 % 10));
    else if (us < 2 * 86400000000LL)
        printf("%lld s", (long long)(us / 1000000));
    else
        printf("%lld days", (long long)(us / 86400000000LL));
}

void net_cmd_ntp(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: ntp [server]   (default pool.ntp.org)\n");
        return;
    }
    const char *server = argc > 1 ? argv[1] : "pool.ntp.org";
    if (!net_need_link())
        return;
    ip_addr_t addr;
    if (!net_resolve(server, &addr))
        return;
    char ip[IPADDR_STRLEN_MAX];
    ipaddr_ntoa_r(&addr, ip, sizeof ip);

    cyw43_arch_lwip_begin();
    struct udp_pcb *pcb = udp_new();
    if (pcb)
        udp_recv(pcb, on_ntp, NULL);
    cyw43_arch_lwip_end();
    if (!pcb) {
        printf("Out of network memory\n");
        return;
    }

    // Each request carries its send time in the transmit field; the server
    // echoes it back as the origin, which matches a reply to its request.
    uint8_t sent_stamp[8];
    int64_t t1 = 0;          // wall clock when the last request went
    uint64_t t1_us = 0;      // time_us_64() then
    bool ok = false, stopped = false;
    absolute_time_t until = make_timeout_time_ms(NTP_TIMEOUT_MS);
    absolute_time_t resend = nil_time;
    ntp_got = false;
    while (!ok) {
        if (time_reached(until))
            break;
        if (net_stop_key()) {
            stopped = true;
            break;
        }
        if (time_reached(resend)) {
            t1 = wall_us();
            t1_us = time_us_64();
            uint64_t ntp_sec = (uint64_t)(t1 / 1000000) + NTP_UNIX_OFFSET;
            put32(sent_stamp, (uint32_t)ntp_sec);
            put32(sent_stamp + 4, (uint32_t)(((uint64_t)(t1 % 1000000) << 32) / 1000000));
            cyw43_arch_lwip_begin();
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
            if (p) {
                uint8_t *b = p->payload;
                memset(b, 0, 48);
                b[0] = 0 << 6 | 4 << 3 | 3;  // no leap warning, version 4, client
                memcpy(b + 40, sent_stamp, 8);
                ntp_got = false;
                udp_sendto(pcb, p, &addr, NTP_PORT);
                pbuf_free(p);
            }
            cyw43_arch_lwip_end();
            resend = make_timeout_time_ms(NTP_RESEND_MS);
        }
        if (ntp_got) {
            const uint8_t *r = ntp_reply;
            if ((r[0] & 7) == 4 && memcmp(r + 24, sent_stamp, 8) == 0)
                ok = true;
            else
                ntp_got = false;  // not ours, or a late answer to an earlier request
        }
        sleep_ms(2);
    }
    cyw43_arch_lwip_begin();
    udp_remove(pcb);
    cyw43_arch_lwip_end();

    char name[80];
    if (strcmp(server, ip) == 0)
        snprintf(name, sizeof name, "%s", server);
    else
        snprintf(name, sizeof name, "%s (%s)", server, ip);
    if (stopped) {
        printf("Stopped\n");
        return;
    }
    if (!ok) {
        printf("No answer from %s: it may not be a time server\n", name);
        return;
    }
    const uint8_t *r = ntp_reply;
    int stratum = r[1];
    if ((r[0] >> 6) == 3 || stratum == 0 || stratum > 15) {
        printf("%s answered but its own clock isn't set (stratum %d); try another server\n", name,
               stratum);
        return;
    }
    // The usual NTP sums: t2/t3 are the server's receive/transmit times.
    int64_t t2 = ntp_to_us(r + 32), t3 = ntp_to_us(r + 40);
    int64_t t4 = t1 + (int64_t)(ntp_rx_us - t1_us);
    int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
    int64_t round_trip = (t4 - t1) - (t3 - t2);
    bool was_set = t1 >= 1700000000LL * 1000000;

    int64_t now = wall_us() + offset;
    struct timeval tv = {.tv_sec = (time_t)(now / 1000000), .tv_usec = (suseconds_t)(now % 1000000)};
    settimeofday(&tv, NULL);
    synced = true;

    time_t secs = tv.tv_sec;
    struct tm tm;
    gmtime_r(&secs, &tm);
    char text[40];
    strftime(text, sizeof text, "%a %Y-%m-%d %H:%M:%S UTC", &tm);
    printf("Time set from %s: %s\n", name, text);
    if (!was_set) {
        printf("The clock wasn't set before");
    } else {
        printf("The clock was ");
        print_span(offset < 0 ? -offset : offset);
        printf(offset < 0 ? " ahead" : " behind");
    }
    printf("; round trip %lld ms, stratum %d\n", (long long)(round_trip / 1000), stratum);
}
