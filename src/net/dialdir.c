/**
 * @file dialdir.c
 * @brief Reading a BBS dialing directory (see dialdir.h).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "dialdir.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/// The value of attribute `key` in the tag from p to end, with XML's entities
/// undone; false if it isn't there.
static bool attr(const char *p, const char *end, const char *key, char *out, size_t size) {
    size_t kl = strlen(key);
    for (const char *q = p; q + kl + 2 < end; ++q) {
        if (memcmp(q, key, kl) || q[kl] != '=' || (q[kl + 1] != '"' && q[kl + 1] != '\''))
            continue;
        if (q > p && !isspace((unsigned char)q[-1]))
            continue;  // "ip" inside another name
        char quote = q[kl + 1];
        size_t n = 0;
        for (q += kl + 2; q < end && *q != quote; ++q) {
            char c = *q;
            if (c == '&') {
                static const struct { const char *e; char c; } ents[] = {
                    ///@{
                    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
                    ///@}
                for (unsigned i = 0; i < sizeof ents / sizeof *ents; ++i) {
                    size_t el = strlen(ents[i].e);
                    if (q + el <= end && !memcmp(q, ents[i].e, el)) {
                        c = ents[i].c;
                        q += el - 1;
                        break;
                    }
                }
            } else if (c == '\t' || c == '\r' || c == '\n') {
                c = ' ';
            }
            if (n + 1 < size)
                out[n++] = c;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

/// Copies s into out (size bytes), cut to fit.
static void copy(char *out, size_t size, const char *s) {
    size_t n = strlen(s);
    if (n >= size)
        n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ')
        ++p;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
}

int dialdir_parse(const char *text, dial_entry_t *out, int max, int *skipped) {
    int n = 0, skip = 0;
    for (const char *p = text; (p = strstr(p, "<BBS")) != NULL;) {
        const char *end = strchr(p, '>');
        if (!end)
            break;
        char proto[16] = "TELNET", ip[96] = "", port[12] = "";
        dial_entry_t e;
        memset(&e, 0, sizeof e);
        attr(p, end, "name", e.name, sizeof e.name);
        attr(p, end, "ip", ip, sizeof ip);
        attr(p, end, "port", port, sizeof port);
        attr(p, end, "protocol", proto, sizeof proto);
        p = end + 1;
        trim(e.name);
        trim(ip);
        bool telnet = !strcasecmp(proto, "telnet"), ssh = !strcasecmp(proto, "ssh");
        // "user@host" (or "user:password@host": the password is left out).
        char *at = strrchr(ip, '@'), *host = ip;
        if (at) {
            *at = 0;
            host = at + 1;
            ip[strcspn(ip, ":")] = 0;
            copy(e.user, sizeof e.user, ip);
        }
        long pn = port[0] ? strtol(port, NULL, 10) : (ssh ? 22 : 23);
        if ((!telnet && !ssh) || !host[0] || strchr(host, ' ') || strlen(host) >= sizeof e.host ||
            pn <= 0 || pn > 65535) {
            ++skip;
            continue;
        }
        strcpy(e.host, host);
        e.port = (uint16_t)pn;
        e.ssh = ssh;
        if (!e.name[0])
            copy(e.name, sizeof e.name, e.host);
        if (out) {
            if (n >= max)
                break;
            out[n] = e;
        }
        ++n;
    }
    if (skipped)
        *skipped = skip;
    return n;
}
