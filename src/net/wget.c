/**
 * @file wget.c
 * @brief wget: downloads a file over HTTP or HTTPS to the flash drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "net.h"
#include "pico/stdlib.h"
#include "shell.h"

#define MAX_REDIRECTS 5
#define HEADER_MAX    8192  ///< redirects from big sites carry long headers
#define CHUNK         16384 ///< the drive writes big blocks much faster
#define URL_MAX       1024
#define IDLE_TIMEOUT_MS 30000

typedef struct {
    bool https;
    char host[128];
    uint16_t port;
    char path[URL_MAX];
} url_t;

/// A connection, encrypted or not.
typedef struct {
    net_conn_t *conn;
    net_tls_t *tls;
} stream_t;

static int s_read(stream_t *s, void *buf, size_t max) {
    return s->tls ? net_tls_read(s->tls, buf, max) : net_conn_read(s->conn, buf, max);
}

static bool s_write(stream_t *s, const void *data, size_t len) {
    return s->tls ? net_tls_write(s->tls, data, len) : net_conn_write(s->conn, data, len);
}

static bool s_open(stream_t *s, const url_t *u, bool verify) {
    s->tls = NULL;
    if (u->https && verify && !net_tls_prepare())
        return false;
    s->conn = net_conn_open(u->host, u->port);
    if (!s->conn)
        return false;
    if (u->https) {
        s->tls = net_tls_start(s->conn, u->host, verify);
        if (!s->tls) {
            net_conn_close(s->conn);
            return false;
        }
    }
    return true;
}

static void s_close(stream_t *s) {
    net_tls_end(s->tls);
    net_conn_close(s->conn);
}

/// Parses [http[s]://]host[:port][/path]. A redirect's Location may also be
/// "//host/path", "/path" or a path relative to `base`.
static bool parse_url(const char *s, url_t *u, const url_t *base) {
    if (base && s[0] == '/' && s[1] != '/') {
        *u = *base;
        snprintf(u->path, sizeof u->path, "%s", s);
        return true;
    }
    if (base && !strstr(s, "://") && s[0] != '/') {
        *u = *base;
        char *slash = strrchr(u->path, '/');
        size_t keep = slash ? (size_t)(slash - u->path + 1) : 0;
        snprintf(u->path + keep, sizeof u->path - keep, "%s", s);
        return true;
    }
    u->https = base ? base->https : false;
    if (strncasecmp(s, "https://", 8) == 0) {
        u->https = true;
        s += 8;
    } else if (strncasecmp(s, "http://", 7) == 0) {
        u->https = false;
        s += 7;
    } else if (s[0] == '/' && s[1] == '/') {
        s += 2;
    } else if (strstr(s, "://")) {
        printf("Only http:// and https:// are supported\n");
        return false;
    }
    size_t n = strcspn(s, ":/?");
    if (n == 0 || n >= sizeof u->host) {
        printf("Bad URL\n");
        return false;
    }
    memcpy(u->host, s, n);
    u->host[n] = 0;
    s += n;
    u->port = u->https ? 443 : 80;
    if (*s == ':') {
        u->port = (uint16_t)atoi(s + 1);
        s += strcspn(s, "/?");
    }
    if (*s == '?')
        snprintf(u->path, sizeof u->path, "/%s", s);
    else
        snprintf(u->path, sizeof u->path, "%s", *s ? s : "/");
    return true;
}

/// The file name to save as: the last part of the path, without any query.
static void default_name(const char *path, char *out, size_t size) {
    const char *end = path + strcspn(path, "?#");
    const char *start = end;
    while (start > path && start[-1] != '/')
        --start;
    if (start == end)
        snprintf(out, size, "index.html");
    else
        snprintf(out, size, "%.*s", (int)(end - start), start);
}

/// Reads the response header into `hdr`. Returns the number of body bytes
/// read past it, or -1.
static int read_header(stream_t *s, char *hdr, int *hdr_len) {
    int len = 0;
    absolute_time_t idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
    for (;;) {
        if (net_stop_key()) {
            printf("Stopped\n");
            return -1;
        }
        int n = s_read(s, hdr + len, HEADER_MAX - 1 - len);
        if (n < 0) {
            printf("Connection closed before the reply\n");
            return -1;
        }
        if (n == 0) {
            if (time_reached(idle)) {
                printf("No reply\n");
                return -1;
            }
            sleep_ms(2);
            continue;
        }
        idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
        len += n;
        hdr[len] = 0;
        char *end = strstr(hdr, "\r\n\r\n");
        if (end) {
            *hdr_len = end + 4 - hdr;
            return len - *hdr_len;
        }
        if (len >= HEADER_MAX - 1) {
            printf("Reply header too long\n");
            return -1;
        }
    }
}

/// Finds header `name` (case-insensitive) and copies its value.
static bool header(const char *hdr, const char *name, char *out, size_t size) {
    size_t n = strlen(name);
    for (const char *p = strstr(hdr, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
            p += n + 1;
            while (*p == ' ')
                ++p;
            size_t len = strcspn(p, "\r\n");
            if (len >= size)
                len = size - 1;
            memcpy(out, p, len);
            out[len] = 0;
            return true;
        }
    }
    return false;
}

static void progress(uint32_t got, int32_t total, uint64_t start_us) {
    uint32_t ms = (uint32_t)((time_us_64() - start_us) / 1000);
    unsigned rate = ms ? (unsigned)((uint64_t)got * 1000 / 1024 / ms) : 0;
    if (total > 0)
        printf("\r%lu of %ld KB (%d%%)  %u KB/s   ", (unsigned long)(got / 1024),
               (long)(total / 1024), (int)((uint64_t)got * 100 / total), rate);
    else
        printf("\r%lu KB  %u KB/s   ", (unsigned long)(got / 1024), rate);
    fflush(stdout);
}

/// Saves the body of the reply. Returns true if all of it arrived.
static bool save_body(stream_t *s, const char *name, char *buf, int have, int32_t total) {
    FIL f;
    FRESULT r = f_open(&f, name, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        printf("%s: %s\n", name, fresult_name(r));
        return false;
    }
    uint32_t got = 0;
    uint64_t start = time_us_64(), shown = 0;
    absolute_time_t idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
    bool ok = true, done = false;
    while (!done) {
        // Fill the buffer before writing: the drive likes big writes.
        while (have < CHUNK) {
            int n = s_read(s, buf + have, CHUNK - have);
            if (n < 0) {
                done = true;
                break;
            }
            if (n == 0)
                break;
            have += n;
            idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
        }
        if (have) {
            UINT w;
            r = f_write(&f, buf, have, &w);
            if (r != FR_OK || w != (UINT)have) {
                printf("\nWrite failed: %s\n", r != FR_OK ? fresult_name(r) : "drive full");
                ok = false;
                break;
            }
            got += have;
            have = 0;
        } else if (!done) {
            if (time_reached(idle)) {
                printf("\nTimed out\n");
                ok = false;
                break;
            }
            sleep_ms(2);
        }
        if (net_stop_key()) {
            printf("\nStopped\n");
            ok = false;
            break;
        }
        if (time_us_64() - shown > 250000 || done) {
            progress(got, total, start);
            shown = time_us_64();
        }
        if (total > 0 && got >= (uint32_t)total)
            done = true;
    }
    f_close(&f);
    if (ok && total > 0 && got < (uint32_t)total) {
        printf("\nConnection closed early\n");
        ok = false;
    }
    if (!ok) {
        f_unlink(name);
        return false;
    }
    printf("\nSaved %lu bytes to %s\n", (unsigned long)got, name);
    return true;
}

static void usage(void) {
    printf("usage: wget [-k] URL [file]   http:// or https://\n"
           "       -k  don't check the server's certificate\n");
}

void net_cmd_wget(int argc, char **argv) {
    bool verify = true;
    const char *arg[2] = {NULL, NULL};
    int nargs = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-k") == 0)
            verify = false;
        else if (argv[i][0] == '-' || nargs == 2) {
            usage();
            return;
        } else {
            arg[nargs++] = argv[i];
        }
    }
    if (!nargs) {
        usage();
        return;
    }
    if (!shell_drive_mounted()) {
        printf("No USB drive. Plug one into the hub.\n");
        return;
    }
    if (!net_need_link())
        return;
    url_t *url = malloc(2 * sizeof(url_t));
    char *buf = malloc(HEADER_MAX > CHUNK ? HEADER_MAX : CHUNK);
    char *value = malloc(URL_MAX);
    char *req = malloc(URL_MAX + 256);
    if (!url || !buf || !value || !req) {
        printf("Out of memory\n");
        goto out;
    }
    url_t *next = url + 1;
    if (!parse_url(arg[0], url, NULL))
        goto out;
    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        stream_t s;
        if (!s_open(&s, url, verify))
            break;
        int n = snprintf(req, URL_MAX + 256,
                         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pico-os-wget/1.1\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n",
                         url->path, url->host);
        int hdr_len;
        int body = -1;
        if (s_write(&s, req, n))
            body = read_header(&s, buf, &hdr_len);
        else
            printf("Send failed\n");
        if (body < 0) {
            s_close(&s);
            break;
        }
        int code = 0;
        sscanf(buf, "HTTP/%*s %d", &code);
        if (code >= 301 && code <= 308 && code != 304 && header(buf, "Location", value, URL_MAX)) {
            printf("HTTP %d, redirected to %s\n", code, value);
            s_close(&s);
            if (hop == MAX_REDIRECTS) {
                printf("Too many redirects\n");
                break;
            }
            if (!parse_url(value, next, url))
                break;
            *url = *next;
            continue;
        }
        if (code != 200) {
            char *eol = strstr(buf, "\r\n");
            printf("Server said: %.*s\n", eol ? (int)(eol - buf) : 40, buf);
            s_close(&s);
            break;
        }
        int32_t total = header(buf, "Content-Length", value, URL_MAX) ? atol(value) : -1;
        char name[96];
        if (arg[1])
            snprintf(name, sizeof name, "%s", arg[1]);
        else
            default_name(url->path, name, sizeof name);
        printf("Saving to %s", name);
        if (total >= 0)
            printf(" (%ld bytes)", (long)total);
        printf(", Esc to stop\n");
        memmove(buf, buf + hdr_len, body);  // body bytes that came with the header
        save_body(&s, name, buf, body, total);
        s_close(&s);
        break;
    }
out:
    free(req);
    free(value);
    free(buf);
    free(url);
}
