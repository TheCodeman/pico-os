/**
 * @file net_conn.c
 * @brief TCP client connections over lwIP's raw API.
 *
 * lwIP's callbacks run in its interrupt and only copy received data into a
 * ring buffer; the command reading it runs normally and tells lwIP to reopen
 * the window as it reads, so a slow reader (e.g. writing to the flash drive)
 * just slows the sender.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "net.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

#define RING_SIZE      (16 * 1024)  ///< more than TCP_WND, so a delivery always fits
#define DNS_TIMEOUT_MS 10000
#define CONN_TIMEOUT_MS 15000

enum { ST_CONNECTING, ST_OPEN, ST_CLOSED, ST_FAILED };

struct net_conn {
    struct tcp_pcb *pcb;
    volatile int state;
    volatile err_t err;
    volatile size_t head, tail;  ///< ring: lwIP writes at head, we read at tail
    uint8_t ring[RING_SIZE];
};

bool net_stop_key(void) {
    int c = getchar_timeout_us(0);
    return c == 3 || c == 0x1b;
}

// ---- lwIP callbacks (interrupt) ---------------------------------------------

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    net_conn_t *c = arg;
    if (!p) {
        c->state = ST_CLOSED;
        return ERR_OK;
    }
    size_t head = c->head;
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *src = q->payload;
        for (u16_t i = 0; i < q->len; ++i) {
            c->ring[head] = src[i];
            head = (head + 1) % RING_SIZE;
        }
    }
    c->head = head;
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    net_conn_t *c = arg;
    c->pcb = NULL;  // lwIP has already freed it
    c->err = err;
    c->state = c->state == ST_OPEN ? ST_CLOSED : ST_FAILED;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    net_conn_t *c = arg;
    c->state = ST_OPEN;
    return ERR_OK;
}

static volatile int dns_done;  ///< 0 waiting, 1 found, -1 not found
static ip_addr_t dns_addr;

static void on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    if (addr)
        dns_addr = *addr;
    dns_done = addr ? 1 : -1;
}

// ---- Commands' side ---------------------------------------------------------

bool net_resolve(const char *host, ip_addr_t *addr) {
    if (ipaddr_aton(host, addr))
        return true;
    dns_done = 0;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &dns_addr, on_dns, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) {
        *addr = dns_addr;
        return true;
    }
    if (e != ERR_INPROGRESS) {
        printf("Can't look up %s\n", host);
        return false;
    }
    absolute_time_t until = make_timeout_time_ms(DNS_TIMEOUT_MS);
    while (!dns_done) {
        bool late = time_reached(until);
        if (late || net_stop_key()) {
            printf("Lookup of %s %s\n", host, late ? "timed out" : "stopped");
            dns_done = -1;  // a late answer is ignored
            return false;
        }
        sleep_ms(5);
    }
    if (dns_done < 0) {
        printf("Unknown host %s\n", host);
        return false;
    }
    *addr = dns_addr;
    return true;
}

static void abort_pcb(net_conn_t *c) {
    cyw43_arch_lwip_begin();
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        tcp_abort(c->pcb);
        c->pcb = NULL;
    }
    cyw43_arch_lwip_end();
}

net_conn_t *net_conn_open(const char *host, uint16_t port) {
    ip_addr_t addr;
    if (!net_resolve(host, &addr))
        return NULL;
    // In PSRAM: its 16 KB ring is too much for what SRAM has left.
    net_conn_t *c = psram_buffer_alloc(sizeof *c);
    if (c)
        memset(c, 0, sizeof *c);
    if (!c) {
        printf("Out of memory\n");
        return NULL;
    }
    printf("Connecting to %s (%s) port %u...\n", host, ipaddr_ntoa(&addr), port);
    cyw43_arch_lwip_begin();
    c->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    err_t e = ERR_MEM;
    if (c->pcb) {
        tcp_arg(c->pcb, c);
        tcp_recv(c->pcb, on_recv);
        tcp_err(c->pcb, on_err);
        tcp_nagle_disable(c->pcb);  // telnet keystrokes go out at once
        e = tcp_connect(c->pcb, &addr, port, on_connected);
    }
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        printf("Can't connect: %s\n", lwip_strerr(e));
        abort_pcb(c);
        psram_buffer_free(c);
        return NULL;
    }
    absolute_time_t until = make_timeout_time_ms(CONN_TIMEOUT_MS);
    while (c->state == ST_CONNECTING) {
        bool late = time_reached(until);
        if (late || net_stop_key()) {
            printf(late ? "Connection timed out\n" : "Stopped\n");
            abort_pcb(c);
            psram_buffer_free(c);
            return NULL;
        }
        sleep_ms(5);
    }
    if (c->state != ST_OPEN) {
        printf("Connection failed: %s\n",
               c->err == ERR_RST ? "refused" : lwip_strerr(c->err));
        abort_pcb(c);
        psram_buffer_free(c);
        return NULL;
    }
    return c;
}

int net_conn_read(net_conn_t *c, void *buf, size_t max) {
    size_t head = c->head, n = 0;
    uint8_t *out = buf;
    while (c->tail != head && n < max) {
        out[n++] = c->ring[c->tail];
        c->tail = (c->tail + 1) % RING_SIZE;
    }
    if (n) {
        // Let the sender have that much window back.
        cyw43_arch_lwip_begin();
        if (c->pcb)
            tcp_recved(c->pcb, n);
        cyw43_arch_lwip_end();
        return n;
    }
    return c->state == ST_OPEN ? 0 : -1;
}

bool net_conn_write(net_conn_t *c, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        if (c->state != ST_OPEN)
            return false;
        cyw43_arch_lwip_begin();
        size_t room = c->pcb ? tcp_sndbuf(c->pcb) : 0;
        size_t n = len < room ? len : room;
        err_t e = ERR_OK;
        if (n) {
            e = tcp_write(c->pcb, p, n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK)
                tcp_output(c->pcb);
        }
        cyw43_arch_lwip_end();
        if (e == ERR_OK && n) {
            p += n;
            len -= n;
        } else {
            if (e != ERR_OK && e != ERR_MEM)
                return false;
            sleep_ms(2);  // send buffer full: wait for acks
        }
    }
    return true;
}

void net_conn_close(net_conn_t *c) {
    if (!c)
        return;
    cyw43_arch_lwip_begin();
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (tcp_close(c->pcb) != ERR_OK)
            tcp_abort(c->pcb);
        c->pcb = NULL;
    }
    cyw43_arch_lwip_end();
    psram_buffer_free(c);
}
