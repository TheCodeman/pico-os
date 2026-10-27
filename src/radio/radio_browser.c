/**
 * @file radio_browser.c
 * @brief Station search through radio-browser.info's API (plain HTTP, JSON back): MP3 and AAC stations (it can't ask for both at once, so the others are left out here, and HLS ones, which don't play).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psram_heap.h"
#include "radio.h"

/// The most of a server reply kept.
#define REPLY_MAX (160 * 1024)

/// The servers (any of them will do; tried in turn).
static const char *const servers[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info",
                                      "fi1.api.radio-browser.info", "all.api.radio-browser.info"};

/// URL-encodes a string.
/// @param in the text
/// @param out the encoded text
/// @param size room in out
static void url_escape(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 4 < size; ++in) {
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.')
            out[n++] = c;
        else
            n += snprintf(out + n, size - n, "%%%02X", c);
    }
    out[n] = 0;
}

/// The string value of "key" in a JSON object's text (from p to end), with
/// escapes undone (non-ASCII becomes '?'). False if it isn't there.
/// @param p the object
/// @param end its end
/// @param key the key
/// @param out the value
/// @param size room in out
/// @return found
static bool json_string(const char *p, const char *end, const char *key, char *out, size_t size) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t pl = strlen(pat);
    for (const char *q = p; q + pl < end; ++q) {
        if (memcmp(q, pat, pl))
            continue;
        q += pl;
        while (q < end && (*q == ' ' || *q == ':'))
            ++q;
        size_t n = 0;
        if (q < end && *q == '"') {
            for (++q; q < end && *q != '"'; ++q) {
                char c = *q;
                if (c == '\\' && q + 1 < end) {
                    c = *++q;
                    if (c == 'u') {
                        q += 4;
                        c = '?';
                    } else if (c == 'n' || c == 't' || c == 'r') {
                        c = ' ';
                    }
                } else if ((unsigned char)c >= 0x80) {
                    // UTF-8: one '?' per character.
                    if (((unsigned char)c & 0xc0) == 0x80)
                        continue;
                    c = '?';
                }
                if (n + 1 < size)
                    out[n++] = c;
            }
        } else {  // a number
            while (q < end && (isdigit((unsigned char)*q) || *q == '-') && n + 1 < size)
                out[n++] = *q++;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

int radio_search(const char *what, radio_station_t *out, int max, const char **err) {
    char field[16] = "name", value[96], esc[200], url[400];
    const char *colon = strchr(what, ':');
    if (colon && colon - what < 10) {
        int n = colon - what;
        if (!strncasecmp(what, "tag", n))
            strcpy(field, "tag");
        else if (!strncasecmp(what, "country", n))
            strcpy(field, "countrycode");
        else if (!strncasecmp(what, "lang", n) || !strncasecmp(what, "language", n))
            strcpy(field, "language");
        else
            colon = NULL;
        if (colon)
            what = colon + 1;
    }
    while (*what == ' ')
        ++what;
    snprintf(value, sizeof value, "%s", what);
    url_escape(value, esc, sizeof esc);
    char *reply = psram_malloc(REPLY_MAX);
    if (!reply) {
        *err = "not enough memory";
        return -1;
    }
    int len = -1;
    for (unsigned s = 0; s < sizeof servers / sizeof *servers && len < 0; ++s) {
        snprintf(url, sizeof url,
                 "http://%s/json/stations/search?%s=%s&hidebroken=true&order=clickcount"
                 "&reverse=true&limit=%d",
                 servers[s], field, esc, max);
        len = radio_http_get(url, reply, REPLY_MAX, err);
        if (len < 0 && !strcmp(*err, "stopped"))
            break;
    }
    if (len < 0) {
        psram_free(reply);
        return -1;
    }
    // [ {...}, {...} ]: each object's fields. (Objects don't nest here.)
    int n = 0;
    const char *end = reply + len;
    for (const char *p = reply; p < end && n < max;) {
        const char *o = memchr(p, '{', end - p);
        if (!o)
            break;
        const char *c = memchr(o, '}', end - o);
        if (!c)
            break;
        radio_station_t *st = &out[n];
        memset(st, 0, sizeof *st);
        char num[16];
        json_string(o, c, "name", st->name, sizeof st->name);
        if (!json_string(o, c, "url_resolved", st->url, sizeof st->url) || !st->url[0])
            json_string(o, c, "url", st->url, sizeof st->url);
        if (json_string(o, c, "bitrate", num, sizeof num))
            st->bitrate = (uint16_t)atoi(num);
        json_string(o, c, "countrycode", st->country, sizeof st->country);
        json_string(o, c, "codec", st->codec, sizeof st->codec);
        char hls[4] = "";
        json_string(o, c, "hls", hls, sizeof hls);
        // MP3, AAC or AAC+ (HE-AAC); not HLS, Ogg, Opus, FLAC...
        bool ok = (!strcasecmp(st->codec, "MP3") || !strcasecmp(st->codec, "AAC") ||
                   !strcasecmp(st->codec, "AAC+")) &&
                  strcmp(hls, "1") && !strstr(st->url, ".m3u8");
        // Names often have spaces around them.
        char *nm = st->name;
        while (*nm == ' ')
            memmove(nm, nm + 1, strlen(nm));
        if (ok && st->url[0] && st->name[0])
            ++n;
        p = c + 1;
    }
    psram_free(reply);
    if (!n)
        *err = "no MP3 or AAC stations found";
    return n;
}
