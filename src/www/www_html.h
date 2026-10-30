/**
 * @file www_html.h
 * @brief The web browser's page engine: HTML (or plain text) laid out as lines
 *        of 78 code page 437 characters, with links, form fields and anchors.
 *
 * Nothing here touches the hardware, so it also runs on a PC (tools/wwwtest).
 * All of a page lives in PSRAM, in arrays that grow as the page is read.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The page's width in characters (the screen less a column each side).
#define WWW_COLS 78
/// The longest URL kept.
#define WWW_URL_MAX 1024

/// How a character is shown.
enum {
    WS_TEXT,    ///< plain text
    WS_BOLD,    ///< <b>, <strong>, <th>
    WS_ITALIC,  ///< <i>, <em>
    WS_CODE,    ///< <code>, <pre>, <tt>
    WS_H1,      ///< <h1>
    WS_H2,      ///< <h2> to <h6>
    WS_IMG,     ///< an image's [alt text]
    WS_FIELD,   ///< a form field
    WS_RULE,    ///< <hr>, list bullets
    WS_QUOTE,   ///< <blockquote>
    WS_COUNT,   ///< how many styles there are
};

/// What a link is.
enum {
    WL_HREF,      ///< <a href>, <iframe src>, a meta refresh
    WL_TEXT,      ///< <input type=text> and the like
    WL_PASSWORD,  ///< <input type=password>
    WL_TEXTAREA,  ///< <textarea>, edited as one line
    WL_CHECK,     ///< <input type=checkbox>
    WL_RADIO,     ///< <input type=radio>
    WL_SELECT,    ///< <select>
    WL_SUBMIT,    ///< <input type=submit|image>, <button>
    WL_HIDDEN,    ///< <input type=hidden>: sent, never shown
    WL_IMAGE,     ///< an <img> with alt text, not in a link: href is its src
};

/// A character on the page.
typedef struct {
    uint8_t ch;  ///< the code page 437 character
    uint8_t st;  ///< its style (WS_)
} www_cell_t;

/// A line: its cells in the page's cell array.
typedef struct {
    uint32_t cell;  ///< the first
    uint16_t len;   ///< how many (trailing spaces are dropped)
} www_line_t;

/// A run of a link's characters on one line.
typedef struct {
    int32_t line;   ///< the line
    uint8_t col;    ///< where it starts
    uint8_t len;    ///< how long
    uint16_t link;  ///< the link (index in links)
} www_seg_t;

/// A link or a form field.
typedef struct {
    uint8_t type;    ///< WL_
    bool checked;    ///< a checkbox or radio button is on
    int16_t form;    ///< its form, or -1
    uint32_t href;   ///< the address (WL_HREF), or the field's name: in the pool
    uint32_t value;  ///< a field's value, or a submit button's label: in the pool
    uint32_t opts;   ///< a <select>'s options, "label\0value\0" pairs: in the pool
    uint16_t nopts;  ///< how many options
    uint16_t sel;    ///< the option chosen
    uint8_t width;   ///< a field's width on the page
    int32_t seg;     ///< its first run of characters, or -1 if it has none
    uint32_t img;    ///< the address of a picture in it (in the pool), or 0 for none
} www_link_t;

/// A form.
typedef struct {
    uint32_t action;  ///< where it goes (in the pool; "" is the page itself)
    bool post;        ///< method=post
} www_form_t;

/// A place a URL's #fragment can go to.
typedef struct {
    uint32_t name;  ///< the id or name (in the pool)
    int32_t line;   ///< the line
} www_anchor_t;

/// The most rows a picture takes on the page (in the browser's picture mode).
#define WWW_PIC_ROWS 20

/// A picture drawn in the page (picture mode): a box of empty lines, the first
/// showing its [alt text] until it is drawn over (or if it can't be).
typedef struct {
    uint32_t src;   ///< its address (in the pool)
    int32_t line;   ///< the box's first line
    uint8_t rows;   ///< the lines it takes
    uint8_t col;    ///< the column it starts in (the indent)
    uint16_t w;     ///< the box's size in pixels (the picture is fitted in it)
    uint16_t h;     ///< (h <= rows * 16)
    int32_t link;   ///< what clicking it does: its link, or -1
} www_pic_t;

/// A laid-out page.
typedef struct {
    www_cell_t *cells;  ///< all the lines' characters
    uint32_t ncells;    ///< how many
    uint32_t cap_cells; ///< room
    www_line_t *lines;  ///< the lines
    int32_t nlines;     ///< how many
    int32_t cap_lines;  ///< room
    www_seg_t *segs;    ///< the links' runs, in line order
    int32_t nsegs;      ///< how many
    int32_t cap_segs;   ///< room
    www_link_t *links;  ///< links and fields, in page order
    int32_t nlinks;     ///< how many
    int32_t cap_links;  ///< room
    www_form_t *forms;  ///< the forms
    int32_t nforms;     ///< how many
    int32_t cap_forms;  ///< room
    www_anchor_t *anchors;  ///< the anchors
    www_pic_t *pics;        ///< the pictures (picture mode), in line order
    int32_t npics;          ///< how many
    int32_t cap_pics;       ///< room
    int32_t nanchors;       ///< how many
    int32_t cap_anchors;    ///< room
    char *pool;         ///< strings, each 0-terminated
    uint32_t npool;     ///< bytes used
    uint32_t cap_pool;  ///< room
    char title[96];     ///< <title>, in code page 437
    char base[WWW_URL_MAX];  ///< <base href>, or ""
    bool oom;           ///< PSRAM ran out: the page is cut short
} www_doc_t;

/// Picture mode: the size of a picture already loaded, by its src (as written
/// in the page). True if known; 0 x 0 means it can't be drawn (only its
/// [alt text] is shown).
typedef bool (*www_size_fn)(const char *src, int *w, int *h);

/// Lays out HTML, or plain text if `html` is false.
/// @param src the page's bytes (needn't be 0-terminated)
/// @param len how many
/// @param html HTML rather than plain text
/// @param latin1 the bytes are ISO-8859-1 or Windows-1252 rather than UTF-8
/// (a <meta charset> in the page changes it)
/// @param pictures leave room for pictures (PNG, JPEG, BMP) in the page, to be
/// drawn there (www_pic_t); else they are only their [alt text]
/// @param size_of the sizes of pictures already loaded, or NULL
/// @return the page (PSRAM), or NULL if there was no memory at all
www_doc_t *www_layout(const char *src, size_t len, bool html, bool latin1, bool pictures,
                      www_size_fn size_of);

/// Frees a page.
/// @param d the page, or NULL
void www_free(www_doc_t *d);

/// A string in the page's pool.
/// @param d the page
/// @param off its offset
/// @return the string
static inline const char *www_str(const www_doc_t *d, uint32_t off) {
    return d->pool + off;
}

/// Sets a field's value (code page 437) and draws it again on its line.
/// @param d the page
/// @param link the field
/// @param value the new value
/// @return false if there was no memory
bool www_set_value(www_doc_t *d, int link, const char *value);

/// Draws a field again (after its checked or sel changed).
/// @param d the page
/// @param link the field
void www_redraw_field(www_doc_t *d, int link);

/// A <select>'s option label or value.
/// @param d the page
/// @param link the select
/// @param i the option
/// @param value its value, rather than its label
/// @return the text
const char *www_option(const www_doc_t *d, int link, int i, bool value);

/// The first run of characters at or after `line`, by binary search.
/// @param d the page
/// @param line the line
/// @return its index (nsegs if none)
int32_t www_seg_from(const www_doc_t *d, int32_t line);

/// The line an anchor is on, or -1.
/// @param d the page
/// @param name the anchor (a URL's #fragment, without the #)
/// @return the line, or -1
int32_t www_anchor(const www_doc_t *d, const char *name);

/// Makes `rel` absolute against `base` (both URLs), the way a browser does:
/// "//host/x", "/x", "x", "../x", "?q", "#f". Dot segments are taken out.
/// @param base the page's URL
/// @param rel the link
/// @param out the absolute URL
/// @param size room in out
void www_resolve(const char *base, const char *rel, char *out, size_t size);

/// The form's fields as application/x-www-form-urlencoded (UTF-8), with the
/// submit button `button` (or none if -1).
/// @param d the page
/// @param form the form
/// @param button the button pressed, or -1
/// @param out the encoded fields
/// @param size room in out
/// @return false if they didn't fit
bool www_form_data(const www_doc_t *d, int form, int button, char *out, size_t size);

/// Percent-encodes text (code page 437, sent as UTF-8) for a query: spaces
/// become '+'.
/// @param s the text
/// @param out the encoded text
/// @param size room in out
/// @return false if it didn't fit
bool www_url_encode(const char *s, char *out, size_t size);

/// A code point as code page 437 (accents are dropped where CP437 has none).
/// @param u the code point
/// @return the byte, or 0 for nothing (zero-width characters)
uint8_t www_cp437(uint32_t u);

/// A code page 437 character's code point.
/// @param c the character
/// @return the code point
uint32_t www_unicode(uint8_t c);
