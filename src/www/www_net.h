/**
 * @file www_net.h
 * @brief The web browser's fetcher: http:// and https:// (over wget's network
 *        and TLS code, net.h), and file:// pages on the drive.
 *
 * Progress and errors are printed (the browser shows them on its status
 * line). Cookies a site sets are kept until the browser closes.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "www_html.h"

/// The biggest page or file the browser loads (in PSRAM).
#define WWW_PAGE_MAX (4u << 20)

/// What www_fetch got.
typedef struct {
    char *body;               ///< the page (PSRAM, 0-terminated), or NULL
    uint32_t len;             ///< its length
    int code;                 ///< the HTTP status (200 for a file)
    char url[WWW_URL_MAX];    ///< where it came from, after any redirects
    char type[64];            ///< its Content-Type, lower case, without parameters
    bool latin1;              ///< its charset is ISO-8859-1 or Windows-1252
    bool cut;                 ///< stopped with Esc, or too big: only the start
} www_reply_t;

/// What became of a fetch.
enum {
    WF_OK,    ///< got it (r->body is set)
    WF_FAIL,  ///< failed (and printed why)
    WF_CERT,  ///< the certificate couldn't be checked (no CACERT.PEM, or untrusted)
};

/// Fetches a URL: GET, or POST with `post` (form data) if it isn't NULL.
/// Redirects are followed.
/// @param url the URL (http://, https:// or file://)
/// @param post form data to POST, or NULL
/// @param verify check https certificates against CACERT.PEM
/// @param r what was got (free r->body with psram_free)
/// @return WF_
int www_fetch(const char *url, const char *post, bool verify, www_reply_t *r);

/// Forgets the cookies.
void www_net_end(void);

/// Is the type text the browser shows (HTML or plain text)?
/// @param type a Content-Type
/// @param html set if it is HTML
/// @return it is shown
bool www_is_text(const char *type, bool *html);
