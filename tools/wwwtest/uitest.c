/**
 * @file uitest.c
 * @brief The web browser on a PC: its real screen, menus, dialogs, fetcher
 *        and page engine against a pretend web server, driven by scripted
 *        keys and mouse clicks, with checks of what the screen shows and
 *        screenshots.
 *
 *     ./uitest.sh  (builds this, writes www_*.png into the folder given)
 *
 * The pretend server answers any host: / (a demo page, sent chunked),
 * /redirect (a 302 that sets a cookie), /pics (pictures: /pic.bmp and
 * /pic.gif), /cookie (shows the cookie sent back),
 * /search?q= (shows the query), /post (shows the form data posted), /long (a
 * long page), /file.bin (a download) and anything else a 404.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mc.h"
#include "mouse.h"
#include "net.h"
#include "pico/stdlib.h"
#include "www.h"

#undef printf  // (the test's own printing goes to the terminal)

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< (mc's overlay mode, unused)
stdio_driver_t console_stdio_driver;          ///< the console's driver
bool pc_psram_full;                           ///< PSRAM has run out
static stdio_driver_t *capture_driver;        ///< another driver that is on (the browser's)
static bool console_on = true;                ///< the console's driver is on
static uint64_t now_us = 1000000;             ///< the clock
static uint8_t cell_ch[SCR_H][SCR_W], cell_at[SCR_H][SCR_W];  ///< the screen
static int failures;                          ///< checks that failed
static int requests;                          ///< requests the server answered

// ---- The board's functions ------------------------------------------------------------------------

void stdio_set_driver_enabled(stdio_driver_t *d, bool on) {
    if (d == &console_stdio_driver)
        console_on = on;
    else
        capture_driver = on ? d : NULL;
}

int pc_printf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
        n = sizeof buf - 1;
    if (capture_driver)
        capture_driver->out_chars(buf, n);
    else if (console_on && getenv("VERBOSE"))
        fputs(buf, stdout);
    return n;
}

/// The 16 PC colours.
static const uint8_t vga[16][3] = {
    {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170}, {170, 0, 0}, {170, 0, 170}, {170, 85, 0},
    {170, 170, 170}, {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255}, {255, 85, 85},
    {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

/// A PC colour as RGB332.
/// @param c the colour (0-15)
/// @return it
static uint8_t rgb332(int c) {
    return (uint8_t)((vga[c][0] >> 5) << 5 | (vga[c][1] >> 5) << 2 | vga[c][2] >> 6);
}

void console_set_cell(int row, int col, uint8_t ch, uint8_t attr) {
    cell_ch[row][col] = ch;
    cell_at[row][col] = attr;
    for (int y = 0; y < 16; ++y)  // the pixels too, as the console draws them
        for (int x = 0; x < 8; ++x)
            dvi_framebuf[(row * 16 + y) * DVI_WIDTH + col * 8 + x] =
                rgb332(font8x16[ch][y] & (0x80 >> x) ? attr & 15 : attr >> 4);
}
void console_get_cell(int row, int col, uint8_t *ch, uint8_t *attr) {
    *ch = cell_ch[row][col];
    *attr = cell_at[row][col];
}
void console_get_cursor(int *row, int *col) { *row = *col = 0; }
void console_freeze(bool on) { (void)on; }
uint64_t time_us_64(void) { return now_us; }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }
absolute_time_t make_timeout_time_ms(uint32_t ms) { return now_us + ms * 1000ull; }
bool time_reached(absolute_time_t t) { return now_us >= t; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
void mouse_pointer_hide(void) {}
void mouse_pointer_show(void) {}
void mouse_get(int *x, int *y, int *buttons) { *x = *y = *buttons = 0; }

// ---- FatFs on the test's folder -------------------------------------------------------------------

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param path the path
/// @return the local one
static const char *local(const char *path) {
    static char buf[2][300];
    static int k;
    k ^= 1;
    snprintf(buf[k], sizeof buf[k], ".%s%s", path[0] == '/' ? "" : "/", path);
    return buf[k];
}
FRESULT f_open(FIL *f, const char *path, int mode) {
    struct stat st;
    bool there = !stat(local(path), &st);
    if ((mode & FA_CREATE_NEW) && there)
        return FR_EXIST;
    const char *how = mode == FA_READ ? "rb" : (mode & FA_OPEN_APPEND) == FA_OPEN_APPEND ? "ab" : "wb";
    f->f = fopen(local(path), how);
    return f->f ? FR_OK : FR_NO_FILE;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }
FSIZE_t f_size(FIL *f) {
    long at = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long n = ftell(f->f);
    fseek(f->f, at, SEEK_SET);
    return (FSIZE_t)n;
}
FRESULT f_lseek(FIL *f, FSIZE_t at) { return fseek(f->f, (long)at, SEEK_SET) ? 1 : FR_OK; }
FSIZE_t f_tell(FIL *f) { return (FSIZE_t)ftell(f->f); }
FRESULT f_stat(const char *path, FILINFO *fi) {
    struct stat st;
    if (stat(local(path), &st))
        return FR_NO_FILE;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fi->fsize = (FSIZE_t)st.st_size;
    return FR_OK;
}
FRESULT f_getcwd(char *buf, UINT n) { snprintf(buf, n, "/"); return FR_OK; }
FRESULT f_unlink(const char *path) { return remove(local(path)) ? FR_NO_FILE : FR_OK; }
FRESULT f_mkdir(const char *path) { mkdir(local(path), 0755); return FR_OK; }
int f_printf(FIL *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f->f, fmt, ap);
    va_end(ap);
    return n;
}
int f_putc(char c, FIL *f) { return fputc(c, f->f); }

// ---- The pretend web server -------------------------------------------------------------------------

/// A connection to the pretend server.
struct net_conn {
    char host[128];   ///< who it's to
    char req[8192];   ///< the request so far
    int reqlen;       ///< its length
    char *resp;       ///< the reply, once the request is in
    int resplen;      ///< its length
    int at;           ///< how much has been read
};

/// The demo page.
static const char demo_page[] =
    "<html><head><title>pico-os demo page</title></head><body>"
    "<h1>Welcome to the demo page</h1>"
    "<p>This page shows what <b>www</b> does with HTML: <i>styles</i>, <code>code</code>, "
    "<a href=\"/long\">links</a> that wrap round the end of a line when they are long, like "
    "<a href=\"/redirect\">this one, which redirects and sets a cookie on the way</a>, and "
    "characters such as caf&eacute;, &ldquo;quotes&rdquo; and 25&deg;C.</p>"
    "<h2>A list</h2><ul><li>First item<li>Second, with <a href=\"/file.bin\">a file to download</a>"
    "<ul><li>Nested</li></ul><li>Third</ul>"
    "<h2>A form</h2><form action=\"/search\">Search: <input name=q size=24 value=\"pico\"> "
    "<select name=lang><option value=en>English<option value=de>Deutsch</select> "
    "<input type=checkbox name=safe checked> safe <input type=submit value=\"Go\"></form>"
    "<form action=\"/post\" method=post>Message: <input name=msg value=\"hello\"> "
    "<input type=hidden name=id value=\"7\"><input type=submit value=\"Post it\"></form>"
    "<h2>A table</h2><table><tr><th>Board</th><th>Chip</th></tr><tr><td>Pico 2</td><td>RP2350</td></tr></table>"
    "<pre>  printf(\"hello\\n\");</pre><hr><p id=end>The end. <a href=\"#end\">Here</a>, "
    "<a href=\"mailto:a@b.c\">mail</a> and <a href=\"/missing\">a missing page</a>.</p></body></html>";

/// Makes the reply to a request.
/// @param c the connection
static void answer(struct net_conn *c) {
    char method[8], path[2048];
    sscanf(c->req, "%7s %2047s", method, path);
    const char *body = strstr(c->req, "\r\n\r\n") + 4;
    char cookie[256] = "";
    const char *ck = strstr(c->req, "\r\nCookie: ");
    if (ck)
        snprintf(cookie, sizeof cookie, "%.*s", (int)strcspn(ck + 10, "\r"), ck + 10);
    static char page[200000];
    char head[512] = "";
    const char *type = "text/html; charset=utf-8";
    int code = 200, n = 0;
    bool chunked = false;
    if (!strcmp(path, "/")) {
        n = snprintf(page, sizeof page, "%s", demo_page);
        chunked = true;
    } else if (!strcmp(path, "/redirect")) {
        code = 302;
        snprintf(head, sizeof head, "Location: /cookie\r\nSet-Cookie: session=abc123; Path=/\r\n");
    } else if (!strcmp(path, "/cookie")) {
        n = snprintf(page, sizeof page, "<title>Cookie</title><p>Cookie sent: [%s]</p>", cookie);
    } else if (!strncmp(path, "/search?", 8)) {
        n = snprintf(page, sizeof page, "<title>Results</title><p>Query: [%s]</p><ol>"
                     "<li><a href=\"/\">Result one</a><li><a href=\"/long\">Result two</a></ol>", path + 8);
    } else if (!strcmp(path, "/post") && !strcmp(method, "POST")) {
        n = snprintf(page, sizeof page, "<title>Posted</title><p>Posted: [%s]</p>", body);
    } else if (!strcmp(path, "/long")) {
        n = snprintf(page, sizeof page, "<title>A long page</title><h1>A long page</h1>");
        for (int i = 1; i <= 120; ++i)
            n += snprintf(page + n, sizeof page - n, "<p>Paragraph %d. Some text to read %s</p>%s", i,
                          i == 77 ? "with the word needle in it." : "on the way down.",
                          i % 10 == 0 ? "<p><a href=\"/\">Back to the demo</a></p>" : "");
    } else if (!strcmp(path, "/pics")) {
        n = snprintf(page, sizeof page, "<title>Pictures</title><h1>Pictures</h1>"
                     "<p><img src=\"pic.bmp\" alt=\"A test picture\"> beside text</p>"
                     "<p><a href=\"/\"><img src=\"/pic.gif\" alt=\"logo\"> Home</a></p>"
                     "<p><a href=\"/long\"><img src=\"/pic2.bmp\" width=128 height=96 "
                     "alt=\"Linked picture\"></a></p><p>After the pictures.</p>");
    } else if (!strcmp(path, "/pic.bmp") || !strcmp(path, "/pic2.bmp")) {  // a 64 x 48 colour ramp, 24-bit BMP
        static const uint8_t hdr[54] = {'B', 'M', 0x36, 0x24, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                                        64, 0, 0, 0, 48, 0, 0, 0, 1, 0, 24, 0};
        memcpy(page, hdr, sizeof hdr);
        n = sizeof hdr;
        for (int y = 47; y >= 0; --y)  // bottom row first, BGR
            for (int x = 0; x < 64; ++x) {
                page[n++] = (char)(255 - x * 4);
                page[n++] = (char)(y * 5);
                page[n++] = (char)(x * 4);
            }
        type = "image/bmp";
    } else if (!strcmp(path, "/pic.gif")) {
        n = snprintf(page, sizeof page, "GIF89a not really");
        type = "image/gif";
    } else if (!strcmp(path, "/file.bin")) {
        type = "application/octet-stream";
        for (n = 0; n < 1000; ++n)
            page[n] = (char)n;
    } else {
        code = 404;
        n = snprintf(page, sizeof page, "<title>Not found</title><h1>404</h1><p>No %s here.</p>", path);
    }
    c->resp = malloc(n * 2 + 1024);
    int k = snprintf(c->resp, 1024, "HTTP/1.1 %d X\r\nContent-Type: %s\r\n%s%s\r\n", code, type, head,
                     chunked ? "Transfer-Encoding: chunked\r\n" : "");
    if (chunked) {  // in chunks of 100 bytes
        for (int at = 0; at < n; at += 100) {
            int len = n - at < 100 ? n - at : 100;
            k += sprintf(c->resp + k, "%x\r\n", len);
            memcpy(c->resp + k, page + at, len);
            k += len;
            k += sprintf(c->resp + k, "\r\n");
        }
        k += sprintf(c->resp + k, "0\r\n\r\n");
    } else {
        memcpy(c->resp + k, page, n);
        k += n;
    }
    c->resplen = k;
    ++requests;
}

net_conn_t *net_conn_open(const char *host, uint16_t port) {
    pc_printf("Connecting to %s port %u...\n", host, port);
    struct net_conn *c = calloc(1, sizeof *c);
    snprintf(c->host, sizeof c->host, "%s", host);
    return c;
}
bool net_conn_write(net_conn_t *c, const void *data, size_t len) {
    if (c->reqlen + (int)len >= (int)sizeof c->req)
        return false;
    memcpy(c->req + c->reqlen, data, len);
    c->reqlen += (int)len;
    c->req[c->reqlen] = 0;
    char *end = strstr(c->req, "\r\n\r\n");
    const char *cl = strstr(c->req, "Content-Length: ");
    if (end && !c->resp && (!cl || c->reqlen >= (end + 4 - c->req) + atoi(cl + 16)))
        answer(c);
    return true;
}
int net_conn_read(net_conn_t *c, void *buf, size_t max) {
    if (!c->resp)
        return 0;
    if (c->at == c->resplen)
        return -1;
    int n = c->resplen - c->at;
    if (n > (int)max)
        n = (int)max;
    if (n > 700)
        n = 700;  // the reply comes in pieces
    memcpy(buf, c->resp + c->at, n);
    c->at += n;
    return n;
}
void net_conn_close(net_conn_t *c) {
    if (c) {
        free(c->resp);
        free(c);
    }
}
bool net_need_link(void) { return true; }
bool net_stop_key(void) { return false; }
bool net_tls_prepare(void) { return true; }
net_tls_t *net_tls_start(net_conn_t *c, const char *host, bool verify) {
    (void)host;
    (void)verify;
    return (net_tls_t *)c;
}
int net_tls_read(net_tls_t *t, void *buf, size_t max) { return net_conn_read((net_conn_t *)t, buf, max); }
bool net_tls_write(net_tls_t *t, const void *data, size_t len) {
    return net_conn_write((net_conn_t *)t, data, len);
}
void net_tls_end(net_tls_t *t) { (void)t; }

// ---- Screenshots and checks ---------------------------------------------------------------------------


/// Writes the screen as 640x480 RGB bytes to shot_<name>.raw.
/// @param name the screenshot's name
static void shot(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "shot_%s.raw", name);
    FILE *f = fopen(path, "wb");
    for (int y = 0; y < DVI_HEIGHT; ++y)
        for (int x = 0; x < DVI_WIDTH; ++x) {
            int r = y / 16, c = x / 8;
            uint8_t at = cell_at[r][c];
            bool on = font8x16[cell_ch[r][c]][y % 16] & (0x80 >> (x % 8));
            fwrite(vga[on ? at & 15 : at >> 4], 1, 3, f);
        }
    fclose(f);
}

/// Writes the pixels (a picture over the text) as RGB bytes to shot_<name>.raw.
/// @param name the screenshot's name
static void shot_pixels(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "shot_%s.raw", name);
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < DVI_WIDTH * DVI_HEIGHT; ++i) {
        uint8_t v = dvi_framebuf[i];
        uint8_t px[3] = {(uint8_t)((v >> 5) * 255 / 7), (uint8_t)(((v >> 2) & 7) * 255 / 7), (uint8_t)((v & 3) * 85)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/// Whether the screen shows `text`.
/// @param text the text
/// @return it does
static bool screen_has(const char *text) {
    size_t n = strlen(text);
    for (int r = 0; r < SCR_H; ++r)
        for (int c = 0; c + (int)n <= SCR_W; ++c)
            if (!memcmp(&cell_ch[r][c], text, n))
                return true;
    return false;
}

/// Prints the screen (for a failed check).
static void dump_screen(void) {
    for (int r = 0; r < SCR_H; ++r) {
        printf("|");
        for (int c = 0; c < SCR_W; ++c)
            putchar(cell_ch[r][c] >= 32 && cell_ch[r][c] < 127 ? cell_ch[r][c] : '.');
        printf("|\n");
    }
}

// ---- The script --------------------------------------------------------------------------------------

/// What a step of the script does.
enum {
    S_KEYS,     ///< types s
    S_SHOT,     ///< takes screenshot s
    S_PIXELS,   ///< takes screenshot s of the pixels (a picture shown)
    S_SEE,      ///< checks the screen shows s
    S_NOTSEE,   ///< checks it doesn't
    S_FILE,     ///< checks file s has size a
    S_CLICK,    ///< a left click at column a, row b (button c if set)
    S_WHEEL,    ///< the wheel, a steps
    S_PAUSE,    ///< no key for a moment (to end a bare Esc)
    S_END,      ///< the end
};

/// A step.
typedef struct {
    int what;       ///< S_
    const char *s;  ///< its text
    int a, b, c;    ///< its numbers
} step_t;

#define DOWN  "\x1b[B"   ///< the arrow keys
#define UP    "\x1b[A"   ///< up
#define LEFT  "\x1b[D"   ///< left
#define RIGHT "\x1b[C"   ///< right
#define PGDN  "\x1b[6~"  ///< page down
#define F9    "\x1b[20~" ///< F9

/// The script.
static const step_t script[] = {
    // The built-in start page (no HOME.HTM on the "drive").
    {S_SEE, "pico-os web browser"},
    {S_SEE, "Search the web:"},
    {S_SHOT, "home"},
    // Open the demo page by typing its address.
    {S_KEYS, "g"},
    {S_SEE, "Address, or words to search for"},
    {S_KEYS, "http://demo.test/\r"},
    {S_SEE, "Welcome to the demo page"},
    {S_SEE, "caf\x82, \"quotes\" and 25\xf8" "C"},
    {S_SEE, "pico-os demo page"},
    {S_SHOT, "demo"},
    // The second link (the redirect): it sets a cookie, which /cookie shows.
    {S_KEYS, DOWN},
    {S_SEE, "http://demo.test/redirect"},
    {S_KEYS, "\r"},
    {S_SEE, "Cookie sent: [session=abc123]"},
    {S_KEYS, LEFT},
    {S_SEE, "Welcome to the demo page"},
    // The menus.
    {S_KEYS, F9},
    {S_SEE, "Open address..."},
    {S_KEYS, RIGHT},
    {S_SEE, "History..."},
    {S_SHOT, "menu"},
    {S_KEYS, "\x1b"},
    {S_PAUSE, NULL, 0, 0, 0},
    {S_NOTSEE, "History..."},
    // Forward again, then back.
    {S_KEYS, "\x06"},
    {S_SEE, "Cookie sent"},
    {S_KEYS, "\b"},
    // The search form: edit the field (Enter sends it: the form has one text field).
    {S_KEYS, DOWN DOWN},
    {S_SEE, "Text field q"},
    {S_KEYS, "\r"},
    {S_SEE, "Text field"},
    {S_SHOT, "field"},
    {S_KEYS, "\x15" "rp2350 board\r"},
    {S_SEE, "Query: [q=rp2350+board&lang=en&safe=on]"},
    {S_SHOT, "results"},
    {S_KEYS, LEFT},
    // The list: choose Deutsch, clear the checkbox, press Go.
    {S_KEYS, DOWN},
    {S_SEE, "List lang"},
    {S_KEYS, "\r" DOWN "\r"},
    {S_SEE, "[Deutsch"},
    {S_KEYS, DOWN "\r" DOWN},
    {S_SEE, "Button: Enter sends the form (get) to http://demo.test/search"},
    {S_KEYS, "\r"},
    {S_SEE, "Query: [q=pico&lang=de]"},
    {S_KEYS, LEFT},
    // The post form.
    {S_KEYS, DOWN DOWN},
    {S_SEE, "sends the form (post)"},
    {S_KEYS, "\r"},
    {S_SEE, "Posted: [msg=hello&id=7]"},
    {S_KEYS, LEFT},
    // A link that wraps, by the mouse: "links" on the first paragraph.
    {S_KEYS, "\x1b[H"},
    {S_CLICK, NULL, 58, 3},
    {S_SEE, "A long page"},
    // Find on the long page.
    {S_KEYS, "/needle\r"},
    {S_SEE, "Paragraph 77. Some text to read with the word needle in it."},
    {S_SHOT, "find"},
    {S_KEYS, "n"},
    {S_SEE, "Found again from the top"},
    {S_WHEEL, NULL, -3},
    {S_KEYS, "\x1b[F"},
    {S_SEE, "100%"},
    {S_KEYS, "\x1b[H"},
    {S_SEE, "A long page"},
    // The source.
    {S_KEYS, "\\"},
    {S_SEE, "<title>A long page</title>"},
    {S_KEYS, "\\"},
    // Back to the demo with the right button; its anchor link; mailto; a 404.
    // Back by the right button's menu (rows 11 and down, from column 10).
    {S_CLICK, NULL, 10, 10, MOUSE_RIGHT},
    {S_SEE, "Bookmark this page..."},
    {S_NOTSEE, "Download link..."},
    {S_CLICK, NULL, 14, 12},
    {S_SEE, "Welcome to the demo page"},
    {S_KEYS, "\x1b[F"},
    {S_SEE, "The end."},
    {S_SEE, "http://demo.test/missing"},
    {S_KEYS, UP},
    {S_SEE, "mailto:a@b.c"},
    {S_KEYS, "\r"},
    {S_SEE, "Can't open mailto links"},
    {S_KEYS, DOWN "\r"},
    {S_SEE, "No /missing here."},
    {S_KEYS, LEFT},
    // Download a file: the link in the list.
    // ... by the right button on it, then Download link: "a file to download", row 10.
    {S_KEYS, "\x1b[H"},
    {S_CLICK, NULL, 22, 10, MOUSE_RIGHT},
    {S_SEE, "http://demo.test/file.bin"},
    {S_SEE, "Download link..."},
    {S_SHOT, "popup"},
    {S_KEYS, DOWN DOWN "\r"},
    {S_SEE, "Save as (on the drive):"},
    {S_KEYS, "\x15" "/FILE.BIN\r"},
    {S_FILE, "FILE.BIN", 1000},
    // Following a link to a file asks the same.
    {S_KEYS, "\r"},
    {S_SEE, "Download (application/octet-stream, 1000 bytes)"},
    {S_KEYS, "\x1b"},
    {S_PAUSE, NULL, 0, 0, 0},
    // Bookmark the page, then look at the bookmarks.
    {S_KEYS, "a"},
    {S_SEE, "Name it:"},
    {S_KEYS, "\r"},
    {S_SEE, "Bookmarked"},
    {S_KEYS, "v"},
    {S_SEE, "Bookmarks"},
    {S_SEE, "pico-os demo page"},
    {S_KEYS, "\r"},
    {S_SEE, "Welcome to the demo page"},
    // Words typed go to the search engine; the history list; page info; help.
    {S_KEYS, "=" },
    {S_SEE, "Page info"},
    {S_SHOT, "info"},
    {S_KEYS, "\r"},
    {S_KEYS, "\x19"},
    {S_SEE, "History"},
    {S_KEYS, "\x1b"},
    {S_PAUSE, NULL, 0, 0, 0},
    // Pictures drawn in the page: one with alt text is a link of its own;
    // Enter shows it on the whole screen.
    {S_KEYS, "ghttp://demo.test/pics\r"},
    {S_PAUSE, NULL, 0, 0, 0},  // (no keys a moment: the pictures load, the page is laid out again)
    {S_PAUSE, NULL, 0, 0, 0},
    {S_PAUSE, NULL, 0, 0, 0},
    {S_PAUSE, NULL, 0, 0, 0},
    {S_PAUSE, NULL, 0, 0, 0},
    {S_PAUSE, NULL, 0, 0, 0},
    {S_NOTSEE, "[A test picture]"},  // (drawn over: its alt text is hidden)
    {S_SEE, "beside text"},
    {S_SEE, "After the pictures."},
    {S_PIXELS, "pics"},
    {S_SEE, "Picture: Enter shows it: http://demo.test/pic.bmp"},
    {S_KEYS, "\r"},
    {S_PIXELS, "image"},
    {S_KEYS, " "},
    {S_SEE, "Picture 64 x 48: http://demo.test/pic.bmp"},
    // A link with a picture: i shows it (a GIF: it can't).
    {S_KEYS, DOWN},
    {S_SEE, "http://demo.test/   (i: its picture)"},
    {S_KEYS, "i"},
    {S_SEE, "Can't show it: not a BMP, PNG or JPEG file"},
    {S_KEYS, "\r"},
    // A picture in a link: Enter follows the link.
    {S_KEYS, DOWN},
    {S_SEE, "http://demo.test/long   (i: its picture)"},
    {S_KEYS, "\r"},
    {S_SEE, "A long page"},
    {S_KEYS, LEFT},
    {S_SEE, "After the pictures."},
    // The right button's menu over the picture: View image, no Follow link.
    {S_CLICK, NULL, 4, 3, MOUSE_RIGHT},
    {S_SEE, "View image"},
    {S_NOTSEE, "Follow link"},
    {S_KEYS, "\x1b"},
    {S_PAUSE, NULL, 0, 0, 0},
    // Text only, then pictures again.
    {S_KEYS, "*"},
    {S_SEE, "Pictures are shown as [alt text] only"},
    {S_SEE, "[A test picture] beside text"},
    {S_KEYS, "*"},
    {S_SEE, "Pictures are drawn in pages"},
    {S_KEYS, "h"},
    {S_SEE, "www: keys"},
    {S_SHOT, "help"},
    {S_KEYS, "q"},
    {S_END, NULL, 0, 0, 0},
};

/// The step running.
static int step;
/// Where in a S_KEYS step's text.
static int key_at;
/// Keys handed out after the script ended (it should have quit).
static int extra;

/// Runs steps that aren't keys; returns at a key, a mouse step or the end.
static void run_steps(void) {
    for (;;) {
        const step_t *s = &script[step];
        switch (s->what) {
        case S_KEYS:
            if (s->s[key_at])
                return;
            key_at = 0;
            ++step;
            continue;
        case S_SHOT:
            shot(s->s);
            break;
        case S_PIXELS:
            shot_pixels(s->s);
            break;
        case S_SEE:
        case S_NOTSEE:
            if (screen_has(s->s) != (s->what == S_SEE)) {
                printf("FAIL at step %d: the screen %s \"%s\"\n", step,
                       s->what == S_SEE ? "doesn't show" : "shows", s->s);
                dump_screen();
                ++failures;
            }
            break;
        case S_FILE: {
            struct stat st;
            if (stat(local(s->s), &st) || st.st_size != s->a) {
                printf("FAIL at step %d: %s isn't %d bytes\n", step, s->s, s->a);
                ++failures;
            }
            break;
        }
        default:
            return;
        }
        ++step;
    }
}

int getchar_timeout_us(uint32_t us) {
    (void)us;
    run_steps();
    const step_t *s = &script[step];
    if (s->what == S_KEYS)
        return (uint8_t)s->s[key_at++];
    if (s->what == S_PAUSE) {
        ++step;
        return -1;
    }
    if (s->what == S_END) {
        if (++extra > 50) {
            printf("FAIL: the browser didn't quit at the end\n");
            dump_screen();
            exit(1);
        }
        return extra % 2 ? 'q' : 0x1b;
    }
    return -1;  // a mouse step: mouse_event has it
}

bool mouse_event(mouse_event_t *e) {
    run_steps();
    const step_t *s = &script[step];
    if (s->what != S_CLICK && s->what != S_WHEEL)
        return false;
    memset(e, 0, sizeof *e);
    if (s->what == S_CLICK) {
        e->type = MOUSE_DOWN;
        e->button = s->c ? (uint8_t)s->c : MOUSE_LEFT;
        e->x = (int16_t)(s->a * 8 + 4);
        e->y = (int16_t)(s->b * 16 + 8);
    } else {
        e->type = MOUSE_WHEEL;
        e->wheel = (int8_t)s->a;
    }
    ++step;
    return true;
}

int main(void) {
    remove("FILE.BIN");
    remove("WWW/BOOKMARK.HTM");
    char *argv[] = {"www", NULL};
    www_command(1, argv);
    run_steps();
    if (script[step].what != S_END) {
        printf("FAIL: the browser quit at step %d\n", step);
        ++failures;
    }
    printf("%d requests answered; %s\n", requests, failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
