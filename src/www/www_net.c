/**
 * @file www_net.c
 * @brief The web browser's fetcher: HTTP/1.0 GET and POST over net.h's TCP
 *        and TLS (as wget does), redirects, cookies, and file:// pages.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// URLs are cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "www_net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "version.h"

/// Redirects followed.
#define MAX_REDIRECTS 8
/// The longest reply header (big sites send long ones).
#define HEADER_MAX 16384
/// How long the server may stay quiet.
#define IDLE_TIMEOUT_MS 30000
/// Cookies kept.
#define MAX_COOKIES 48
/// The first room for a page; it doubles as needed.
#define FIRST_ROOM (64u << 10)
/// Who we say we are: lynx's name gets simple pages from many sites.
#define USER_AGENT "Lynx/2.9.0 (pico-os www/" VER_WWW ") libwww-FM/2.14"

/// A URL's parts.
typedef struct {
    bool https;               ///< https
    char host[128];           ///< the host
    uint16_t port;            ///< the port
    char path[WWW_URL_MAX];   ///< the path and query, from the '/'
} url_t;

/// A connection, encrypted or not.
typedef struct {
    net_conn_t *conn;  ///< the TCP connection
    net_tls_t *tls;    ///< the TLS session, or NULL
} stream_t;

/// A cookie.
typedef struct {
    char domain[64];  ///< the host, or the domain it is for
    bool host_only;   ///< only that host (no Domain= given)
    char path[64];    ///< the paths it is for
    char nv[256];     ///< "name=value"
} cookie_t;

/// The cookies (PSRAM, made on the first one).
static cookie_t *jar;
/// How many.
static int njar;

/// A fetch's buffers (PSRAM: the stack is small).
typedef struct {
    url_t url;                    ///< where
    url_t next;                   ///< a redirect
    char hdr[HEADER_MAX];         ///< the reply's header
    char req[WWW_URL_MAX + 2048]; ///< the request
    char value[WWW_URL_MAX];      ///< a header's value
    char abs[WWW_URL_MAX];        ///< a redirect made absolute
    char cur[WWW_URL_MAX];        ///< the URL being fetched
} fetch_t;

// ---- Streams ----------------------------------------------------------------------------------------

/// Reads.
/// @param s the stream
/// @param buf where
/// @param max how many
/// @return what net_tls_read or net_conn_read returns
static int s_read(stream_t *s, void *buf, size_t max) {
    return s->tls ? net_tls_read(s->tls, buf, max) : net_conn_read(s->conn, buf, max);
}

/// Writes.
/// @param s the stream
/// @param data the bytes
/// @param len how many
/// @return written
static bool s_write(stream_t *s, const void *data, size_t len) {
    return s->tls ? net_tls_write(s->tls, data, len) : net_conn_write(s->conn, data, len);
}

/// Closes.
/// @param s the stream
static void s_close(stream_t *s) {
    net_tls_end(s->tls);
    net_conn_close(s->conn);
}

/// Parses http[s]://host[:port][/path].
/// @param s the URL
/// @param u the parts
/// @return parsed
static bool parse_url(const char *s, url_t *u) {
    if (!strncasecmp(s, "https://", 8)) {
        u->https = true;
        s += 8;
    } else if (!strncasecmp(s, "http://", 7)) {
        u->https = false;
        s += 7;
    } else {
        printf("Only http, https and file URLs can be opened\n");
        return false;
    }
    size_t n = strcspn(s, ":/?#");
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
        s += strcspn(s, "/?#");
    }
    // The path as it is sent: no #fragment; spaces and 8-bit bytes escaped.
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    if (*s != '/')
        u->path[k++] = '/';
    for (; *s && *s != '#' && k + 4 < sizeof u->path; ++s) {
        uint8_t c = (uint8_t)*s;
        if (c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>') {
            u->path[k++] = '%';
            u->path[k++] = hex[c >> 4];
            u->path[k++] = hex[c & 15];
        } else {
            u->path[k++] = (char)c;
        }
    }
    u->path[k] = 0;
    return true;
}

// ---- Cookies -------------------------------------------------------------------------------------------

/// Keeps a cookie from a Set-Cookie header.
/// @param u the URL it came with
/// @param v the header's value
static void set_cookie(const url_t *u, const char *v) {
    cookie_t c;
    memset(&c, 0, sizeof c);
    size_t n = strcspn(v, ";");
    if (!memchr(v, '=', n) || n >= sizeof c.nv)
        return;
    snprintf(c.nv, sizeof c.nv, "%.*s", (int)n, v);
    snprintf(c.domain, sizeof c.domain, "%s", u->host);
    c.host_only = true;
    snprintf(c.path, sizeof c.path, "/");
    bool gone = false;
    for (const char *a = v + n; *a == ';';) {
        ++a;
        while (*a == ' ')
            ++a;
        size_t an = strcspn(a, ";");
        if (!strncasecmp(a, "domain=", 7) && an > 7) {
            const char *d = a + 7 + (a[7] == '.');
            int dn = (int)(an - 7 - (a[7] == '.'));
            size_t hn = strlen(u->host);
            // The host must be in that domain.
            if ((size_t)dn <= hn && !strncasecmp(u->host + hn - dn, d, dn) &&
                (hn == (size_t)dn || u->host[hn - dn - 1] == '.') && dn < (int)sizeof c.domain) {
                snprintf(c.domain, sizeof c.domain, "%.*s", dn, d);
                c.host_only = false;
            }
        } else if (!strncasecmp(a, "path=", 5) && an > 5 && an - 5 < sizeof c.path) {
            snprintf(c.path, sizeof c.path, "%.*s", (int)(an - 5), a + 5);
        } else if (!strncasecmp(a, "max-age=", 8)) {
            gone = atoi(a + 8) <= 0;
        }
        a += an;
    }
    size_t name = strcspn(c.nv, "=");
    int slot = -1;
    for (int i = 0; i < njar; ++i)
        if (!strcasecmp(jar[i].domain, c.domain) && !strcmp(jar[i].path, c.path) &&
            !strncmp(jar[i].nv, c.nv, name + 1))
            slot = i;
    if (gone) {
        if (slot >= 0)
            jar[slot] = jar[--njar];
        return;
    }
    if (!jar && !(jar = psram_malloc(MAX_COOKIES * sizeof *jar)))
        return;
    if (slot < 0) {
        if (njar == MAX_COOKIES)  // full: the oldest goes
            memmove(jar, jar + 1, --njar * sizeof *jar);
        slot = njar++;
    }
    jar[slot] = c;
}

/// The Cookie header's value for a URL ("" if there are none).
/// @param u the URL
/// @param out the value
/// @param size room in out
static void cookies_for(const url_t *u, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    size_t hn = strlen(u->host);
    for (int i = 0; i < njar; ++i) {
        const cookie_t *c = &jar[i];
        size_t dn = strlen(c->domain);
        bool host = c->host_only ? !strcasecmp(u->host, c->domain)
                                 : dn <= hn && !strcasecmp(u->host + hn - dn, c->domain) &&
                                       (hn == dn || u->host[hn - dn - 1] == '.');
        if (!host || strncmp(u->path, c->path, strlen(c->path)))
            continue;
        int k = snprintf(out + n, size - n, "%s%s", n ? "; " : "", c->nv);
        if (k < 0 || n + k >= size) {
            out[n] = 0;
            break;
        }
        n += k;
    }
}

void www_net_end(void) {
    psram_free(jar);
    jar = NULL;
    njar = 0;
}

// ---- The reply ---------------------------------------------------------------------------------------

/// Finds `what` in `s`, any case.
/// @param s the text
/// @param what what
/// @return where, or NULL
static const char *find_ci(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; ++s)
        if (!strncasecmp(s, what, n))
            return s;
    return NULL;
}

/// Finds header `name` (case-insensitive) after `from` and copies its value.
/// @param hdr the header
/// @param from where to look from (moved past it), or NULL for the start
/// @param name the field
/// @param out its value
/// @param size room in out
/// @return found
static bool header(const char *hdr, const char **from, const char *name, char *out, size_t size) {
    size_t n = strlen(name);
    for (const char *p = strstr(from && *from ? *from : hdr, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
            p += n + 1;
            while (*p == ' ' || *p == '\t')
                ++p;
            size_t len = strcspn(p, "\r\n");
            if (len >= size)
                len = size - 1;
            memcpy(out, p, len);
            out[len] = 0;
            if (from)
                *from = p;
            return true;
        }
    }
    return false;
}

/// Reads the reply's header into f->hdr. Returns the number of body bytes read
/// past it (moved to the start of `body`), or -1.
/// @param s the stream
/// @param f the buffers
/// @param body where the body's first bytes go (room for HEADER_MAX)
/// @return the body bytes, or -1
static int read_header(stream_t *s, fetch_t *f, char *body) {
    int len = 0;
    absolute_time_t idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
    for (;;) {
        if (net_stop_key()) {
            printf("Stopped\n");
            return -1;
        }
        int n = s_read(s, f->hdr + len, HEADER_MAX - 1 - len);
        if (n < 0) {
            printf("The connection closed before the reply\n");
            return -1;
        }
        if (n == 0) {
            if (time_reached(idle)) {
                printf("No reply from %s\n", f->url.host);
                return -1;
            }
            sleep_ms(2);
            continue;
        }
        idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
        len += n;
        f->hdr[len] = 0;
        char *end = strstr(f->hdr, "\r\n\r\n");
        if (end) {
            int hlen = (int)(end + 4 - f->hdr);
            memcpy(body, f->hdr + hlen, len - hlen);
            end[2] = 0;  // the header ends with its last line's \r\n
            return len - hlen;
        }
        if (len >= HEADER_MAX - 1) {
            printf("The reply's header is too long\n");
            return -1;
        }
    }
}

/// Undoes chunked transfer encoding in place.
/// @param buf the body
/// @param len its length
/// @return the length decoded
static uint32_t unchunk(char *buf, uint32_t len) {
    uint32_t in = 0, out = 0;
    while (in < len) {
        char *end;
        unsigned long n = strtoul(buf + in, &end, 16);
        char *eol = memchr(buf + in, '\n', len - in);
        if (!eol || end == buf + in || n == 0)
            break;
        in = (uint32_t)(eol + 1 - buf);
        if (n > len - in)
            n = len - in;
        memmove(buf + out, buf + in, n);
        out += (uint32_t)n;
        in += (uint32_t)n;
        while (in < len && (buf[in] == '\r' || buf[in] == '\n'))
            ++in;
    }
    buf[out] = 0;
    return out;
}

/// Reads the body into a growing PSRAM buffer.
/// @param s the stream
/// @param r the reply (body and len)
/// @param have bytes already in r->body
/// @param room r->body's size
/// @param total the Content-Length, or -1
/// @return false if nothing could be read
static bool read_body(stream_t *s, www_reply_t *r, uint32_t have, uint32_t room, int32_t total) {
    absolute_time_t idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
    uint64_t shown = 0;
    for (;;) {
        if (total >= 0 && have >= (uint32_t)total)
            break;
        if (have + 1 >= room) {
            if (room >= WWW_PAGE_MAX) {
                printf("Only the first %u MB was loaded\n", WWW_PAGE_MAX >> 20);
                r->cut = true;
                break;
            }
            char *more = psram_realloc(r->body, room * 2);
            if (!more) {
                printf("Out of memory: only the start was loaded\n");
                r->cut = true;
                break;
            }
            r->body = more;
            room *= 2;
        }
        int n = s_read(s, r->body + have, room - 1 - have);
        if (n < 0)
            break;
        if (n == 0) {
            if (time_reached(idle)) {
                printf("Timed out: only the start was loaded\n");
                r->cut = true;
                break;
            }
            if (net_stop_key()) {
                printf("Stopped: only the start was loaded\n");
                r->cut = true;
                break;
            }
            sleep_ms(2);
            continue;
        }
        have += (uint32_t)n;
        idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
        if (time_us_64() - shown > 300000) {
            shown = time_us_64();
            if (total > 0)
                printf("Loading %lu of %lu KB (Esc stops)\r", (unsigned long)(have >> 10),
                       (unsigned long)(total >> 10));
            else
                printf("Loading %lu KB (Esc stops)\r", (unsigned long)(have >> 10));
            fflush(stdout);
        }
    }
    r->body[have] = 0;
    r->len = have;
    return true;
}

/// Chunked transfer encoding undone as the bytes go by (for a file saved).
typedef struct {
    int state;      ///< 0 the size line, 1 its end, 2 data, 3 the CRLF after it, 4 the end
    uint32_t size;  ///< the chunk's size, as read
    uint32_t left;  ///< its bytes still to come
} dechunk_t;

/// Takes the chunk framing out of `n` bytes in place.
/// @param d the state
/// @param buf the bytes (the data is moved to the front)
/// @param n how many
/// @return the data bytes
static uint32_t dechunk(dechunk_t *d, char *buf, uint32_t n) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < n && d->state != 4;) {
        char c = buf[i];
        switch (d->state) {
        case 0:  // hex digits, then anything up to the newline
            if (isxdigit((unsigned char)c))
                d->size = d->size * 16 + (uint32_t)(isdigit((unsigned char)c) ? c - '0' : (c | 0x20) - 'a' + 10);
            else
                d->state = 1;
            if (d->state == 0) {
                ++i;
                break;
            }
            // fall through
        case 1:
            ++i;
            if (c == '\n') {
                d->left = d->size;
                d->state = d->size ? 2 : 4;
                d->size = 0;
            }
            break;
        case 2: {
            uint32_t k = n - i < d->left ? n - i : d->left;
            memmove(buf + out, buf + i, k);
            out += k;
            i += k;
            d->left -= k;
            if (!d->left)
                d->state = 3;
            break;
        }
        case 3:  // the CRLF after the data
            ++i;
            if (c == '\n')
                d->state = 0;
            break;
        }
    }
    return out;
}

/// Streams the body to a file, with progress; Esc stops (and deletes it).
/// @param s the stream
/// @param r the reply (len is set to the bytes saved)
/// @param path the file
/// @param buf a buffer, with the body's first bytes
/// @param have how many
/// @param room its size
/// @param chunked the body is chunked
/// @return all of it was saved
static bool save_body(stream_t *s, www_reply_t *r, const char *path, char *buf, uint32_t have,
                      uint32_t room, bool chunked) {
    FIL file;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("Can't write %s\n", path);
        return false;
    }
    dechunk_t dc = {0, 0, 0};
    uint32_t got = 0;
    uint64_t start = time_us_64(), shown = 0;
    absolute_time_t idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
    bool ok = true, done = false;
    const char *why = NULL;
    while (!done) {
        // Fill the buffer before writing: the drive likes big writes.
        while (have < room / 2) {
            int n = s_read(s, buf + have, room / 2 - have);
            if (n < 0) {
                done = true;
                break;
            }
            if (n == 0)
                break;
            have += (uint32_t)n;
            idle = make_timeout_time_ms(IDLE_TIMEOUT_MS);
        }
        uint32_t data = chunked ? dechunk(&dc, buf, have) : have;
        if (chunked && dc.state == 4)
            done = true;
        if (data) {
            UINT w;
            if (f_write(&file, buf, data, &w) != FR_OK || w != data) {
                why = "Can't write to the drive (is it full?)";
                ok = false;
                break;
            }
            got += data;
        } else if (!have && !done) {
            if (time_reached(idle)) {
                why = "Timed out";
                ok = false;
                break;
            }
            sleep_ms(2);
        }
        have = 0;
        if (net_stop_key()) {
            why = "Stopped";
            ok = false;
            break;
        }
        if (r->total >= 0 && !chunked && got >= (uint32_t)r->total)
            done = true;
        if (time_us_64() - shown > 300000 || done) {
            shown = time_us_64();
            uint32_t ms = (uint32_t)((shown - start) / 1000);
            unsigned rate = ms ? (unsigned)((uint64_t)got * 1000 / 1024 / ms) : 0;
            if (r->total > 0)
                printf("Saving %lu of %lu KB to %s, %u KB/s (Esc stops)\r", (unsigned long)(got >> 10),
                       (unsigned long)(r->total >> 10), path, rate);
            else
                printf("Saving %lu KB to %s, %u KB/s (Esc stops)\r", (unsigned long)(got >> 10), path,
                       rate);
            fflush(stdout);
        }
    }
    f_close(&file);
    if (ok && r->total > 0 && !chunked && got < (uint32_t)r->total) {
        why = "The connection closed early";
        ok = false;
    }
    if (!ok) {
        f_unlink(path);
        printf("%s: %s was deleted\n", why, path);
        return false;
    }
    r->len = got;
    r->saved = true;
    printf("Saved %lu bytes to %s\n", (unsigned long)got, path);
    return true;
}

bool www_is_text(const char *type, bool *html) {
    *html = !type[0] || strstr(type, "html") || strstr(type, "xml");
    return *html || !strncmp(type, "text/", 5) || !strcmp(type, "application/json") ||
           !strcmp(type, "application/javascript");
}

/// The type of a file on the drive, from its name.
/// @param path the file
/// @param type its Content-Type
/// @param size room in type
static void file_type(const char *path, char *type, size_t size) {
    const char *dot = strrchr(path, '.');
    static const char *const text[] = {".txt", ".bas", ".md", ".c", ".h", ".ini", ".cfg", ".log",
                                       ".csv", ".py", ".json", ".xml", ".s", ".asm", ".css", NULL};
    if (dot && (!strcasecmp(dot, ".htm") || !strcasecmp(dot, ".html"))) {
        snprintf(type, size, "text/html");
        return;
    }
    for (int i = 0; dot && text[i]; ++i)
        if (!strcasecmp(dot, text[i])) {
            snprintf(type, size, "text/plain");
            return;
        }
    snprintf(type, size, "application/octet-stream");
}

/// A file:// URL: the file read from the drive.
/// @param url the URL
/// @param r the reply
/// @return WF_
static int fetch_file(const char *url, www_reply_t *r) {
    char path[256];
    const char *p = url + 7;  // after "file://"
    size_t n = 0;
    for (; *p && *p != '?' && *p != '#' && n + 1 < sizeof path; ++p) {  // %XX undone
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hx[3] = {p[1], p[2], 0};
            path[n++] = (char)strtol(hx, NULL, 16);
            p += 2;
        } else {
            path[n++] = *p;
        }
    }
    path[n] = 0;
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        printf("Can't open %s\n", path);
        return WF_FAIL;
    }
    FSIZE_t size = f_size(&f);
    if (size > WWW_PAGE_MAX) {
        size = WWW_PAGE_MAX;
        r->cut = true;
    }
    r->body = psram_malloc((size_t)size + 1);
    UINT got = 0;
    if (!r->body || f_read(&f, r->body, (UINT)size, &got) != FR_OK) {
        printf(r->body ? "Can't read %s\n" : "Out of memory\n", path);
        f_close(&f);
        psram_free(r->body);
        r->body = NULL;
        return WF_FAIL;
    }
    f_close(&f);
    r->body[got] = 0;
    r->len = got;
    r->code = 200;
    snprintf(r->url, sizeof r->url, "%s", url);
    file_type(path, r->type, sizeof r->type);
    return WF_OK;
}

int www_fetch(const char *url, const char *post, bool verify, www_save_fn save, www_reply_t *r) {
    memset(r, 0, sizeof *r);
    r->total = -1;
    if (!strncasecmp(url, "file://", 7))
        return fetch_file(url, r);
    fetch_t *f = psram_malloc(sizeof *f);
    if (!f) {
        printf("Out of memory\n");
        return WF_FAIL;
    }
    int result = WF_FAIL;
    snprintf(f->cur, sizeof f->cur, "%s", url);
    if (!parse_url(f->cur, &f->url) || !net_need_link())
        goto out;
    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        url_t *u = &f->url;
        stream_t s = {NULL, NULL};
        if (u->https && verify && !net_tls_prepare()) {
            result = WF_CERT;
            goto out;
        }
        if (!(s.conn = net_conn_open(u->host, u->port)))
            goto out;
        if (u->https && !(s.tls = net_tls_start(s.conn, u->host, verify))) {
            net_conn_close(s.conn);
            if (verify)
                result = WF_CERT;
            goto out;
        }
        printf("Sent the request to %s; waiting for the reply\n", u->host);
        char port[8] = "";
        if (u->port != (u->https ? 443 : 80))
            snprintf(port, sizeof port, ":%u", u->port);
        cookies_for(u, f->value, sizeof f->value);
        int n = snprintf(f->req, sizeof f->req,
                         "%s %s HTTP/1.0\r\nHost: %s%s\r\nUser-Agent: " USER_AGENT "\r\n"
                         "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
                         "Accept-Language: en\r\nConnection: close\r\n",
                         post ? "POST" : "GET", u->path, u->host, port);
        if (f->value[0])
            n += snprintf(f->req + n, sizeof f->req - n, "Cookie: %s\r\n", f->value);
        if (post)
            n += snprintf(f->req + n, sizeof f->req - n,
                          "Content-Type: application/x-www-form-urlencoded\r\n"
                          "Content-Length: %u\r\n",
                          (unsigned)strlen(post));
        n += snprintf(f->req + n, sizeof f->req - n, "\r\n");
        if (n >= (int)sizeof f->req) {
            printf("The request is too long\n");
            s_close(&s);
            goto out;
        }
        r->body = psram_malloc(FIRST_ROOM > HEADER_MAX ? FIRST_ROOM : HEADER_MAX);
        int body = -1;
        if (!r->body)
            printf("Out of memory\n");
        else if (!s_write(&s, f->req, n) || (post && !s_write(&s, post, strlen(post))))
            printf("Couldn't send the request\n");
        else
            body = read_header(&s, f, r->body);
        if (body < 0) {
            s_close(&s);
            goto out;
        }
        sscanf(f->hdr, "HTTP/%*s %d", &r->code);
        for (const char *from = NULL; header(f->hdr, &from, "Set-Cookie", f->value, sizeof f->value);)
            set_cookie(u, f->value);
        if (r->code >= 301 && r->code <= 308 && r->code != 304 &&
            header(f->hdr, NULL, "Location", f->value, sizeof f->value)) {
            s_close(&s);
            psram_free(r->body);
            r->body = NULL;
            if (hop == MAX_REDIRECTS) {
                printf("Too many redirects\n");
                goto out;
            }
            www_resolve(f->cur, f->value, f->abs, sizeof f->abs);
            printf("Redirected to %s\n", f->abs);
            if (r->code != 307 && r->code != 308)
                post = NULL;  // the next request is a GET
            snprintf(f->cur, sizeof f->cur, "%s", f->abs);
            if (!parse_url(f->cur, &f->url))
                goto out;
            continue;
        }
        snprintf(r->url, sizeof r->url, "%s", f->cur);
        if (header(f->hdr, NULL, "Content-Type", f->value, sizeof f->value)) {
            const char *cs = find_ci(f->value, "charset=");
            r->latin1 = cs && (find_ci(cs, "8859") || find_ci(cs, "1252") ||
                               find_ci(cs, "latin"));
            size_t k = strcspn(f->value, "; ");
            for (size_t i = 0; i < k && i + 1 < sizeof r->type; ++i)
                r->type[i] = (char)tolower((unsigned char)f->value[i]);
        }
        int32_t total = header(f->hdr, NULL, "Content-Length", f->value, sizeof f->value)
                            ? atol(f->value) : -1;
        r->total = total;
        if (header(f->hdr, NULL, "Content-Disposition", f->value, sizeof f->value)) {
            const char *fn = find_ci(f->value, "filename=");
            if (fn) {  // filename="x.zip" or filename=x.zip: the last part of it
                fn += 9 + (fn[9] == '"');
                size_t k = strcspn(fn, "\";");
                const char *slash = fn;
                for (size_t i = 0; i < k; ++i)
                    if (fn[i] == '/' || fn[i] == '\\')
                        slash = fn + i + 1;
                k -= (size_t)(slash - fn);
                snprintf(r->name, sizeof r->name, "%.*s", (int)k, slash);
            }
        }
        bool chunked = header(f->hdr, NULL, "Transfer-Encoding", f->value, sizeof f->value) &&
                       find_ci(f->value, "chunked");
        if (header(f->hdr, NULL, "Content-Encoding", f->value, sizeof f->value) &&
            strcasecmp(f->value, "identity")) {
            printf("The server sent it compressed (%s), which can't be read\n", f->value);
            s_close(&s);
            goto out;
        }
        uint32_t room = FIRST_ROOM > HEADER_MAX ? FIRST_ROOM : HEADER_MAX;
        int what = save ? save(r, f->abs, sizeof f->abs) : WS_MEMORY;
        if (what == WS_CANCEL) {
            s_close(&s);
            goto out;
        }
        if (what == WS_SAVE) {
            bool ok = save_body(&s, r, f->abs, r->body, (uint32_t)body, room, chunked);
            s_close(&s);
            psram_free(r->body);
            r->body = NULL;
            result = ok ? WF_OK : WF_FAIL;
            break;
        }
        read_body(&s, r, (uint32_t)body, room, chunked ? -1 : total);
        s_close(&s);
        if (chunked)
            r->len = unchunk(r->body, r->len);
        result = WF_OK;
        break;
    }
out:
    if (result != WF_OK) {
        psram_free(r->body);
        r->body = NULL;
    }
    psram_free(f);
    return result;
}
