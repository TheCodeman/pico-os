/**
 * @file www.c
 * @brief www: a text-mode web browser on mc's screen (mc.h), in the manner of
 *        lynx: pages laid out as text (www_html.c), fetched over HTTP and HTTPS
 *        (www_net.c), with links and simple forms chosen by the keys or the
 *        mouse, pull-down menus, history, bookmarks and find.
 *
 * Everything it uses is in PSRAM, and only while it is open. Pages already
 * seen are kept (up to a budget) so Back doesn't load them again.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "www.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "image.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "psram_heap.h"
#include "www_html.h"
#include "www_net.h"
#include "version.h"

/// The page's first screen row (under the menu bar).
#define PAGE_TOP   1
/// The page's rows.
#define PAGE_ROWS  (SCR_H - 3)
/// The status line.
#define STATUS_ROW (SCR_H - 2)
/// Pages remembered for Back.
#define HIST_MAX   40
/// Pages already seen are kept up to this much in all.
#define CACHE_BUDGET (3u << 20)
/// The most form data sent.
#define POST_MAX   4096
/// The start page, if the drive has one.
#define HOME_FILE  "/WWW/HOME.HTM"
/// The bookmarks.
#define BOOKMARKS  "/WWW/BOOKMARK.HTM"
/// A picture being shown: the viewer reads it from the drive.
#define IMAGE_TMP  "/WWW/IMAGE.TMP"
/// Where a search goes: the words are put on the end.
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="

// ---- Colours ----------------------------------------------------------------------------------------
/// The page.
#define C_PAGE      A(LGRAY, BLACK)
/// A link.
#define C_LINK      A(LGREEN, BLACK)
/// A picture (its alt text) that is a link of its own, to view it.
#define C_IMG_LINK  A(LMAGENTA, BLACK)
/// The link chosen.
#define C_LINK_CUR  A(BLACK, CYAN)
/// A form field.
#define C_FIELD     A(BLACK, LGRAY)
/// The form field chosen.
#define C_FIELD_CUR A(BLACK, CYAN)
/// Text found.
#define C_FOUND     A(BLACK, YELLOW)
/// The status line.
#define C_STATUS    A(WHITE, BLUE)
/// The scroll bar.
#define C_SCROLL    A(DGRAY, BLACK)

/// Each style's colours (WS_).
static const uint8_t style_attr[WS_COUNT] = {
    [WS_TEXT] = A(LGRAY, BLACK),  [WS_BOLD] = A(WHITE, BLACK),   [WS_ITALIC] = A(LCYAN, BLACK),
    [WS_CODE] = A(CYAN, BLACK),   [WS_H1] = A(YELLOW, BLACK),    [WS_H2] = A(YELLOW, BLACK),
    [WS_IMG] = A(LMAGENTA, BLACK), [WS_FIELD] = A(BLACK, LGRAY), [WS_RULE] = A(DGRAY, BLACK),
    [WS_QUOTE] = A(BROWN, BLACK),
};

/// A page in the history.
typedef struct {
    char url[WWW_URL_MAX];  ///< its address
    char title[64];         ///< its title
    char *post;             ///< the form data it was sent (PSRAM), or NULL
    char *src;              ///< the page as it came (PSRAM), or NULL if not kept
    uint32_t len;           ///< its length
    char type[48];          ///< its Content-Type
    bool latin1;            ///< ISO-8859-1 rather than UTF-8
    int32_t top;            ///< the first line shown when it was left
    int cur;                ///< the link chosen then
} hist_t;

/// The browser (PSRAM, while it runs).
typedef struct {
    hist_t hist[HIST_MAX];  ///< the pages visited
    int nhist;              ///< how many
    int pos;                ///< the one shown
    www_doc_t *doc;         ///< it, laid out
    bool source;            ///< showing its source rather than the page
    int32_t top;            ///< the first line shown
    int cur;                ///< the link chosen, or -1
    bool verify;            ///< check https certificates
    bool quit;              ///< leaving
    char note[160];         ///< said on the status line until the next key
    char cap[160];          ///< a line being printed (captured, see capture())
    int caplen;             ///< its length
    bool cap_esc;           ///< in an escape sequence
    char msg[160];          ///< the last line printed
    bool drawing;           ///< the status line is being drawn (no recursion)
    char find[64];          ///< the text to find
    int32_t find_line;      ///< where it was found, or -1
    int find_col;           ///< its column
    int find_len;           ///< its length
    char input[WWW_URL_MAX];  ///< a dialog's text
    char url[WWW_URL_MAX];    ///< a URL being worked out
    char url2[WWW_URL_MAX];   ///< and another
    char target[WWW_URL_MAX]; ///< the page load() is loading
    char post[POST_MAX];      ///< form data
    www_reply_t reply;        ///< what a fetch got
    menu_t menus[5];          ///< the menu bar (menu_titles and the items)
    menu_item_t view[8];      ///< the View menu, with its tick
    int key_col[11];          ///< where each key starts on the key bar, for the mouse
    stdio_driver_t cap_driver;  ///< the driver that catches printing (see capture())
} www_t;

static www_t *W;  ///< PSRAM, while open

/// The built-in start page.
static const char home_page[] =
    "<title>pico-os web browser</title><h1>pico-os web browser</h1>"
    "<form action=\"" SEARCH_URL "\" method=get>Search the web: <input name=q size=36> "
    "<input type=submit value=Search></form>"
    "<p>Press <b>g</b> to go to an address, <b>F9</b> for the menus and <b>F1</b> for help. "
    "Up and Down choose a link, Enter or Right follows it, Left goes back.</p>"
    "<h2>Sites that read well in text</h2><ul>"
    "<li><a href=\"https://text.npr.org/\">NPR News</a> (text only)"
    "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a>"
    "<li><a href=\"https://en.m.wikipedia.org/wiki/Special:Random\">A random Wikipedia article</a>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a>: a search engine for vintage computers"
    "<li><a href=\"https://wiby.me/\">Wiby</a>: the classic web"
    "<li><a href=\"http://68k.news/\">68k.news</a>: headlines for old browsers"
    "<li><a href=\"file://" BOOKMARKS "\">Your bookmarks</a></ul>"
    "<p>Your own start page: save it as <code>" HOME_FILE "</code> on the drive. "
    "https needs <code>CACERT.PEM</code> in the drive's root to check certificates.</p>";

/// The help page.
static const char help_page[] =
    "<title>www help</title><h1>www: keys</h1><pre>"
    "Up, Down, Tab     choose the previous or next link (the page scrolls)\n"
    "Enter, Right      follow the link; edit, tick or pick a form field\n"
    "Left, Backspace   back                  Ctrl-F   forward\n"
    "PgDn, Space       a page down           PgUp, b  a page up\n"
    "Home, End         the top or the end of the page\n"
    "g, F2             go to an address (or type words to search the web)\n"
    "G                 edit this page's address\n"
    "o                 open a file on the drive\n"
    "/, F3             find text on the page   n  find it again\n"
    "\\, F6            the page's source, and back\n"
    "r, F5             load the page again\n"
    "m                 the start page\n"
    "a, F7             bookmark this page      v, F8  the bookmarks\n"
    "d                 download the link to the drive\n"
    "i                 show the chosen picture (or the link's) on the whole screen\n"
    "s                 save the page's source  p      save it as text\n"
    "=                 about this page and the link\n"
    "F9, Alt+letter    the menus\n"
    "q, F10            quit\n"
    "</pre><h2>The mouse</h2><p>Click a link to follow it; right-click for a menu (back, forward, reload, "
    "download the link, bookmark, find...); "
    "the wheel scrolls; click the menu bar, the key bar or the scroll bar.</p>"
    "<h2>Notes</h2><ul><li>There are no pictures, scripts or styles: pages show their "
    "text, links and forms, as in lynx.<li>Pages up to 4 MB load into PSRAM; bigger files "
    "go to the drive with <b>d</b> or the wget command.<li>Text fields are edited one line "
    "at a time. In a form with one text field, Enter after editing it sends the form."
    "<li>Cookies are kept until the browser closes.</ul>";

// ---- Menus ----------------------------------------------------------------------------------------

/// Menu items.
enum {
    M_OPEN = 1, M_FILE, M_RELOAD, M_SAVE_SRC, M_SAVE_TEXT, M_DOWNLOAD, M_QUIT,
    M_BACK, M_FORWARD, M_HOME, M_HISTORY, M_FIND, M_FIND_NEXT, M_TOP, M_END,
    M_SOURCE, M_INFO, M_VERIFY,
    M_MARK, M_MARKS,
    M_HELP, M_ABOUT,
    M_FOLLOW, M_IMAGE,
};

static const menu_item_t file_items[] = {
    {"Open address...", M_OPEN, "g", false},  {"Open file...", M_FILE, "o", false},
    {"Reload", M_RELOAD, "r", false},         {"-", 0, NULL, false},
    {"Save source...", M_SAVE_SRC, "s", false}, {"Save as text...", M_SAVE_TEXT, "p", false},
    {"Download link...", M_DOWNLOAD, "d", false}, {"-", 0, NULL, false},
    {"Quit", M_QUIT, "q", false},             {NULL, 0, NULL, false},
};  ///< File
static const menu_item_t nav_items[] = {
    {"Back", M_BACK, "Left", false},       {"Forward", M_FORWARD, "Ctrl-F", false},
    {"Start page", M_HOME, "m", false},    {"History...", M_HISTORY, "Ctrl-Y", false},
    {"-", 0, NULL, false},                 {"Find...", M_FIND, "/", false},
    {"Find next", M_FIND_NEXT, "n", false}, {"Top", M_TOP, "Home", false},
    {"End", M_END, "End", false},          {NULL, 0, NULL, false},
};  ///< Go
static const menu_item_t view_items[] = {
    {"Page source", M_SOURCE, "\\", false},  {"Page info", M_INFO, "=", false},
    {"View image", M_IMAGE, "i", false},     {"-", 0, NULL, false},
    {"Check certificates", M_VERIFY, NULL, false}, {NULL, 0, NULL, false},
};  ///< View
static const menu_item_t mark_items[] = {
    {"Add this page", M_MARK, "a", false}, {"Show bookmarks", M_MARKS, "v", false},
    {NULL, 0, NULL, false},
};  ///< Bookmarks
static const menu_item_t help_items[] = {
    {"Keys", M_HELP, "F1", false}, {"About", M_ABOUT, NULL, false}, {NULL, 0, NULL, false},
};  ///< Help
/// The menu bar's titles.
static const char *const menu_titles[] = {"File", "Go", "View", "Bookmarks", "Help"};
/// Their count.
#define NMENUS (int)(sizeof menu_titles / sizeof *menu_titles)
_Static_assert(sizeof view_items <= sizeof ((www_t *)0)->view, "room for the View menu in W");

/// Where menu i's title starts on the bar (as mc's menu_run draws it).
/// @param i the menu
/// @return its column
static int menu_col(int i) {
    int c = 2;
    for (int k = 0; k < i; ++k)
        c += (int)strlen(menu_titles[k]) + 4;
    return c;
}

// ---- The key bar --------------------------------------------------------------------------------------

static const struct {
    /// The key's name and what it does.
    ///@{
    const char *key, *text;
    ///@}
    int code;  ///< the key code
} keys[] = {
    {"F1", "Help", KEY_F1},   {"F2", "Go", KEY_F2},     {"F3", "Find", KEY_F3},
    {"F4", "Back", KEY_F4},   {"F5", "Reload", KEY_F5}, {"F6", "Source", KEY_F6},
    {"F7", "Mark", KEY_F7},   {"F8", "Marks", KEY_F8},  {"F9", "Menu", KEY_F9},
    {"F10", "Quit", KEY_F10},
};  ///< the key bar
/// Their count.
#define NKEYS (int)(sizeof keys / sizeof *keys)
_Static_assert(NKEYS + 1 <= (int)(sizeof ((www_t *)0)->key_col / sizeof (int)), "room for the key bar in W");

// ---- Printing goes to the status line ---------------------------------------------------------------

/// Draws text on the status line now.
/// @param text the text
static void status_now(const char *text) {
    if (W->drawing)
        return;
    W->drawing = true;
    scr_fill(STATUS_ROW, 0, 1, SCR_W, ' ', C_STATUS);
    scr_text(STATUS_ROW, 1, SCR_W - 2, C_STATUS, text);
    scr_flush();
    W->drawing = false;
}

/// stdio's output while a page loads: each line printed (by the network
/// code) shows on the status line, and the last is kept for errors.
/// @param buf the characters
/// @param len how many
static void cap_out(const char *buf, int len) {
    for (int i = 0; i < len; ++i) {
        char c = buf[i];
        if (W->cap_esc) {  // escape sequences are dropped
            W->cap_esc = !isalpha((unsigned char)c);
            continue;
        }
        if (c == 0x1b) {
            W->cap_esc = true;
        } else if (c == '\n' || c == '\r') {
            if (W->caplen) {
                W->cap[W->caplen] = 0;
                snprintf(W->msg, sizeof W->msg, "%s", W->cap);
                W->caplen = 0;
                status_now(W->msg);
            }
        } else if ((uint8_t)c >= ' ' && W->caplen < (int)sizeof W->cap - 1) {
            W->cap[W->caplen++] = c;
        }
    }
}

/// Sends printing to the status line (on) or the console again (off).
/// @param on on or off
static void capture(bool on) {
    if (on) {
        W->msg[0] = 0;
        W->caplen = 0;
        W->cap_esc = false;
        scr_flush();
    }
    fflush(stdout);
    stdio_set_driver_enabled(&console_stdio_driver, !on);
    stdio_set_driver_enabled(&W->cap_driver, on);
}

// ---- The page on the screen -----------------------------------------------------------------------------

/// The furthest the page scrolls down.
/// @return the top line then
static int32_t max_top(void) {
    int32_t n = W->doc->nlines - PAGE_ROWS;
    return n > 0 ? n : 0;
}

/// Keeps the page's top in range.
static void clamp_top(void) {
    if (W->top > max_top())
        W->top = max_top();
    if (W->top < 0)
        W->top = 0;
}

/// Is a link's first line on the screen?
/// @param link the link
/// @return it is
static bool visible(int link) {
    if (link < 0 || link >= W->doc->nlinks || W->doc->links[link].seg < 0)
        return false;
    int32_t line = W->doc->segs[W->doc->links[link].seg].line;
    return line >= W->top && line < W->top + PAGE_ROWS;
}

/// The first link on the screen, or -1.
/// @return the link
static int first_visible(void) {
    int32_t s = www_seg_from(W->doc, W->top);
    return s < W->doc->nsegs && W->doc->segs[s].line < W->top + PAGE_ROWS ? W->doc->segs[s].link : -1;
}

/// The page's base for its links: <base href>, else its own address.
/// @return the base URL
static const char *base_url(void) {
    return W->doc->base[0] ? W->doc->base : W->hist[W->pos].url;
}

/// What the status line says about the chosen link.
/// @param out the text
/// @param size room in out
static void describe_link(char *out, size_t size) {
    const www_doc_t *d = W->doc;
    const www_link_t *l = &d->links[W->cur];
    const char *name = www_str(d, l->href);
    switch (l->type) {
    case WL_HREF:
        www_resolve(base_url(), name, W->url2, sizeof W->url2);
        snprintf(out, size, "%s%s", W->url2, l->img ? "   (i: its picture)" : "");
        return;
    case WL_IMAGE:
        www_resolve(base_url(), name, W->url2, sizeof W->url2);
        snprintf(out, size, "Picture: Enter shows it: %s", W->url2);
        return;
    case WL_TEXT:
    case WL_TEXTAREA:
        snprintf(out, size, "Text field %s: Enter to type in it", name);
        return;
    case WL_PASSWORD:
        snprintf(out, size, "Password field %s: Enter to type in it", name);
        return;
    case WL_CHECK:
        snprintf(out, size, "Checkbox %s: Enter ticks or clears it", name);
        return;
    case WL_RADIO:
        snprintf(out, size, "Radio button %s: Enter chooses it", name);
        return;
    case WL_SELECT:
        snprintf(out, size, "List %s: Enter to choose (%d choices)", name, l->nopts);
        return;
    case WL_SUBMIT:
        if (l->form >= 0) {
            www_resolve(base_url(), www_str(d, d->forms[l->form].action), W->url2, sizeof W->url2);
            snprintf(out, size, "Button: Enter sends the form (%s) to %s",
                     d->forms[l->form].post ? "post" : "get", W->url2);
        } else {
            snprintf(out, size, "Button (outside any form)");
        }
        return;
    }
    out[0] = 0;
}

/// Draws the menu bar's titles and the page's title.
static void draw_bar(void) {
    scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
    for (int i = 0; i < NMENUS; ++i)
        scr_printf(0, menu_col(i), -1, C_MENU, "  %s  ", menu_titles[i]);
    int c = menu_col(NMENUS) + 1;
    const char *title = W->source ? "Source" : W->doc->title[0] ? W->doc->title : W->hist[W->pos].url;
    int w = SCR_W - c - 1, n = (int)strlen(title);
    scr_text(0, n < w ? SCR_W - 1 - n : c, w, C_MENU, title);
}

/// Draws the key bar.
static void draw_keybar(void) {
    int c = 0;
    scr_fill(SCR_H - 1, 0, 1, SCR_W, ' ', C_KEYTEXT);
    for (int i = 0; i < NKEYS; ++i) {
        W->key_col[i] = c;
        scr_text(SCR_H - 1, c, -1, C_KEYNUM, keys[i].key);
        c += (int)strlen(keys[i].key);
        scr_text(SCR_H - 1, c, -1, C_KEYTEXT, keys[i].text);
        c += (int)strlen(keys[i].text) + 1;
    }
    W->key_col[NKEYS] = c;
}

/// Draws everything.
static void draw(void) {
    const www_doc_t *d = W->doc;
    draw_bar();
    for (int r = 0; r < PAGE_ROWS; ++r) {
        int row = PAGE_TOP + r;
        int32_t line = W->top + r;
        scr_fill(row, 0, 1, SCR_W, ' ', C_PAGE);
        if (line >= d->nlines)
            continue;
        const www_line_t *ln = &d->lines[line];
        for (int c = 0; c < ln->len; ++c) {
            const www_cell_t *cell = &d->cells[ln->cell + c];
            scr_put(row, 1 + c, cell->ch, style_attr[cell->st < WS_COUNT ? cell->st : 0]);
        }
    }
    // The links on the screen, in their colours.
    for (int32_t s = www_seg_from(d, W->top); s < d->nsegs && d->segs[s].line < W->top + PAGE_ROWS; ++s) {
        const www_seg_t *g = &d->segs[s];
        int type = d->links[g->link].type;
        bool field = type != WL_HREF && type != WL_IMAGE;
        uint8_t at = g->link == W->cur ? (field ? C_FIELD_CUR : C_LINK_CUR)
                     : field           ? C_FIELD
                     : type == WL_IMAGE ? C_IMG_LINK
                                        : C_LINK;
        const www_cell_t *cells = d->cells + d->lines[g->line].cell + g->col;
        for (int i = 0; i < g->len; ++i)
            scr_put(PAGE_TOP + g->line - W->top, 1 + g->col + i, cells[i].ch, at);
    }
    if (W->find_line >= W->top && W->find_line < W->top + PAGE_ROWS) {
        const www_cell_t *cells = d->cells + d->lines[W->find_line].cell + W->find_col;
        for (int i = 0; i < W->find_len; ++i)
            scr_put(PAGE_TOP + W->find_line - W->top, 1 + W->find_col + i, cells[i].ch, C_FOUND);
    }
    // The scroll bar.
    if (d->nlines > PAGE_ROWS) {
        int thumb = (int)((int64_t)W->top * (PAGE_ROWS - 1) / max_top());
        for (int r = 0; r < PAGE_ROWS; ++r)
            scr_put(PAGE_TOP + r, SCR_W - 1, r == thumb ? BLOCK_FULL : BLOCK_LIGHT, C_SCROLL);
    }
    // The status line: a note, else the link chosen, else the address.
    char text[WWW_URL_MAX + 64];
    if (W->note[0])
        snprintf(text, sizeof text, "%s", W->note);
    else if (W->cur >= 0 && W->cur < d->nlinks)
        describe_link(text, sizeof text);
    else
        snprintf(text, sizeof text, "%s", W->hist[W->pos].url);
    char where[32];
    if (d->nlines > PAGE_ROWS) {
        int64_t pc = (int64_t)(W->top + PAGE_ROWS) * 100 / d->nlines;
        snprintf(where, sizeof where, " %d%% ", pc > 100 ? 100 : (int)pc);
    } else
        snprintf(where, sizeof where, " All ");
    int ww = (int)strlen(where);
    scr_fill(STATUS_ROW, 0, 1, SCR_W, ' ', C_STATUS);
    scr_text(STATUS_ROW, 1, SCR_W - 2 - ww, C_STATUS, text);
    scr_text(STATUS_ROW, SCR_W - ww, ww, C_STATUS, where);
    draw_keybar();
    scr_cursor(-1, 0);
}

// ---- History and loading ---------------------------------------------------------------------------------

/// Frees a history entry's page and form data.
/// @param h the entry
static void hist_free(hist_t *h) {
    psram_free(h->src);
    psram_free(h->post);
    h->src = h->post = NULL;
}

/// Keeps the pages remembered within CACHE_BUDGET: the oldest go first (they
/// are loaded again if Back comes to them).
static void trim_cache(void) {
    uint32_t total = 0;
    for (int i = 0; i < W->nhist; ++i)
        total += W->hist[i].src ? W->hist[i].len : 0;
    for (int i = 0; i < W->nhist && total > CACHE_BUDGET; ++i)
        if (i != W->pos && W->hist[i].src) {
            total -= W->hist[i].len;
            psram_free(W->hist[i].src);
            W->hist[i].src = NULL;
        }
}

/// Lays out the current history entry's page (or its source).
/// @param fragment a #name to scroll to, or NULL
static void show(const char *fragment) {
    hist_t *h = &W->hist[W->pos];
    bool html;
    www_is_text(h->type, &html);
    if (W->source)
        html = false;
    status_now("Laying out the page...");
    www_free(W->doc);
    W->doc = www_layout(h->src ? h->src : "", h->src ? h->len : 0, html, h->latin1);
    if (!W->doc)
        W->doc = www_layout("", 0, false, false);  // (tiny: there is room for this)
    if (W->doc->oom)
        snprintf(W->note, sizeof W->note, "Out of memory: only the start of the page is shown");
    if (!W->source && W->doc->title[0])
        snprintf(h->title, sizeof h->title, "%s", W->doc->title);
    W->find_line = -1;
    W->top = h->top;
    W->cur = h->cur;
    if (fragment && *fragment) {
        int32_t line = www_anchor(W->doc, fragment);
        if (line >= 0)
            W->top = line;
    }
    clamp_top();
    if (W->cur >= W->doc->nlinks || !visible(W->cur))
        W->cur = first_visible();
}

/// Remembers where the page shown is scrolled to.
static void remember_place(void) {
    if (W->nhist) {
        W->hist[W->pos].top = W->top;
        W->hist[W->pos].cur = W->cur;
    }
}

/// Saves bytes to a file on the drive, asking for its name.
/// @param title the dialog's title
/// @param name the name suggested
/// @param data the bytes
/// @param len how many
static void save_bytes(const char *title, const char *name, const char *data, uint32_t len) {
    snprintf(W->input, sizeof W->input, "%s", name);
    if (!dlg_input(title, "Save as (on the drive):", W->input, 256) || !W->input[0])
        return;
    FIL f;
    FRESULT r = f_open(&f, W->input, FA_WRITE | FA_CREATE_NEW);
    if (r == FR_EXIST) {
        if (dlg_message(title, "That file is there already. Replace it?", "Replace|Cancel", false) != 0)
            return;
        r = f_open(&f, W->input, FA_WRITE | FA_CREATE_ALWAYS);
    }
    UINT put = 0;
    if (r == FR_OK) {
        r = f_write(&f, data, len, &put);
        f_close(&f);
    }
    if (r != FR_OK || put != len) {
        f_unlink(W->input);
        dlg_message(title, "Can't write the file (is the drive in, and not full?)", "OK", true);
        return;
    }
    snprintf(W->note, sizeof W->note, "Saved %lu bytes to %s", (unsigned long)len, W->input);
}

/// A file name for a URL: the last part of its path.
/// @param url the URL
/// @param out the name
/// @param size room in out
static void name_from_url(const char *url, char *out, size_t size) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    s += strcspn(s, "/");
    const char *end = s + strcspn(s, "?#"), *start = end;
    while (start > s && start[-1] != '/')
        --start;
    if (start == end)
        snprintf(out, size, "index.htm");
    else
        snprintf(out, size, "%.*s", (int)(end - start), start);
}

/// Fetches a URL, asking about a certificate that can't be checked; printing
/// shows on the status line. Says why on failure.
/// @param url the URL
/// @param post form data, or NULL
/// @return got it (in W->reply)
static bool fetch(const char *url, const char *post) {
    snprintf(W->note, sizeof W->note, "Loading %s", url);
    status_now(W->note);
    W->note[0] = 0;
    capture(true);
    int res = www_fetch(url, post, W->verify, &W->reply);
    capture(false);
    if (res == WF_CERT) {
        char text[300];
        snprintf(text, sizeof text, "%.66s\n\nThe server's certificate can't be checked.\n"
                 "Load the page anyway (not safe for passwords)?", W->msg);
        int b = dlg_message("Certificate", text, "Yes|No|Always", true);
        if (b != 0 && b != 2)
            return false;
        if (b == 2)
            W->verify = false;
        capture(true);
        res = www_fetch(url, post, false, &W->reply);
        capture(false);
    }
    if (res != WF_OK) {
        dlg_message("Can't load the page", W->msg[0] ? W->msg : "It failed", "OK", true);
        return false;
    }
    if (W->reply.cut)
        snprintf(W->note, sizeof W->note, "%s", W->msg);
    return true;
}

/// Loads a page and shows it.
/// @param url its address (http, https, file or about)
/// @param post form data to send, or NULL
/// @param push a new page in the history (else the current one, reloaded)
/// @return it was shown
static bool load(const char *url, const char *post, bool push) {
    char *src;
    uint32_t len;
    char type[48];
    bool latin1 = false;
    // The URL is copied: it may be in a buffer used below, or in the history
    // entry about to change.
    snprintf(W->target, sizeof W->target, "%s", url);
    const char *frag = strchr(W->target, '#');
    if (!strncasecmp(W->target, "about:", 6)) {
        const char *page = !strcasecmp(W->target, "about:help") ? help_page : home_page;
        len = (uint32_t)strlen(page);
        src = psram_malloc(len + 1);
        if (!src)
            return false;
        memcpy(src, page, len + 1);
        snprintf(type, sizeof type, "text/html");
    } else {
        if (!fetch(W->target, post))
            return false;
        www_reply_t *r = &W->reply;
        bool html;
        if (!www_is_text(r->type, &html)) {
            char name[96];
            name_from_url(r->url, name, sizeof name);
            char title[80];
            snprintf(title, sizeof title, "Download (%s, %lu bytes)", r->type, (unsigned long)r->len);
            save_bytes(title, name, r->body, r->len);
            psram_free(r->body);
            return false;
        }
        src = r->body;
        len = r->len;
        latin1 = r->latin1;
        snprintf(type, sizeof type, "%s", r->type);
        // Keep the #fragment asked for, if a redirect lost it.
        if (frag && !strchr(r->url, '#'))
            snprintf(W->url2, sizeof W->url2, "%s%s", r->url, frag);
        else
            snprintf(W->url2, sizeof W->url2, "%s", r->url);
        snprintf(W->target, sizeof W->target, "%s", W->url2);
        frag = strchr(W->target, '#');
    }
    char *post_copy = NULL;
    if (post && (post_copy = psram_malloc(strlen(post) + 1)) != NULL)
        strcpy(post_copy, post);
    if (push) {
        remember_place();
        for (int i = W->pos + 1; i < W->nhist; ++i)  // forward is forgotten
            hist_free(&W->hist[i]);
        W->nhist = W->nhist ? W->pos + 1 : 0;
        if (W->nhist == HIST_MAX) {  // full: the oldest goes
            hist_free(&W->hist[0]);
            memmove(W->hist, W->hist + 1, (HIST_MAX - 1) * sizeof *W->hist);
            --W->nhist;
        }
        W->pos = W->nhist++;
        memset(&W->hist[W->pos], 0, sizeof W->hist[W->pos]);
        W->hist[W->pos].cur = -1;
    } else {
        hist_free(&W->hist[W->pos]);
    }
    hist_t *h = &W->hist[W->pos];
    snprintf(h->url, sizeof h->url, "%s", W->target);
    h->src = src;
    h->len = len;
    h->post = post_copy;
    h->latin1 = latin1;
    snprintf(h->type, sizeof h->type, "%s", type);
    h->title[0] = 0;
    if (push)
        W->source = false;
    show(frag ? frag + 1 : NULL);
    trim_cache();
    return true;
}

/// Goes back (-1) or forward (+1) in the history.
/// @param dir the way
static void history_go(int dir) {
    int to = W->pos + dir;
    if (to < 0 || to >= W->nhist) {
        snprintf(W->note, sizeof W->note, dir < 0 ? "Nothing to go back to" : "Nothing to go forward to");
        return;
    }
    remember_place();
    int from = W->pos;
    W->pos = to;
    W->source = false;
    hist_t *h = &W->hist[to];
    if (h->src) {
        show(NULL);
        return;
    }
    // Not kept: loaded again, in place.
    int32_t top = h->top;
    int cur = h->cur;
    if (!load(h->url, h->post, false)) {
        W->pos = from;
        return;
    }
    h->top = top;
    h->cur = cur;
    show(NULL);
}

/// Goes to a link's address (or, for #name on this page, just scrolls).
/// @param url the address, absolute
/// @param post form data, or NULL
static void follow(const char *url, const char *post) {
    const char *here = W->hist[W->pos].url;
    size_t n = strcspn(url, "#");
    if (!post && url[n] == '#' && n == strcspn(here, "#") && !strncmp(url, here, n)) {
        int32_t line = www_anchor(W->doc, url + n + 1);
        if (line < 0) {
            snprintf(W->note, sizeof W->note, "No #%s on this page", url + n + 1);
            return;
        }
        W->top = line;
        clamp_top();
        if (!visible(W->cur))
            W->cur = first_visible();
        return;
    }
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8) &&
        strncasecmp(url, "file://", 7) && strncasecmp(url, "about:", 6)) {
        snprintf(W->note, sizeof W->note, "Can't open %.*s links", (int)strcspn(url, ":"), url);
        return;
    }
    load(url, post, true);
}

/// Opens what was typed: an address, a file on the drive, or words to search for.
/// @param text what was typed
static void open_typed(const char *text) {
    while (*text == ' ')
        ++text;
    if (!*text)
        return;
    FILINFO fi;
    if (strstr(text, "://") || !strncasecmp(text, "about:", 6)) {
        snprintf(W->url, sizeof W->url, "%s", text);
    } else if (!strchr(text, ' ') && f_stat(text, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) {
        char cwd[200] = "/";
        if (text[0] != '/' && f_getcwd(cwd, sizeof cwd) != FR_OK)
            snprintf(cwd, sizeof cwd, "/");
        const char *slash = strchr(cwd, '/');  // (no drive number)
        snprintf(W->url, sizeof W->url, "file://%s%s%s", text[0] == '/' ? "" : slash ? slash : "/",
                 text[0] == '/' || (slash && slash[strlen(slash) - 1] == '/') ? "" : "/", text);
    } else if (!strchr(text, ' ') && strchr(text, '.')) {
        snprintf(W->url, sizeof W->url, "https://%s", text);
    } else {
        char *q = W->url2;
        if (!www_url_encode(text, q, sizeof W->url2 - 64))
            return;
        snprintf(W->url, sizeof W->url, SEARCH_URL "%s", q);
    }
    snprintf(W->url2, sizeof W->url2, "%s", W->url);
    follow(W->url2, NULL);
}

/// The start page: HOME.HTM on the drive, else the built-in one.
static void go_home(void) {
    FILINFO fi;
    follow(f_stat(HOME_FILE, &fi) == FR_OK ? "file://" HOME_FILE : "about:home", NULL);
}

// ---- Links and forms ----------------------------------------------------------------------------------

/// Chooses the next (+1) or previous (-1) link, scrolling half a page when
/// it isn't on the screen.
/// @param dir the way
static void next_link(int dir) {
    const www_doc_t *d = W->doc;
    int32_t s;
    if (dir > 0) {
        if (visible(W->cur)) {
            s = d->links[W->cur].seg;
            while (s < d->nsegs && d->segs[s].link == W->cur)
                ++s;
        } else {
            s = www_seg_from(d, W->top);
        }
        if (s < d->nsegs && d->segs[s].line < W->top + PAGE_ROWS) {
            W->cur = d->segs[s].link;
            return;
        }
        if (W->top >= max_top()) {
            if (s >= d->nsegs)
                snprintf(W->note, sizeof W->note, "The end of the page");
            return;
        }
        W->top += PAGE_ROWS / 2;
    } else {
        s = visible(W->cur) ? d->links[W->cur].seg - 1 : www_seg_from(d, W->top + PAGE_ROWS) - 1;
        if (s >= 0 && d->segs[s].line >= W->top) {
            W->cur = d->segs[s].link;
            return;
        }
        if (W->top == 0)
            return;
        W->top -= PAGE_ROWS / 2;
    }
    clamp_top();
    if (s >= 0 && s < d->nsegs && d->segs[s].line >= W->top && d->segs[s].line < W->top + PAGE_ROWS)
        W->cur = d->segs[s].link;
    else if (!visible(W->cur))
        W->cur = -1;
}

/// Sends a form, as if `button` (or no button, -1) were pressed.
/// @param form the form
/// @param button its submit button, or -1
static void submit(int form, int button) {
    const www_doc_t *d = W->doc;
    if (form < 0 || form >= d->nforms) {
        snprintf(W->note, sizeof W->note, "That isn't in a form");
        return;
    }
    if (!www_form_data(d, form, button, W->post, sizeof W->post)) {
        snprintf(W->note, sizeof W->note, "The form is too big to send");
        return;
    }
    www_resolve(base_url(), www_str(d, d->forms[form].action), W->url, sizeof W->url);
    if (d->forms[form].post) {
        follow(W->url, W->post);
        return;
    }
    size_t n = strcspn(W->url, "?#");
    snprintf(W->url2, sizeof W->url2, "%.*s?%s", (int)n, W->url, W->post);
    follow(W->url2, NULL);
}

/// Edits a text field; in a form with only this text field, sends the form.
/// @param link the field
static void edit_field(int link) {
    www_doc_t *d = W->doc;
    www_link_t *l = &d->links[link];
    snprintf(W->input, 256, "%s", www_str(d, l->value));
    char prompt[80];
    snprintf(prompt, sizeof prompt, "%s:", www_str(d, l->href)[0] ? www_str(d, l->href) : "Text");
    if (!dlg_input(l->type == WL_PASSWORD ? "Password" : "Text field", prompt, W->input, 256))
        return;
    if (!www_set_value(d, link, W->input)) {
        snprintf(W->note, sizeof W->note, "Out of memory");
        return;
    }
    int texts = 0, button = -1;
    for (int i = 0; i < d->nlinks && l->form >= 0; ++i) {
        const www_link_t *o = &d->links[i];
        if (o->form != l->form)
            continue;
        texts += o->type == WL_TEXT || o->type == WL_PASSWORD || o->type == WL_TEXTAREA;
        if (o->type == WL_SUBMIT && button < 0)
            button = i;
    }
    if (texts == 1 && l->type != WL_TEXTAREA)
        submit(l->form, button);
}

/// Shows the chosen link's picture on the whole screen (PNG, JPEG or BMP,
/// as the view command does) until a key or a click.
static void view_image(void) {
    const www_doc_t *d = W->doc;
    if (W->cur < 0 || !visible(W->cur) || !d->links[W->cur].img) {
        snprintf(W->note, sizeof W->note, "Choose a [picture], or a link with one, first");
        return;
    }
    www_resolve(base_url(), www_str(d, d->links[W->cur].img), W->url, sizeof W->url);
    if (strncasecmp(W->url, "http://", 7) && strncasecmp(W->url, "https://", 8) &&
        strncasecmp(W->url, "file://", 7)) {
        snprintf(W->note, sizeof W->note, "Can't load a %.*s: picture", (int)strcspn(W->url, ":"), W->url);
        return;
    }
    if (!fetch(W->url, NULL))
        return;
    // The viewer reads files: the picture goes to the drive for a moment.
    f_mkdir("/WWW");
    FIL f;
    UINT put = 0;
    bool ok = f_open(&f, IMAGE_TMP, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if (ok) {
        ok = f_write(&f, W->reply.body, W->reply.len, &put) == FR_OK && put == W->reply.len;
        f_close(&f);
    }
    psram_free(W->reply.body);
    W->reply.body = NULL;
    image_t img;
    const char *err = "can't write " IMAGE_TMP;
    if (ok) {
        status_now("Decoding the picture...");
        ok = image_load(IMAGE_TMP, DVI_WIDTH, DVI_HEIGHT, &img, &err);
    }
    f_unlink(IMAGE_TMP);
    uint8_t *saved = ok ? psram_malloc(DVI_WIDTH * DVI_HEIGHT) : NULL;
    if (!saved) {
        if (ok)
            image_free(&img);
        char text[160];
        snprintf(text, sizeof text, "Can't show it: %s\n(PNG, JPEG and BMP can be shown;\nnot GIF, WebP or SVG)",
                 ok ? "not enough memory" : err);
        dlg_message("Picture", text, "OK", true);
        return;
    }
    // Drawn over the screen, which is put back afterwards.
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(true);
    memcpy(saved, dvi_framebuf, DVI_WIDTH * DVI_HEIGHT);
    memset(dvi_framebuf, 0, DVI_WIDTH * DVI_HEIGHT);
    int x = 0, y = 0, w = DVI_WIDTH, h = DVI_HEIGHT;
    image_fit(&img, &x, &y, &w, &h);
    image_draw(&img, dvi_framebuf, x, y, w, h, NULL);
    snprintf(W->note, sizeof W->note, "Picture %d x %d: %s", img.w, img.h, W->url);
    image_free(&img);
    for (;;) {
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k != KEY_MOUSE || e.type == MOUSE_DOWN)
            break;
    }
    memcpy(dvi_framebuf, saved, DVI_WIDTH * DVI_HEIGHT);
    psram_free(saved);
    console_freeze(false);
}

/// Enter on the link chosen: follows it, or works the form field.
static void activate(void) {
    www_doc_t *d = W->doc;
    if (W->cur < 0 || W->cur >= d->nlinks || !visible(W->cur)) {
        snprintf(W->note, sizeof W->note, "No link chosen: Up and Down choose one");
        return;
    }
    www_link_t *l = &d->links[W->cur];
    switch (l->type) {
    case WL_HREF:
        www_resolve(base_url(), www_str(d, l->href), W->url2, sizeof W->url2);
        snprintf(W->url, sizeof W->url, "%s", W->url2);
        follow(W->url, NULL);
        break;
    case WL_TEXT:
    case WL_PASSWORD:
    case WL_TEXTAREA:
        edit_field(W->cur);
        break;
    case WL_CHECK:
        l->checked = !l->checked;
        www_redraw_field(d, W->cur);
        break;
    case WL_RADIO:
        for (int i = 0; i < d->nlinks; ++i) {
            www_link_t *o = &d->links[i];
            if (o->type == WL_RADIO && o->form == l->form && o->checked &&
                !strcmp(www_str(d, o->href), www_str(d, l->href))) {
                o->checked = false;
                www_redraw_field(d, i);
            }
        }
        l->checked = true;
        www_redraw_field(d, W->cur);
        break;
    case WL_SELECT: {
        if (!l->nopts)
            break;
        char **items = psram_malloc(l->nopts * sizeof *items);
        if (!items)
            break;
        for (int i = 0; i < l->nopts; ++i)
            items[i] = (char *)www_option(d, W->cur, i, false);
        int i = dlg_list(www_str(d, l->href)[0] ? www_str(d, l->href) : "Choose", items, l->nopts, l->sel);
        psram_free(items);
        if (i >= 0) {
            l->sel = (uint16_t)i;
            www_redraw_field(d, W->cur);
        }
        break;
    }
    case WL_SUBMIT:
        submit(l->form, W->cur);
        break;
    case WL_IMAGE:
        view_image();
        break;
    }
}

// ---- Find, save, bookmarks, info -----------------------------------------------------------------------

/// Finds W->find from just after the last match, wrapping round to the top.
static void find_next(void) {
    const www_doc_t *d = W->doc;
    int n = (int)strlen(W->find);
    if (!n)
        return;
    int32_t line = W->find_line >= 0 ? W->find_line : W->top;
    int col = W->find_line >= 0 ? W->find_col + 1 : 0;
    for (int32_t k = 0; k <= d->nlines; ++k) {
        int32_t l = (line + k) % (d->nlines ? d->nlines : 1);
        if (l >= d->nlines)
            break;
        const www_line_t *ln = &d->lines[l];
        for (int c = k ? 0 : col; c + n <= ln->len; ++c) {
            int i = 0;
            while (i < n && tolower(d->cells[ln->cell + c + i].ch) == tolower((uint8_t)W->find[i]))
                ++i;
            if (i == n) {
                if (line + k >= d->nlines)
                    snprintf(W->note, sizeof W->note, "Found again from the top");
                W->find_line = l;
                W->find_col = c;
                W->find_len = n;
                if (l < W->top || l >= W->top + PAGE_ROWS)
                    W->top = l - PAGE_ROWS / 3;
                clamp_top();
                // Choose a link on that line, if there is one.
                int32_t s = www_seg_from(d, l);
                if (s < d->nsegs && d->segs[s].line == l)
                    W->cur = d->segs[s].link;
                else if (!visible(W->cur))
                    W->cur = first_visible();
                return;
            }
        }
    }
    W->find_line = -1;
    snprintf(W->note, sizeof W->note, "\"%s\" isn't on the page", W->find);
}

/// Asks what to find, and finds it.
static void find(void) {
    snprintf(W->input, sizeof W->find, "%s", W->find);
    if (!dlg_input("Find", "Text to find on the page:", W->input, sizeof W->find) || !W->input[0])
        return;
    snprintf(W->find, sizeof W->find, "%s", W->input);
    W->find_line = -1;
    find_next();
}

/// Saves the page as text (what is on the screen, every line).
static void save_text(void) {
    const www_doc_t *d = W->doc;
    uint32_t size = 0;
    for (int32_t i = 0; i < d->nlines; ++i)
        size += d->lines[i].len + 2;
    char *buf = psram_malloc(size + 1);
    if (!buf) {
        snprintf(W->note, sizeof W->note, "Out of memory");
        return;
    }
    uint32_t n = 0;
    for (int32_t i = 0; i < d->nlines; ++i) {
        for (int c = 0; c < d->lines[i].len; ++c)
            buf[n++] = (char)d->cells[d->lines[i].cell + c].ch;
        buf[n++] = '\r';
        buf[n++] = '\n';
    }
    save_bytes("Save as text", "PAGE.TXT", buf, n);
    psram_free(buf);
}

/// Downloads the chosen link's address to the drive.
static void download(void) {
    const www_doc_t *d = W->doc;
    int type = W->cur >= 0 ? d->links[W->cur].type : -1;
    if (!visible(W->cur) || (type != WL_HREF && type != WL_IMAGE)) {
        snprintf(W->note, sizeof W->note, "Choose a link to download first");
        return;
    }
    www_resolve(base_url(), www_str(d, d->links[W->cur].href), W->url, sizeof W->url);
    if (!strncasecmp(W->url, "about:", 6) || !fetch(W->url, NULL))
        return;
    char name[96];
    name_from_url(W->reply.url, name, sizeof name);
    save_bytes("Download", name, W->reply.body, W->reply.len);
    psram_free(W->reply.body);
    W->reply.body = NULL;
}

/// Adds the page to the bookmarks file (an HTML page of links).
static void add_bookmark(void) {
    const hist_t *h = &W->hist[W->pos];
    snprintf(W->input, 64, "%s", h->title[0] ? h->title : h->url);
    if (!dlg_input("Bookmark", "Name it:", W->input, 64))
        return;
    f_mkdir("/WWW");
    FIL f;
    FILINFO fi;
    bool fresh = f_stat(BOOKMARKS, &fi) != FR_OK;
    if (f_open(&f, BOOKMARKS, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        dlg_message("Bookmark", "Can't write " BOOKMARKS, "OK", true);
        return;
    }
    if (fresh)
        f_printf(&f, "<title>Bookmarks</title>\r\n<h1>Bookmarks</h1>\r\n");
    f_printf(&f, "<li><a href=\"");
    for (const char *s = h->url; *s; ++s)
        if (*s == '"')
            f_printf(&f, "%%22");
        else
            f_putc(*s, &f);
    f_printf(&f, "\">");
    // The name is CP437: &, < and the 8-bit characters are written as entities.
    for (const char *s = W->input; *s; ++s) {
        if (*s == '&')
            f_printf(&f, "&amp;");
        else if (*s == '<')
            f_printf(&f, "&lt;");
        else if ((uint8_t)*s >= 0x80)
            f_printf(&f, "&#%u;", (unsigned)www_unicode((uint8_t)*s));
        else
            f_putc(*s, &f);
    }
    f_printf(&f, "</a>\r\n");
    f_close(&f);
    snprintf(W->note, sizeof W->note, "Bookmarked in " BOOKMARKS);
}

/// A dialog about the page and the link chosen.
static void info(void) {
    const hist_t *h = &W->hist[W->pos];
    const www_doc_t *d = W->doc;
    char text[600];
    int n = snprintf(text, sizeof text, "%.66s\n%.66s\n\n%s, %lu bytes%s\n%ld lines, %ld links, %ld forms",
                     h->title[0] ? h->title : "(no title)", h->url, h->type[0] ? h->type : "text/html",
                     (unsigned long)h->len, h->post ? " (a form's reply)" : "", (long)d->nlines,
                     (long)d->nlinks, (long)d->nforms);
    if (W->cur >= 0 && visible(W->cur)) {
        char link[WWW_URL_MAX + 64];
        describe_link(link, sizeof link);
        snprintf(text + n, sizeof text - n, "\n\nLink: %.66s", link);
    }
    dlg_message("Page info", text, "OK", false);
}

/// The history as a list to pick from.
static void history_list(void) {
    char **items = psram_malloc(W->nhist * sizeof *items);
    if (!items)
        return;
    for (int i = 0; i < W->nhist; ++i)
        items[i] = W->hist[i].title[0] ? W->hist[i].title : W->hist[i].url;
    int i = dlg_list("History", items, W->nhist, W->pos);
    psram_free(items);
    if (i >= 0 && i != W->pos)
        history_go(i - W->pos);
}

// ---- Keys and the mouse -----------------------------------------------------------------------------

/// Does a menu item.
/// @param id the item
static void command(int id) {
    switch (id) {
    case M_OPEN:
        W->input[0] = 0;
        if (dlg_input("Open", "Address, or words to search for:", W->input, WWW_URL_MAX - 8))
            open_typed(W->input);
        break;
    case M_FILE:
        W->input[0] = 0;
        if (dlg_input("Open file", "File on the drive (e.g. /DOCS/PAGE.HTM):", W->input, 200))
            open_typed(W->input);
        break;
    case M_RELOAD: {
        hist_t *h = &W->hist[W->pos];
        remember_place();
        int32_t top = h->top;
        int cur = h->cur;
        snprintf(W->url2, sizeof W->url2, "%s", h->url);
        if (load(W->url2, h->post, false)) {
            W->top = top;
            W->cur = cur;
            clamp_top();
            if (!visible(W->cur))
                W->cur = first_visible();
        }
        break;
    }
    case M_SAVE_SRC: {
        const hist_t *h = &W->hist[W->pos];
        char name[96];
        name_from_url(h->url, name, sizeof name);
        if (h->src)
            save_bytes("Save source", name, h->src, h->len);
        break;
    }
    case M_SAVE_TEXT: save_text(); break;
    case M_DOWNLOAD: download(); break;
    case M_QUIT: W->quit = true; break;
    case M_BACK: history_go(-1); break;
    case M_FORWARD: history_go(1); break;
    case M_HOME: go_home(); break;
    case M_HISTORY: history_list(); break;
    case M_FIND: find(); break;
    case M_FIND_NEXT:
        if (W->find[0])
            find_next();
        else
            find();
        break;
    case M_TOP:
        W->top = 0;
        W->cur = first_visible();
        break;
    case M_END: {
        W->top = max_top();
        int32_t s = www_seg_from(W->doc, W->top + PAGE_ROWS) - 1;  // the last link on the screen
        W->cur = s >= 0 && W->doc->segs[s].line >= W->top ? W->doc->segs[s].link : -1;
        break;
    }
    case M_SOURCE:
        remember_place();
        W->source = !W->source;
        show(NULL);
        break;
    case M_INFO: info(); break;
    case M_IMAGE: view_image(); break;
    case M_VERIFY:
        W->verify = !W->verify;
        snprintf(W->note, sizeof W->note, W->verify ? "https certificates are checked"
                                                     : "https certificates are NOT checked");
        break;
    case M_MARK: add_bookmark(); break;
    case M_MARKS: follow("file://" BOOKMARKS, NULL); break;
    case M_HELP: follow("about:help", NULL); break;
    case M_ABOUT:
        dlg_message("About", "www " VER_WWW ": a text web browser for pico-os\nin the manner of lynx\n\n"
                    "Copyright (c) 2026 Ken Segler, MIT licence", "OK", false);
        break;
    }
}

/// Opens the menus at menu `open`.
/// @param open the menu
static void run_menu(int open) {
    W->view[4].check = W->verify;
    int id = menu_run(W->menus, NMENUS, open);
    if (id > 0)
        command(id);
}

/// The right button's pop-up menu: what it can do (the link items only over a link).
static const menu_item_t popup_items[] = {
    {"Follow link", M_FOLLOW, "Enter", false},  {"Download link...", M_DOWNLOAD, "d", false},
    {"View image", M_IMAGE, "i", false},        {"-", 0, NULL, false},
    {"Back", M_BACK, "Left", false},            {"Forward", M_FORWARD, "Ctrl-F", false},
    {"Reload", M_RELOAD, "r", false},           {"Start page", M_HOME, "m", false},
    {"-", 0, NULL, false},
    {"Bookmark this page...", M_MARK, "a", false}, {"Find...", M_FIND, "/", false},
    {"Page source", M_SOURCE, "\\", false},    {"Save as text...", M_SAVE_TEXT, "p", false},
    {"Page info", M_INFO, "=", false},
};
/// Their count.
#define NPOPUP (int)(sizeof popup_items / sizeof *popup_items)

/// Shows the pop-up menu at a cell and runs the item chosen (by the keys or
/// the mouse: a click, or letting go of the right button over an item).
/// @param row the row clicked
/// @param col the column
/// @param link true if the click was on a link (its items are shown)
static void popup(int row, int col, bool link) {
    const menu_item_t *items[NPOPUP];
    int n = 0, w = 0;
    const www_link_t *l = link ? &W->doc->links[W->cur] : NULL;
    for (int i = 0; i < NPOPUP; ++i) {
        int id = popup_items[i].id;
        // Over a link: its items (View image if it has a picture). Elsewhere, from Back.
        if ((id == M_FOLLOW || id == M_DOWNLOAD || (id == 0 && i < 4)) && !l)
            continue;
        if (id == M_FOLLOW && l->type == WL_IMAGE)
            continue;
        if (id == M_IMAGE && (!l || !l->img))
            continue;
        if (id == M_DOWNLOAD && l->type != WL_HREF && l->type != WL_IMAGE)
            continue;
        items[n++] = &popup_items[i];
        int len = (int)strlen(popup_items[i].text) +
                  (popup_items[i].key ? (int)strlen(popup_items[i].key) + 2 : 0);
        if (len > w)
            w = len;
    }
    w += 4;
    int h = n + 2;
    if (col + w > SCR_W)
        col = SCR_W - w;
    if (row + h > SCR_H - 1)
        row = SCR_H - 1 - h;
    if (row < 1)
        row = 1;
    void *saved = scr_save();
    int sel = -1, id = 0;
    bool moved = false;  // the pointer has left where the menu opened
    for (;;) {
        scr_fill(row, col, h, w, ' ', C_MENU);
        scr_box(row, col, h, w, false, C_MENU);
        for (int i = 0; i < n; ++i) {
            if (items[i]->text[0] == '-') {
                scr_hline(row + 1 + i, col, w, 0xc3, S_H, 0xb4, C_MENU);
                continue;
            }
            uint8_t at = i == sel ? C_MENU_SEL : C_MENU;
            scr_fill(row + 1 + i, col + 1, 1, w - 2, ' ', at);
            scr_text(row + 1 + i, col + 2, -1, at, items[i]->text);
            if (items[i]->key)
                scr_text(row + 1 + i, col + w - 2 - (int)strlen(items[i]->key), -1, at, items[i]->key);
        }
        scr_cursor(-1, 0);
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE) {
            int r = e.y / 16, c = e.x / 8, i = r - row - 1;
            bool inside = c > col && c < col + w - 1 && i >= 0 && i < n && items[i]->text[0] != '-';
            if (e.type == MOUSE_MOVE) {
                moved = true;
                sel = inside ? i : -1;
            } else if (e.type == MOUSE_WHEEL) {
                continue;
            } else if (inside && (e.type == MOUSE_DOWN || (e.type == MOUSE_UP && moved))) {
                id = items[i]->id;
                break;
            } else if (e.type == MOUSE_DOWN) {
                break;  // a click outside closes it
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        if (k == '\r' && sel >= 0) {
            id = items[sel]->id;
            break;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int d = k == KEY_UP ? n - 1 : 1;
            sel = sel < 0 ? (k == KEY_UP ? n - 1 : 0) : (sel + d) % n;
            while (items[sel]->text[0] == '-')
                sel = (sel + d) % n;
        }
    }
    scr_restore(saved);
    if (id == M_FOLLOW)
        activate();
    else if (id)
        command(id);
}

/// Handles a key.
/// @param k the key
static void handle(int k) {
    if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
        for (int i = 0; i < NMENUS; ++i)
            if (tolower((unsigned char)menu_titles[i][0]) == k - KEY_ALT) {
                run_menu(i);
                return;
            }
        return;
    }
    switch (k) {
    case KEY_DOWN: case '\t': next_link(1); break;
    case KEY_UP: next_link(-1); break;
    case KEY_PGDN: case ' ': case '+':
        W->top += PAGE_ROWS - 1;
        clamp_top();
        if (!visible(W->cur))
            W->cur = first_visible();
        break;
    case KEY_PGUP: case 'b': case '-':
        W->top -= PAGE_ROWS - 1;
        clamp_top();
        if (!visible(W->cur))
            W->cur = first_visible();
        break;
    case KEY_HOME: command(M_TOP); break;
    case KEY_END: command(M_END); break;
    case '\r': case KEY_RIGHT: activate(); break;
    case KEY_LEFT: case '\b': case KEY_F4: command(M_BACK); break;
    case CTRL('f'): command(M_FORWARD); break;
    case 'g': case 'u': command(M_OPEN); break;
    case 'G': case KEY_F2:
        snprintf(W->input, sizeof W->input, "%s", W->hist[W->pos].url);
        if (dlg_input("Open", "Address, or words to search for:", W->input, WWW_URL_MAX - 8))
            open_typed(W->input);
        break;
    case 'o': command(M_FILE); break;
    case '/': case KEY_F3: command(M_FIND); break;
    case 'n': command(M_FIND_NEXT); break;
    case '\\': case KEY_F6: command(M_SOURCE); break;
    case 'r': case KEY_F5: case CTRL('r'): command(M_RELOAD); break;
    case 'm': command(M_HOME); break;
    case 'h': case '?': case KEY_F1: command(M_HELP); break;
    case 'a': case KEY_F7: command(M_MARK); break;
    case 'v': case KEY_F8: command(M_MARKS); break;
    case 'd': command(M_DOWNLOAD); break;
    case 's': command(M_SAVE_SRC); break;
    case 'p': command(M_SAVE_TEXT); break;
    case '=': command(M_INFO); break;
    case 'i': command(M_IMAGE); break;
    case CTRL('y'): command(M_HISTORY); break;
    case KEY_F9: run_menu(0); break;
    case 'q': case 'Q': case KEY_F10: case CTRL('c'): command(M_QUIT); break;
    case KEY_ESC: W->find_line = -1; break;
    }
}

/// The mouse: a click follows a link, the right button goes back, the wheel
/// scrolls; the menu bar, key bar and scroll bar work.
/// @param e the event
static void mouse_click(const mouse_event_t *e) {
    int row = e->y / 16, col = e->x / 8;
    if (e->type == MOUSE_WHEEL) {
        W->top += e->wheel > 0 ? -3 : 3;
        clamp_top();
        if (!visible(W->cur))
            W->cur = first_visible();
        return;
    }
    if (e->type != MOUSE_DOWN)
        return;
    if (e->button == MOUSE_RIGHT) {  // the pop-up menu (over a link: it is chosen)
        bool on_link = false;
        if (row >= PAGE_TOP && row < PAGE_TOP + PAGE_ROWS) {
            int32_t line = W->top + row - PAGE_TOP;
            const www_doc_t *d = W->doc;
            for (int32_t s = www_seg_from(d, line); s < d->nsegs && d->segs[s].line == line; ++s)
                if (col - 1 >= d->segs[s].col && col - 1 < d->segs[s].col + d->segs[s].len) {
                    W->cur = d->segs[s].link;
                    on_link = true;
                }
        }
        if (on_link) {  // the link shows as chosen under the menu
            draw();
            scr_flush();
        }
        popup(row + 1, col, on_link);
        return;
    }
    if (row == 0) {
        for (int i = 0; i < NMENUS; ++i)
            if (col >= menu_col(i) && col < menu_col(i) + (int)strlen(menu_titles[i]) + 4)
                run_menu(i);
        return;
    }
    if (row == SCR_H - 1) {
        for (int i = 0; i < NKEYS; ++i)
            if (col >= W->key_col[i] && col < W->key_col[i + 1])
                handle(keys[i].code);
        return;
    }
    if (row < PAGE_TOP || row >= PAGE_TOP + PAGE_ROWS)
        return;
    if (col == SCR_W - 1 && W->doc->nlines > PAGE_ROWS) {  // the scroll bar
        W->top = (int32_t)((int64_t)max_top() * (row - PAGE_TOP) / (PAGE_ROWS - 1));
        clamp_top();
        if (!visible(W->cur))
            W->cur = first_visible();
        return;
    }
    int32_t line = W->top + row - PAGE_TOP;
    const www_doc_t *d = W->doc;
    for (int32_t s = www_seg_from(d, line); s < d->nsegs && d->segs[s].line == line; ++s)
        if (col - 1 >= d->segs[s].col && col - 1 < d->segs[s].col + d->segs[s].len) {
            W->cur = d->segs[s].link;
            activate();
            return;
        }
}

void www_command(int argc, char **argv) {
    if (scr_active()) {
        printf("www: it can't open inside mc\n");
        return;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        printf("usage: www [address | file | words to search for]\n");
        return;
    }
    W = psram_malloc(sizeof *W);
    if (!W) {
        printf("www: not enough memory\n");
        return;
    }
    memset(W, 0, sizeof *W);
    W->verify = true;
    W->cur = -1;
    W->cap_driver.out_chars = cap_out;
    // The menus: menu_run takes them as they are (it never changes the items),
    // and View's tick is set in a copy.
    memcpy(W->view, view_items, sizeof view_items);
    const menu_item_t *items[NMENUS] = {file_items, nav_items, W->view, mark_items, help_items};
    for (int i = 0; i < NMENUS; ++i)
        W->menus[i] = (menu_t){menu_titles[i], (menu_item_t *)items[i]};
    W->find_line = -1;
    W->doc = www_layout("", 0, false, false);
    if (!W->doc) {
        psram_free(W);
        W = NULL;
        printf("www: not enough memory\n");
        return;
    }
    // An empty first entry, until a page loads.
    W->nhist = 1;
    W->hist[0].cur = -1;
    snprintf(W->hist[0].url, sizeof W->hist[0].url, "about:blank");
    scr_begin();
    mouse_pointer(MOUSE_POINTER_TEXT);
    draw();
    scr_flush();
    bool loaded = argc == 2 ? (open_typed(argv[1]), W->nhist > 1) : (go_home(), W->nhist > 1);
    if (loaded) {  // the empty entry goes
        hist_free(&W->hist[0]);
        memmove(W->hist, W->hist + 1, (W->nhist - 1) * sizeof *W->hist);
        --W->nhist;
        --W->pos;
    }
    while (!W->quit) {
        mouse_pointer(MOUSE_POINTER_TEXT);
        draw();
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k != KEY_MOUSE || e.type == MOUSE_DOWN)
            W->note[0] = 0;
        if (k == KEY_MOUSE)
            mouse_click(&e);
        else
            handle(k);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    scr_end();
    for (int i = 0; i < W->nhist; ++i)
        hist_free(&W->hist[i]);
    www_free(W->doc);
    www_net_end();
    psram_free(W);
    W = NULL;
}
