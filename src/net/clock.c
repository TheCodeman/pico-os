/**
 * @file clock.c
 * @brief The real-time clock: set from the network (NTP, pool.ntp.org) once WiFi is up and every hour after, or by hand with `date`.
 *
 * It keeps UTC; times are shown in the time zone set with `tz` (saved in
 * /TZ.CFG as a POSIX TZ string, for the C library's localtime). The C
 * library's time() and FatFs file dates (get_fattime in
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

#include "ff.h"
#include "lwip/dns.h"
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


// ---- Time zone ------------------------------------------------------------------

/// Where the time zone is kept.
#define TZ_FILE "/TZ.CFG"

/// Time zones by name, as POSIX TZ strings (US and EU daylight saving rules).
static const struct {
    const char *name;  ///< what tz takes (any case)
    const char *posix; ///< the TZ string
} zones[] = {
    {"UTC", "UTC0"},
    {"PST", "PST8PDT,M3.2.0,M11.1.0"},   {"PDT", "PST8PDT,M3.2.0,M11.1.0"},
    {"PACIFIC", "PST8PDT,M3.2.0,M11.1.0"},
    {"MST", "MST7MDT,M3.2.0,M11.1.0"},   {"MDT", "MST7MDT,M3.2.0,M11.1.0"},
    {"MOUNTAIN", "MST7MDT,M3.2.0,M11.1.0"}, {"ARIZONA", "MST7"},
    {"CST", "CST6CDT,M3.2.0,M11.1.0"},   {"CDT", "CST6CDT,M3.2.0,M11.1.0"},
    {"CENTRAL", "CST6CDT,M3.2.0,M11.1.0"},
    {"EST", "EST5EDT,M3.2.0,M11.1.0"},   {"EDT", "EST5EDT,M3.2.0,M11.1.0"},
    {"EASTERN", "EST5EDT,M3.2.0,M11.1.0"},
    {"AKST", "AKST9AKDT,M3.2.0,M11.1.0"}, {"ALASKA", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"HST", "HST10"},                    {"HAWAII", "HST10"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0"},  {"GMT", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"CET", "CET-1CEST,M3.5.0,M10.5.0/3"}, {"EET", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"JST", "JST-9"},                    {"AEST", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};

/// Times shown as 9:14:55 PM rather than 21:14:55 (tz 12 / tz 24).
static bool hours12;

/// Uses a POSIX TZ string from now on.
/// @param posix the TZ string
static void use_tz(const char *posix) {
    setenv("TZ", posix, 1);
    tzset();
}

void net_tz_load(void) {
    FIL f;
    char line[64];
    if (f_open(&f, TZ_FILE, FA_READ) != FR_OK)
        return;
    if (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            use_tz(line);
    }
    if (f_gets(line, sizeof line, &f))  // the second line: 12 or 24
        hours12 = atoi(line) == 12;
    f_close(&f);
}

/// The local time as text: "Thu 2026-10-29 21:11:30 PDT".
/// @param t the time
/// @param out the text
/// @param size room in out
static void local_text(time_t t, char *out, size_t size) {
    struct tm tm;
    localtime_r(&t, &tm);
    if (hours12) {
        char zone[16];
        strftime(zone, sizeof zone, "%Z", &tm);
        int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        size_t n = strftime(out, size, "%a %Y-%m-%d ", &tm);
        snprintf(out + n, size - n, "%d:%02d:%02d %s %s", h, tm.tm_min, tm.tm_sec,
                 tm.tm_hour < 12 ? "AM" : "PM", zone);
    } else {
        strftime(out, size, "%a %Y-%m-%d %H:%M:%S %Z", &tm);
    }
}

void net_cmd_tz(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: tz [PST | MST | CST | EST | UTC | ... | POSIX TZ string | 12 | 24]\n");
        return;
    }
    if (argc == 2 && (!strcmp(argv[1], "12") || !strcmp(argv[1], "24"))) {
        hours12 = argv[1][0] == '1';
    } else if (argc == 2) {
        const char *posix = argv[1];
        for (unsigned i = 0; i < sizeof zones / sizeof *zones; ++i)
            if (!strcasecmp(argv[1], zones[i].name))
                posix = zones[i].posix;
        if (posix == argv[1] && !strpbrk(posix, "0123456789")) {
            printf("Unknown time zone %s. Names: ", argv[1]);
            for (unsigned i = 0; i < sizeof zones / sizeof *zones; ++i)
                printf("%s%s", i ? " " : "", zones[i].name);
            printf("\nor a POSIX TZ string, e.g. tz PST8PDT,M3.2.0,M11.1.0\n");
            return;
        }
        use_tz(posix);
    }
    const char *tz = getenv("TZ");
    if (argc == 2) {  // saved: the zone, then 12 or 24
        FIL f;
        if (f_open(&f, TZ_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            f_printf(&f, "%s\r\n%d\r\n", tz ? tz : "UTC0", hours12 ? 12 : 24);
            f_close(&f);
        } else {
            printf("(not saved: no drive)\n");
        }
    }
    printf("Time zone: %s, %d-hour clock\n", tz ? tz : "UTC0 (tz PST etc. changes it)",
           hours12 ? 12 : 24);
    time_t now = time(NULL);
    if (now >= 1700000000) {
        char text[48];
        local_text(now, text, sizeof text);
        printf("It is %s\n", text);
    }
}

void net_cmd_date(int argc, char **argv) {
    if (argc == 3) {
        // date YYYY-MM-DD HH:MM[:SS], in the time zone
        struct tm tm = {0};
        if (sscanf(argv[1], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
            sscanf(argv[2], "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 2) {
            printf("usage: date [YYYY-MM-DD HH:MM[:SS]]   (local time; see tz)\n");
            return;
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        struct timeval tv = {.tv_sec = mktime(&tm)};  // local time (TZ) to UTC
        settimeofday(&tv, NULL);
        synced = false;
    } else if (argc != 1) {
        printf("usage: date [YYYY-MM-DD HH:MM[:SS]]   (local time; see tz)\n");
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        printf("The clock isn't set: it's set from the network once WiFi connects,\n"
               "or by hand with date YYYY-MM-DD HH:MM (local time; see tz)\n");
        return;
    }
    char text[48];
    local_text(now, text, sizeof text);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (getenv("TZ"))
        printf("%s (%02d:%02d UTC)%s\n", text, utc.tm_hour, utc.tm_min, synced ? "" : " (set by hand)");
    else
        printf("%s%s\n", text, synced ? "" : " (set by hand)");
}

// ---- The ntp command ----------------------------------------------------------

/// NTP's UDP port.
#define NTP_PORT        123
#define NTP_UNIX_OFFSET 2208988800u  ///< seconds from 1900 (NTP) to 1970 (Unix)
/// How long to wait for an answer.
#define NTP_TIMEOUT_MS  5000
#define NTP_RESEND_MS   1500  ///< UDP can be lost: ask again this often

/// The reply came.
static volatile bool ntp_got;
/// The reply.
static uint8_t ntp_reply[48];
static uint64_t ntp_rx_us;  ///< time_us_64() when the reply arrived

/// lwIP's UDP receive callback: keeps the reply.
/// @param arg unused
/// @param pcb the socket
/// @param p the packet
/// @param addr the sender
/// @param port its port
static void on_ntp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                   u16_t port) {
    if (!ntp_got && p->tot_len >= sizeof ntp_reply) {
        ntp_rx_us = time_us_64();
        pbuf_copy_partial(p, ntp_reply, sizeof ntp_reply, 0);
        ntp_got = true;
    }
    pbuf_free(p);
}

/// A big-endian 32-bit number.
/// @param b the bytes
/// @return it
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/// Stores a big-endian 32-bit number.
/// @param b the bytes
/// @param v it
static void put32(uint8_t *b, uint32_t v) {
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

/// An NTP timestamp (seconds since 1900, 32.32 fixed point) as Unix microseconds.
/// @param b the timestamp
/// @return the microseconds
static int64_t ntp_to_us(const uint8_t *b) {
    return ((int64_t)get32(b) - NTP_UNIX_OFFSET) * 1000000 +
           (int64_t)(((uint64_t)get32(b + 4) * 1000000) >> 32);
}

/// The clock, in Unix microseconds.
/// @return it
static int64_t wall_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/// Prints a time difference: "12 ms" or "3.4 s" or "2 days".
/// @param us the difference
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

    char text[48];
    local_text(tv.tv_sec, text, sizeof text);
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

// ---- Setting the clock in the background -----------------------------------------
// lwIP's SNTP client never set the clock here, so the main loop asks
// pool.ntp.org itself, as the ntp command does, without printing or waiting:
// a lookup, then a request (sent again if no answer comes), then the clock is
// set from the answer. Until it is set it tries every 30 s, then every hour.

#define BG_SERVER   "pool.ntp.org"  ///< who is asked
#define BG_RETRY_MS 30000           ///< again this soon while the clock isn't set
#define BG_EVERY_MS 3600000         ///< and this often once it is
#define BG_WAIT_MS  2000            ///< how long an answer (or a lookup) may take
#define BG_TRIES    3               ///< requests sent before giving up for now

/// The background sync's state.
static enum { BG_OFF, BG_IDLE, BG_LOOKUP, BG_ASKED } bg_state;
static absolute_time_t bg_when;       ///< the next step's time (start, or give up)
static ip_addr_t bg_addr;             ///< the server's address
static volatile int bg_dns;           ///< 0 waiting, 1 found, -1 not found
static struct udp_pcb *bg_pcb;        ///< the socket, while asking
static volatile bool bg_got;          ///< an answer came
static uint8_t bg_reply[48];          ///< it
static uint64_t bg_rx_us;             ///< time_us_64() when it came
static uint8_t bg_stamp[8];           ///< the request's send time, echoed in the answer
static int64_t bg_t1;                 ///< the wall clock then
static uint64_t bg_t1_us;             ///< and time_us_64()
static int bg_tries;                  ///< requests sent this time

/// lwIP's DNS callback for the background sync.
/// @param name the name
/// @param addr its address, or NULL
/// @param arg unused
static void bg_on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    if (addr)
        bg_addr = *addr;
    bg_dns = addr ? 1 : -1;
}

/// lwIP's UDP callback for the background sync: keeps the answer.
/// @param arg unused
/// @param pcb the socket
/// @param p the packet
/// @param addr the sender
/// @param port its port
static void bg_on_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                      u16_t port) {
    if (!bg_got && p->tot_len >= sizeof bg_reply) {
        bg_rx_us = time_us_64();
        pbuf_copy_partial(p, bg_reply, sizeof bg_reply, 0);
        bg_got = true;
    }
    pbuf_free(p);
}

/// Sends a request (from the main loop; takes the lwIP lock).
static void bg_send(void) {
    bg_t1 = wall_us();
    bg_t1_us = time_us_64();
    put32(bg_stamp, (uint32_t)((uint64_t)(bg_t1 / 1000000) + NTP_UNIX_OFFSET));
    put32(bg_stamp + 4, (uint32_t)(((uint64_t)(bg_t1 % 1000000) << 32) / 1000000));
    cyw43_arch_lwip_begin();
    if (!bg_pcb && (bg_pcb = udp_new()) != NULL)
        udp_recv(bg_pcb, bg_on_udp, NULL);
    struct pbuf *p = bg_pcb ? pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM) : NULL;
    if (p) {
        uint8_t *b = p->payload;
        memset(b, 0, 48);
        b[0] = 0 << 6 | 4 << 3 | 3;  // no leap warning, version 4, client
        memcpy(b + 40, bg_stamp, 8);
        bg_got = false;
        udp_sendto(bg_pcb, p, &bg_addr, NTP_PORT);
        pbuf_free(p);
    }
    cyw43_arch_lwip_end();
    ++bg_tries;
    bg_state = BG_ASKED;
    bg_when = make_timeout_time_ms(BG_WAIT_MS);
}

/// Ends a try: the socket goes, and the next try is due in `ms`.
/// @param ms when to try again
static void bg_done(uint32_t ms) {
    cyw43_arch_lwip_begin();
    if (bg_pcb)
        udp_remove(bg_pcb);
    cyw43_arch_lwip_end();
    bg_pcb = NULL;
    bg_state = BG_IDLE;
    bg_when = make_timeout_time_ms(ms);
}

void net_clock_start(void) {
    if (bg_state == BG_OFF || bg_state == BG_IDLE) {
        bg_state = BG_IDLE;
        bg_when = get_absolute_time();  // now (WiFi has just come up)
    }
}

void net_clock_task(void) {
    switch (bg_state) {
    case BG_OFF:  // WiFi is up, however it came up (wifi wait doesn't call net_clock_start)
        net_clock_start();
        return;
    case BG_IDLE:
        if (!time_reached(bg_when))
            return;
        bg_tries = 0;
        bg_dns = 0;
        cyw43_arch_lwip_begin();
        err_t e = dns_gethostbyname(BG_SERVER, &bg_addr, bg_on_dns, NULL);
        cyw43_arch_lwip_end();
        if (e == ERR_OK) {
            bg_send();
        } else if (e == ERR_INPROGRESS) {
            bg_state = BG_LOOKUP;
            bg_when = make_timeout_time_ms(BG_WAIT_MS * 3);
        } else {
            bg_done(synced ? BG_EVERY_MS : BG_RETRY_MS);
        }
        return;
    case BG_LOOKUP:
        if (bg_dns > 0)
            bg_send();
        else if (bg_dns < 0 || time_reached(bg_when))
            bg_done(synced ? BG_EVERY_MS : BG_RETRY_MS);
        return;
    case BG_ASKED:
        if (bg_got) {
            const uint8_t *r = bg_reply;
            int stratum = r[1];
            if ((r[0] & 7) != 4 || memcmp(r + 24, bg_stamp, 8)) {
                bg_got = false;  // not ours: keep waiting
                return;
            }
            if ((r[0] >> 6) == 3 || stratum == 0 || stratum > 15) {  // its clock isn't set
                bg_done(BG_RETRY_MS);
                return;
            }
            int64_t t2 = ntp_to_us(r + 32), t3 = ntp_to_us(r + 40);
            int64_t t4 = bg_t1 + (int64_t)(bg_rx_us - bg_t1_us);
            int64_t now = wall_us() + ((t2 - bg_t1) + (t3 - t4)) / 2;
            struct timeval tv = {.tv_sec = (time_t)(now / 1000000),
                                 .tv_usec = (suseconds_t)(now % 1000000)};
            settimeofday(&tv, NULL);
            synced = true;
            bg_done(BG_EVERY_MS);
        } else if (time_reached(bg_when)) {
            if (bg_tries < BG_TRIES)
                bg_send();  // lost: ask again
            else
                bg_done(synced ? BG_EVERY_MS : BG_RETRY_MS);
        }
        return;
    }
}
