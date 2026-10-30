/**
 * @file md_ui.c
 * @brief The `md` command: Markdown files from the drive shown in graphics, in the hex editor's style
 *        (a menu bar as the tracker's, bevelled boxes, the scanout mouse arrow).
 *
 * The document is laid out by md_doc.c for the page's width: headings in
 * big letters, bold, italic, code on a dark band, quotes with a bar, lists,
 * tables, rules, and pictures (PNG, JPEG, BMP) from the drive in their place.
 * Links can be chosen (Tab, or the mouse) and followed: another Markdown
 * file opens here (Back goes back), #name goes to that heading, a picture is
 * shown on the whole screen, and a web address opens in www.
 *
 * Memory: everything is in PSRAM, only while the viewer is open: the text,
 * the document, the pictures drawn (up to a budget; the ones furthest off
 * the screen go first, and are loaded again when they come back) and the
 * off-screen picture each frame is drawn in.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "md.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "image.h"
#include "mc.h"
#include "md_doc.h"
#include "md_ttf.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "usb_keyboard.h"
#include "version.h"
#include "www.h"

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height

#define FILE_MAX   (8u << 20)  ///< the biggest file shown
#define HIST_MAX   32          ///< files remembered for Back
#define MAX_PICS   64          ///< pictures kept drawn
#define PIC_BUDGET (3u << 20)  ///< their pixels, at most
#define MAX_FILES  256         ///< entries the file box lists
#define POOL       (8 * 1024)  ///< room for their names
#define PATH_MAX_  200         ///< the longest path

/// Shift held (the HID modifier bits: left and right).
#define SHIFT_BITS 0x22

/// The colours (RGB332), as in the hex editor.
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles
#define C_CYAN   0x1f  ///< typed text, folders
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the link chosen
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the status line
#define C_GREEN  0x3c  ///< a ticked menu item
#define C_FOUND  0xfc  ///< text found (behind it)
#define C_MD     0x9f  ///< Markdown files in the file box

/// Layout.
#define BAR_H    16    ///< the menu bar
#define STAT_Y   460   ///< the status line
#define VIEW_Y   BAR_H ///< the page's top on the screen
#define VIEW_H   (STAT_Y - VIEW_Y)  ///< and its height
#define MARGIN   12    ///< the page's left margin
#define SB_X     628   ///< the scroll bar
#define SB_W     12    ///< its width
#define PAGE_W   (SB_X - 8 - MARGIN)  ///< the width the text is laid out for
#define SHADOW   6     ///< a drop-down's shadow
#define STEP     32    ///< Up and Down scroll this far
#define STRIP    16    ///< rows of the SRAM strip each frame is drawn in, a piece at a time

/// What a click hits.
enum {
    HIT_PAGE = 1, HIT_SB, HIT_MENU, HIT_ITEM, HIT_MENU_OFF, HIT_ASK, HIT_ROW, HIT_FD_OK, HIT_FD_CANCEL,
    HIT_FD_UP, HIT_IN_OK, HIT_IN_CANCEL, HIT_TOC, HIT_SWALLOW,
};

/// The menus' commands.
enum {
    CMD_OPEN = 1, CMD_RELOAD, CMD_BACK, CMD_FORWARD, CMD_QUIT, CMD_TOC, CMD_SOURCE, CMD_PICTURES, CMD_TOP,
    CMD_END, CMD_FIND, CMD_NEXT, CMD_HELP, CMD_ABOUT, CMD_FOLLOW, CMD_NEXT_LINK, CMD_PREV_LINK, CMD_TTF,
};

/// A menu item.
typedef struct {
    const char *text;  ///< NULL ends; "-" is a line between items
    int id;            ///< CMD_*
    const char *key;   ///< the shortcut shown on the right
} mitem_t;

/// A menu on the bar.
typedef struct {
    const char *title;     ///< its title
    const mitem_t *items;  ///< its items
} mmenu_t;

/// The File menu.
static const mitem_t file_items[] = {
    {"Open...", CMD_OPEN, "Ctrl-O"},  {"Reload", CMD_RELOAD, "F5"}, {"-", 0, NULL},
    {"Back", CMD_BACK, "Bksp"},       {"Forward", CMD_FORWARD, "]"}, {"-", 0, NULL},
    {"Quit", CMD_QUIT, "Esc"},        {NULL, 0, NULL},
};
/// The View menu.
static const mitem_t view_items[] = {
    {"Contents...", CMD_TOC, "Ctrl-T"}, {"-", 0, NULL},
    {"Source text", CMD_SOURCE, "F6"},  {"Pictures", CMD_PICTURES, "*"},
    {"TrueType text", CMD_TTF, "F7"},   {"-", 0, NULL},
    {"Top", CMD_TOP, "Home"},           {"End", CMD_END, "End"},          {NULL, 0, NULL},
};
/// The Search menu.
static const mitem_t search_items[] = {
    {"Find...", CMD_FIND, "Ctrl-F"},      {"Find next", CMD_NEXT, "F3"},       {"-", 0, NULL},
    {"Next link", CMD_NEXT_LINK, "Tab"},  {"Previous link", CMD_PREV_LINK, "Shift-Tab"},
    {"Follow the link", CMD_FOLLOW, "Enter"}, {NULL, 0, NULL},
};
/// The Help menu.
static const mitem_t help_items[] = {
    {"Keys", CMD_HELP, "F1"}, {"About", CMD_ABOUT, ""}, {NULL, 0, NULL},
};
/// The right button's menu.
static const mitem_t popup_items[] = {
    {"Follow the link", CMD_FOLLOW, "Enter"}, {"Back", CMD_BACK, "Bksp"}, {"Forward", CMD_FORWARD, "]"},
    {"-", 0, NULL},                           {"Contents...", CMD_TOC, "Ctrl-T"},
    {"Find...", CMD_FIND, "Ctrl-F"},          {"Source text", CMD_SOURCE, "F6"},
    {"Reload", CMD_RELOAD, "F5"},             {NULL, 0, NULL},
};
/// The menus.
static const mmenu_t menus[] = {
    {"File", file_items}, {"View", view_items}, {"Search", search_items}, {"Help", help_items},
};
#define NMENUS 4      ///< their count
#define MAX_ITEMS 12  ///< the most items a menu has

/// Things to do after a question.
enum { ACT_NONE, ACT_WWW };
/// The boxes over the page.
enum { DLG_NONE, DLG_ASK, DLG_FILE, DLG_FIND, DLG_TOC, DLG_HELP, DLG_ABOUT };

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A picture drawn, ready to copy onto the page.
typedef struct {
    int item;       ///< its item (-1: the slot is free)
    uint8_t *px;    ///< its pixels (RGB332, w x h), or NULL if it can't be shown
    uint16_t w, h;  ///< their size
    uint32_t used;  ///< when it was last on the screen
} pic_t;

/// A file visited.
typedef struct {
    char path[PATH_MAX_];  ///< it
    int32_t top;           ///< where it was scrolled to
} hist_t;

/// An entry in the file box.
typedef struct {
    uint16_t name;  ///< its name, in the pool
    bool dir;       ///< a folder
    uint32_t size;  ///< its size
} fd_entry_t;

/// Everything the viewer keeps (PSRAM, while it runs).
typedef struct {
    // The file and its document.
    char path[PATH_MAX_];  ///< the file shown ("" none)
    char dir[PATH_MAX_];   ///< its folder (pictures and links are found from it)
    char *text;            ///< its text
    uint32_t len;          ///< its length
    md_doc_t *doc;         ///< it laid out
    bool source;           ///< showing the text as it is
    bool pictures;         ///< pictures shown (else boxes with their alt text)
    int32_t top;           ///< the page's first pixel row shown
    // History.
    hist_t hist[HIST_MAX]; ///< the files visited
    int nhist;             ///< how many
    int hpos;              ///< the one shown
    // Links.
    int cur_item;          ///< the item of the link chosen (-1: none)
    int cur_link;          ///< the link chosen (0: none)
    int hover_link;        ///< the link under the pointer (0: none)
    // Pictures.
    pic_t pics[MAX_PICS];  ///< drawn
    uint32_t pic_bytes;    ///< their pixels
    uint32_t tick;         ///< a clock for pics' used
    bool pic_wanted;       ///< a picture on the screen isn't drawn yet
    // Find.
    char find[64];         ///< the text looked for
    int find_item;         ///< where it was found (-1: nowhere)
    int find_off;          ///< at which character
    char in[132];          ///< what is typed in the Find box
    // The menus.
    int menu;              ///< the menu open (-1: none; NMENUS: the right button's)
    int msel;              ///< its item chosen (-1: none)
    bool marmed;           ///< opened by a button still down: letting go on an item runs it
    int popx, popy;        ///< where the right button's menu is
    int pop_link;          ///< the link under the right button's menu
    // The boxes.
    int dlg;               ///< DLG_*
    char ask[96];          ///< the question
    int ask_act;           ///< what its first button does
    char ask_url[400];     ///< the web address asked about
    char fd_dir[PATH_MAX_];  ///< the file box's folder
    char fd_name[100];     ///< the name typed
    fd_entry_t fd[MAX_FILES];  ///< its entries
    char fd_pool[POOL];    ///< their names
    int fd_n, fd_sel, fd_top;  ///< how many, the one chosen, the first shown
    int toc_sel, toc_top;  ///< the contents' heading chosen, the first shown
    // Drawing.
    char msg[100];         ///< a message for the status line
    uint32_t msg_until;    ///< until when
    uint8_t *g;            ///< the off-screen picture (PSRAM): used if there is no strip
    uint8_t *strip;        ///< STRIP rows (SRAM heap): a frame is drawn in it a piece at a time
    bool use_strip;        ///< draw through the strip (Ctrl-B switches, to compare)
    uint8_t *t;            ///< where drawing goes: row 0 of the screen as drawn (strip, g or the screen)
    int cy0, cy1;          ///< drawing is clipped to these rows
    int by0, by1;          ///< and always to these (the strip's rows; else 0 and H)
    hit_t hits[96];        ///< places to click
    int nhits;             ///< their count
    bool collect;          ///< places to click are kept while drawing
    bool dirty;            ///< draw again
    // The mouse.
    int mdrag;             ///< 1: the scroll bar's thumb
    int drag_dy;           ///< where the thumb was held
    int mx, my;            ///< the pointer
    bool quit;             ///< leaving
    bool www;              ///< open ask_url in www (from the loop: www wants the stack)
    uint8_t *saved;        ///< the screen as it was before (put back after)
    // Times, for the About box.
    uint32_t frame_us;     ///< the last frame took to draw and show
    uint32_t page_ms;      ///< the page last took to draw
    uint32_t page_us;      ///< adding up this frame's
    uint32_t layout_ms;    ///< the document last took to lay out
    uint32_t ttf_ms;       ///< the fonts took to load
    int pic_n;             ///< pictures loaded, and the time they took (us), by part:
    uint32_t pic_read, pic_decode, pic_convert, pic_draw;  ///< read, decode, to RGB, dithered and kept
    uint32_t pic_sram;     ///< SRAM the last one's decoder had
} md_t;

static md_t *M;  ///< PSRAM, while the viewer runs

static void fd_read(void);

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/// Microseconds since boot.
/// @return them
static uint32_t clock_us(void) {
    return (uint32_t)to_us_since_boot(get_absolute_time());
}

/// Clips drawing to rows y0 to y1 - 1, within the strip's.
/// @param y0 the first
/// @param y1 after the last
static void clip(int y0, int y1) {
    M->cy0 = y0 > M->by0 ? y0 : M->by0;
    M->cy1 = y1 < M->by1 ? y1 : M->by1;
}

/// Sets a message for the status line (for 4 seconds).
/// @param fmt printf's format
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(M->msg, sizeof M->msg, fmt, ap);
    va_end(ap);
    M->msg_until = now_ms() + 4000;
    M->dirty = true;
}

// ---- Paths ---------------------------------------------------------------------------------------

/// Whether a link has a scheme (http:, mailto: ...).
/// @param s it
/// @return it has
static bool has_scheme(const char *s) {
    int i = 0;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')
        ++i;
    return i > 1 && s[i] == ':';
}

/// A path the document names, made whole: from the document's folder (or the drive's root, for /...),
/// with . and .. worked out, %XX undone and ?query or #name dropped.
/// @param rel it
/// @param out the whole path
/// @param size room in out
static void resolve(const char *rel, char *out, int size) {
    char tmp[PATH_MAX_ * 2];
    int k = 0;
    // %XX undone; the query and the #name go.
    for (const char *s = rel; *s && *s != '?' && *s != '#' && k < (int)sizeof tmp - 1; ++s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hx[3] = {s[1], s[2], 0};
            tmp[k++] = (char)strtol(hx, NULL, 16);
            s += 2;
        } else {
            tmp[k++] = *s;
        }
    }
    tmp[k] = 0;
    char whole[PATH_MAX_ * 3];
    if (tmp[0] == '/')
        snprintf(whole, sizeof whole, "%s", tmp);
    else
        snprintf(whole, sizeof whole, "%s/%s", strcmp(M->dir, "/") ? M->dir : "", tmp);
    // The parts, . and .. worked out.
    char *part[64];
    int np = 0;
    for (char *p = strtok(whole, "/"); p && np < 64; p = strtok(NULL, "/")) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            if (np)
                --np;
            continue;
        }
        part[np++] = p;
    }
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < np && n < size - 1; ++i)
        n += snprintf(out + n, size - n, "/%s", part[i]);
    if (!np)
        snprintf(out, size, "/");
}

/// Whether a name ends in one of the extensions.
/// @param name it
/// @param exts the extensions, space-separated, each with its dot and a space after
/// @return it does
static bool ext_is(const char *name, const char *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || strchr(dot, '/'))
        return false;
    char e[12];
    snprintf(e, sizeof e, "%s ", dot);
    for (char *p = e; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    return strstr(exts, e) != NULL;
}

/// The extensions of pictures that can be shown.
#define PIC_EXTS ".png .jpg .jpeg .bmp "

/// The file's name without its folder.
/// @return it, or "(no file)"
static const char *file_name(void) {
    if (!M->path[0])
        return "(no file)";
    const char *s = strrchr(M->path, '/');
    return s ? s + 1 : M->path;
}

// ---- Pictures --------------------------------------------------------------------------------------

/// A picture file's size, from its header (PNG, JPEG, BMP).
/// @param path the file
/// @param w set to its width
/// @param h set to its height
/// @return it is one that can be shown
static bool pic_header(const char *path, int *w, int *h) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    uint8_t b[32];
    UINT got = 0;
    bool ok = false;
    f_read(&f, b, sizeof b, &got);
    if (got >= 24 && !memcmp(b, "\x89PNG", 4)) {
        *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
        *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
        ok = true;
    } else if (got >= 26 && b[0] == 'B' && b[1] == 'M') {
        *w = (int32_t)(b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24);
        int32_t hh = (int32_t)(b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24);
        *h = hh < 0 ? -hh : hh;
        ok = true;
    } else if (got >= 4 && b[0] == 0xff && b[1] == 0xd8) {
        // The markers, to the frame's.
        FSIZE_t at = 2, size = f_size(&f);
        for (int guard = 0; guard < 200 && at + 9 < size; ++guard) {
            if (f_lseek(&f, at) != FR_OK || f_read(&f, b, 9, &got) != FR_OK || got < 9 || b[0] != 0xff)
                break;
            uint8_t m = b[1];
            if (m == 0xff) {  // padding
                ++at;
                continue;
            }
            if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
                *h = b[5] << 8 | b[6];
                *w = b[7] << 8 | b[8];
                ok = m == 0xc0 || m == 0xc1;  // baseline only (as the decoder)
                break;
            }
            at += 2 + (b[2] << 8 | b[3]);
        }
    }
    f_close(&f);
    return ok && *w > 0 && *h > 0 && *w < 20000 && *h < 20000;
}

/// md_layout's question: a picture's size (only pictures on the drive can be shown).
/// @param arg not used
/// @param src the picture, as the document names it
/// @param w set to its width
/// @param h set to its height
/// @return it can be shown
static bool size_of(void *arg, const char *src, int *w, int *h) {
    (void)arg;
    if (has_scheme(src) || !ext_is(src, PIC_EXTS))
        return false;
    char path[PATH_MAX_];
    resolve(src, path, sizeof path);
    return pic_header(path, w, h);
}

/// Forgets every picture drawn.
static void pics_clear(void) {
    for (int i = 0; i < MAX_PICS; ++i) {
        psram_free(M->pics[i].px);
        M->pics[i] = (pic_t){-1, NULL, 0, 0, 0};
    }
    M->pic_bytes = 0;
}

/// The picture drawn for an item.
/// @param item the item
/// @return it, or NULL if it isn't drawn yet
static pic_t *pic_find(int item) {
    for (int i = 0; i < MAX_PICS; ++i)
        if (M->pics[i].item == item)
            return &M->pics[i];
    return NULL;
}

/// Draws a picture for an item that shows and isn't drawn yet (one at a time: decoding takes a while).
/// @return one was drawn (or found to be unshowable)
static bool pic_load_one(void) {
    const md_doc_t *d = M->doc;
    if (!d || !M->pictures)
        return false;
    int it = -1;
    for (int i = md_first_item(d, M->top); i < d->nitems && d->items[i].y < M->top + VIEW_H; ++i)
        if (d->items[i].kind == MI_IMG && d->items[i].y + d->items[i].h > M->top && !pic_find(i)) {
            it = i;
            break;
        }
    if (it < 0)
        return false;
    const md_item_t *item = &d->items[it];
    uint32_t need = (uint32_t)item->w * item->h;
    // A slot, and room: the pictures used longest ago go.
    for (;;) {
        int free_slot = -1, oldest = -1;
        for (int i = 0; i < MAX_PICS; ++i) {
            if (M->pics[i].item < 0) {
                free_slot = i;
                continue;
            }
            if (M->pics[i].used != M->tick && (oldest < 0 || M->pics[i].used < M->pics[oldest].used))
                oldest = i;
        }
        if (free_slot >= 0 && M->pic_bytes + need <= PIC_BUDGET)
            break;
        if (oldest < 0)
            break;
        M->pic_bytes -= (uint32_t)M->pics[oldest].w * M->pics[oldest].h * (M->pics[oldest].px != NULL);
        psram_free(M->pics[oldest].px);
        M->pics[oldest] = (pic_t){-1, NULL, 0, 0, 0};
    }
    pic_t *p = NULL;
    for (int i = 0; i < MAX_PICS && !p; ++i)
        if (M->pics[i].item < 0)
            p = &M->pics[i];
    if (!p)
        return false;
    *p = (pic_t){it, NULL, item->w, item->h, M->tick};
    // Decoded (a big JPEG at a smaller size, if that is still big enough), dithered to the size it has
    // on the page, kept compact.
    char path[PATH_MAX_];
    resolve(d->pool + d->imgs[item->ref].src, path, sizeof path);
    message("Drawing %.60s", path);
    // The status line now, straight onto the screen: decoding takes a while.
    M->t = dvi_framebuf;
    M->cy0 = 0, M->cy1 = H;
    for (int r = STAT_Y; r < H; ++r)
        memset(dvi_framebuf + r * W, C_STRIP, W);
    for (int i = 0; M->msg[i] && i < 78; ++i) {
        const uint8_t *gl = font8x16[(uint8_t)M->msg[i]];
        for (int r = 0; r < 16; ++r)
            for (int b = 0; b < 8; ++b)
                if (gl[r] & (0x80 >> b))
                    dvi_framebuf[(STAT_Y + 2 + r) * W + 8 + i * 8 + b] = C_YELLOW;
    }
    M->msg_until = 0;
    const char *err;
    uint8_t *px = psram_malloc(need);
    if (!px)
        return true;
    uint32_t t0 = clock_us();
    // The strip's SRAM goes to the decoder while it works (its window and rows
    // are much faster there than in PSRAM).
    bool had_strip = M->strip != NULL;
    free(M->strip);
    M->strip = NULL;
    md_ttf_lend(true);  // and the fonts' arena
    bool ok = image_load_compact(path, px, item->w, item->h, &err);
    md_ttf_lend(false);
    if (had_strip)
        M->strip = malloc(STRIP * W);  // (without it: the PSRAM picture)
    if (!ok) {
        psram_free(px);
        return true;
    }
    uint32_t t = clock_us() - t0;
    M->pic_sram = image_times.sram_bytes;
    M->pic_n++;
    M->pic_read += image_times.read_us;
    M->pic_decode += image_times.decode_us;
    M->pic_convert += image_times.convert_us;
    M->pic_draw += t - image_times.read_us - image_times.decode_us - image_times.convert_us;
    p->px = px;
    M->pic_bytes += need;
    return true;
}

/// Shows a picture file on the whole screen until a key or a click.
/// @param path the file
static void show_picture(const char *path) {
    image_t img;
    const char *err;
    message("Loading %.60s", path);
    if (!image_load(path, W, H, &img, &err)) {
        message("%.40s: %.50s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, err);
        return;
    }
    memset(dvi_framebuf, 0, W * H);
    int x = 0, y = 0, w = W, h = H;
    if (img.w <= W && img.h <= H)
        x = (W - img.w) / 2, y = (H - img.h) / 2, w = img.w, h = img.h;
    else
        image_fit(&img, &x, &y, &w, &h);
    image_draw(&img, dvi_framebuf, x, y, w, h, NULL);
    image_free(&img);
    mouse_event_t e;
    for (;;) {
        mouse_pointer_update();
        if (key_poll() != KEY_NONE)
            break;
        if (mouse_event(&e) && e.type == MOUSE_DOWN)
            break;
        sleep_ms(10);
    }
    M->msg[0] = 0;
    M->dirty = true;
}

// ---- Drawing ----------------------------------------------------------------------------------

/// Fills a rectangle (clipped).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < 0)
        w += x, x = 0;
    if (y < M->cy0)
        h -= M->cy0 - y, y = M->cy0;
    if (x + w > W)
        w = W - x;
    if (y + h > M->cy1)
        h = M->cy1 - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; ++r)
        memset(M->t + (y + r) * W + x, c, w);
}

/// Draws a character, the 8x16 font at one of its sizes, its background left alone.
/// @param x left
/// @param y top
/// @param c the character (CP437)
/// @param font MD_FONT_
/// @param fl MF_BOLD and MF_ITALIC are used
/// @param fg its colour
/// @param x1 nothing is drawn from here right
static void glyph(int x, int y, uint8_t c, int font, uint8_t fl, uint8_t fg, int x1) {
    int cw = MD_CW(font), ch = MD_CH(font);
    if (y >= M->cy1 || y + ch <= M->cy0)
        return;
    const uint8_t *gl = font8x16[c];
    for (int r = 0; r < ch; ++r) {
        int yy = y + r;
        if (yy < M->cy0 || yy >= M->cy1)
            continue;
        uint8_t bits = gl[r * 16 / ch];
        if (fl & MF_BOLD)
            bits |= bits >> 1;
        if (!bits)
            continue;
        int sh = fl & MF_ITALIC ? (ch - 1 - r) * (cw / 4) / ch : 0;  // slanted
        uint8_t *row = M->t + yy * W;
        for (int i = 0; i < cw; ++i) {
            int xx = x + i + sh;
            if ((bits & (0x80 >> (i * 8 / cw))) && xx >= 0 && xx < x1)
                row[xx] = fg;
        }
    }
}

/// Draws text in the 8x16 font, its background filled.
/// @param x left
/// @param y top
/// @param s the text
/// @param fg its colour
/// @param bg the background
/// @return the x after it
static int text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; ++s, x += 8) {
        fill(x, y, 8, 16, bg);
        glyph(x, y, (uint8_t)*s, MD_FONT_8x16, 0, fg, W);
    }
    return x;
}

/// Draws formatted text.
/// @param x left
/// @param y top
/// @param fg its colour
/// @param bg the background
/// @param fmt printf's format
/// @return the x after it
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(x, y, buf, fg, bg);
}

/// A bevelled rectangle.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param raised raised (else sunken)
/// @param face its colour
static void bevel(int x, int y, int w, int h, bool raised, uint8_t face) {
    fill(x, y, w, h, face);
    uint8_t a = raised ? C_LIGHT : C_DARK, b = raised ? C_DARK : C_LIGHT;
    fill(x, y, w, 1, a);
    fill(x, y, 1, h, a);
    fill(x, y + h - 1, w, 1, b);
    fill(x + w - 1, y, 1, h, b);
}

/// Remembers a place to click (only while the whole picture is drawn).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param id HIT_*
/// @param arg which
static void hit(int x, int y, int w, int h, int id, int arg) {
    if (M->collect && M->nhits < (int)(sizeof M->hits / sizeof *M->hits))
        M->hits[M->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = M->nhits - 1; i >= 0; --i) {
        const hit_t *h = &M->hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
            return h;
    }
    return NULL;
}

/// A button.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param label its text
/// @param lit pressed
/// @param id what a click on it does
/// @param arg which
static void button(int x, int y, int w, int h, const char *label, bool lit, int id, int arg) {
    uint8_t face = lit ? C_SEL : C_FACE;
    bevel(x, y, w, h, !lit, face);
    int n = (int)strlen(label);
    text(x + (w - n * 8) / 2, y + (h - 16) / 2, label, lit ? C_YELLOW : C_TEXT, face);
    hit(x, y, w, h, id, arg);
}

/// The furthest the page scrolls.
/// @return it
static int32_t top_max(void) {
    int32_t m = M->doc ? M->doc->height - VIEW_H : 0;
    return m > 0 ? m : 0;
}

/// Scrolls the page to a place (kept in range).
/// @param top its first row shown
static void scroll_to(int32_t top) {
    if (top > top_max())
        top = top_max();
    if (top < 0)
        top = 0;
    if (top != M->top)
        M->top = top, M->dirty = true;
}

/// Draws a text item.
/// @param it it
/// @param idx its index
/// @param sx its left on the screen
/// @param sy its top
static void draw_text(const md_item_t *it, int idx, int sx, int sy) {
    const char *s = M->doc->pool + it->ref;
    bool tt = md_ttf_item(it);
    bool chosen = it->link && it->link == M->cur_link;
    bool hover = it->link && it->link == M->hover_link && !chosen;
    if (chosen)
        fill(sx - 1, sy, it->w + 2, it->h, C_SEL);
    else if (it->flags & MF_BG)
        fill(sx, sy, it->w, it->h, it->bg);
    int f0 = -1, f1 = -1;  // the text found in it
    if (idx == M->find_item) {
        f0 = M->find_off, f1 = f0 + (int)strlen(M->find);
        int x0 = sx, x1;
        for (int i = 0; i < f0 && i < it->len; ++i)
            x0 += md_char_w(it, (uint8_t)s[i]);
        x1 = x0;
        for (int i = f0; i < f1 && i < it->len; ++i)
            x1 += md_char_w(it, (uint8_t)s[i]);
        fill(x0, sy, x1 - x0, it->h, C_FOUND);
    }
    for (int i = 0, x = sx; i < it->len && x < SB_X - 2; x += md_char_w(it, (uint8_t)s[i]), ++i) {
        uint8_t fg = i >= f0 && i < f1 ? C_BG : chosen ? C_TEXT : it->fg;
        if (tt)
            md_ttf_char(it, (uint8_t)s[i], M->t + M->cy0 * W, W, SB_X - 2, M->cy1 - M->cy0, x, sy - M->cy0, fg);
        else
            glyph(x, sy, (uint8_t)s[i], it->font, it->flags, fg, SB_X - 2);
    }
    if (it->flags & MF_STRIKE)
        fill(sx, sy + it->h / 2, it->w, 1 + it->font, it->fg);
    if ((it->flags & MF_ULINE) && !chosen)
        fill(sx, sy + it->h - 2, it->w, 1, hover ? C_TEXT : it->fg);
}

/// Draws a picture item: drawn, or a box with its alt text while it loads or if it can't be.
/// @param it it
/// @param idx its index
/// @param sx its left on the screen
/// @param sy its top
static void draw_img(const md_item_t *it, int idx, int sx, int sy) {
    pic_t *p = M->pictures ? pic_find(idx) : NULL;
    int w = it->w, h = it->h;
    if (p && p->px) {
        p->used = M->tick;
        for (int r = 0; r < h; ++r) {
            int y = sy + r;
            if (y < M->cy0 || y >= M->cy1)
                continue;
            int n = w;
            if (sx + n > SB_X - 2)
                n = SB_X - 2 - sx;
            if (n > 0)
                memcpy(M->t + y * W + sx, p->px + r * p->w, n);
        }
    } else {
        if (M->pictures && !p)
            M->pic_wanted = true;
        uint8_t c = it->link && it->link == M->cur_link ? C_SEL : MD_C_RULE;
        fill(sx, sy, w, 1, c);
        fill(sx, sy + h - 1, w, 1, c);
        fill(sx, sy, 1, h, c);
        fill(sx + w - 1, sy, 1, h, c);
        const char *alt = M->doc->pool + M->doc->imgs[it->ref].alt;
        if (!*alt)
            alt = M->pictures && !p ? "loading" : "picture";
        if (h >= 18 && w >= 24) {
            int saved = M->cy1;
            if (sy + h - 1 < M->cy1)
                M->cy1 = sy + h - 1;
            for (int i = 0; alt[i] && 4 + (i + 1) * 8 <= w - 2; ++i)
                glyph(sx + 4 + i * 8, sy + 2, (uint8_t)alt[i], MD_FONT_8x16, 0, MD_C_ALT, sx + w - 2);
            M->cy1 = saved;
        }
    }
    if (it->link && it->link == M->cur_link && p && p->px) {  // a frame round a picture that is the link chosen
        fill(sx - 2, sy - 2, w + 4, 2, C_CYAN);
        fill(sx - 2, sy + h, w + 4, 2, C_CYAN);
        fill(sx - 2, sy, 2, h, C_CYAN);
        fill(sx + w, sy, 2, h, C_CYAN);
    }
}

/// Draws the page: the items on the screen.
static void draw_page(void) {
    clip(VIEW_Y, STAT_Y);
    fill(0, VIEW_Y, SB_X, VIEW_H, MD_C_BG);
    hit(0, VIEW_Y, SB_X, VIEW_H, HIT_PAGE, 0);
    if (M->collect) {  // once a frame (the first piece)
        M->pic_wanted = false;
        ++M->tick;
    }
    const md_doc_t *d = M->doc;
    if (d) {
        for (int i = md_first_item(d, M->top); i < d->nitems; ++i) {
            const md_item_t *it = &d->items[i];
            if (it->y >= M->top + VIEW_H)
                break;
            if (it->y + it->h <= M->top)
                continue;
            int sx = MARGIN + it->x, sy = VIEW_Y + (int)(it->y - M->top);
            if (sy >= M->cy1 || sy + it->h <= M->cy0)
                continue;  // not in this piece
            switch (it->kind) {
            case MI_RECT: fill(sx, sy, it->w > SB_X - 2 - sx ? SB_X - 2 - sx : it->w, it->h, it->fg); break;
            case MI_TEXT: draw_text(it, i, sx, sy); break;
            case MI_IMG: draw_img(it, i, sx, sy); break;
            }
        }
    } else {
        text(MARGIN, VIEW_Y + 16, "No file: Ctrl-O opens one, F1 lists the keys.", C_LABEL, MD_C_BG);
    }
    // The scroll bar.
    fill(SB_X, VIEW_Y, SB_W, VIEW_H, C_FACE);
    int32_t total = d ? d->height : VIEW_H;
    int th = VIEW_H, ty = 0;
    if (total > VIEW_H) {
        th = (int)((int64_t)VIEW_H * VIEW_H / total);
        if (th < 16)
            th = 16;
        ty = (int)((int64_t)(VIEW_H - th) * M->top / top_max());
    }
    bevel(SB_X + 1, VIEW_Y + ty, SB_W - 2, th, true, C_LIGHT);
    hit(SB_X, VIEW_Y, SB_W, VIEW_H, HIT_SB, 0);
    clip(0, H);
}

/// A menu title's x on the bar.
/// @param i the menu
/// @return its left
static int bar_x(int i) {
    int x = 16;
    for (int j = 0; j < i; ++j)
        x += ((int)strlen(menus[j].title) + 4) * 8;
    return x;
}

/// Draws the menu bar, as the tracker's: the titles (the one open lit), the file's name on the right.
static void draw_bar(void) {
    fill(0, 0, W, BAR_H, C_FACE);
    for (int i = 0; i < NMENUS; ++i) {
        int bx = bar_x(i), bw = ((int)strlen(menus[i].title) + 4) * 8;
        bool open = i == M->menu;
        if (open)
            fill(bx, 1, bw, 14, C_SEL);
        text(bx + 16, 0, menus[i].title, open ? C_YELLOW : C_TEXT, open ? C_SEL : C_FACE);
        fill(bx + 16, 14, 8, 1, open ? C_YELLOW : C_LABEL);  // its key letter (Alt and it)
        hit(bx, 0, bw, BAR_H, HIT_MENU, i);
    }
    char name[70];
    snprintf(name, sizeof name, "%.36s%s  Markdown " VER_MD, file_name(), M->source ? " (source)" : "");
    text(W - 8 - (int)strlen(name) * 8, 0, name, C_YELLOW, C_FACE);
    fill(0, 0, W, 1, C_LIGHT);
    fill(0, BAR_H - 1, W, 1, C_DARK);
}

/// The link under the pointer, the one chosen, or none.
/// @return its target, or NULL
static const char *link_url(int link) {
    return link > 0 && M->doc && link <= M->doc->nlinks ? M->doc->pool + M->doc->links[link - 1] : NULL;
}

/// Draws the status line: a message, the link under the pointer or chosen, or where the page is.
static void draw_status(void) {
    fill(0, STAT_Y, W, H - STAT_Y, C_STRIP);
    const char *url;
    if (M->msg[0] && now_ms() < M->msg_until) {
        textf(8, STAT_Y + 2, C_TEXT, C_STRIP, "%.78s", M->msg);
    } else if ((url = link_url(M->hover_link)) || (url = link_url(M->cur_link))) {
        textf(8, STAT_Y + 2, C_CYAN, C_STRIP, "%.78s", url);
    } else {
        int pct = top_max() ? (int)((int64_t)M->top * 100 / top_max()) : 100;
        textf(8, STAT_Y + 2, C_LABEL, C_STRIP, "%3d%%  %d links, %d pictures%s    F1 keys  F10 menus", pct,
              M->doc ? M->doc->nlinks : 0, M->doc ? M->doc->nimgs : 0,
              M->doc && M->doc->short_of_memory ? "  OUT OF MEMORY: the end is missing" : "");
    }
}

// ---- The menus ---------------------------------------------------------------------------------

/// Whether a menu item can be chosen now.
/// @param it the item
/// @return it can
static bool usable(const mitem_t *it) {
    switch (it->id) {
    case 0: return false;
    case CMD_BACK: return M->hpos > 0;
    case CMD_FORWARD: return M->hpos + 1 < M->nhist;
    case CMD_NEXT: return M->find[0] != 0;
    case CMD_FOLLOW: return (M->menu == NMENUS ? M->pop_link : M->cur_link) > 0;
    case CMD_TOC: return M->doc && M->doc->nheads > 0 && !M->source;
    case CMD_RELOAD: case CMD_SOURCE: case CMD_FIND: return M->doc != NULL;
    }
    return true;
}

/// Whether a menu item shows a tick.
/// @param it the item
/// @return it does
static bool ticked(const mitem_t *it) {
    return (it->id == CMD_SOURCE && M->source) || (it->id == CMD_PICTURES && M->pictures) ||
           (it->id == CMD_TTF && md_ttf_on());
}

/// The items of the menu open.
/// @return them
static const mitem_t *menu_items(void) {
    return M->menu == NMENUS ? popup_items : menus[M->menu].items;
}

/// Lays out the menu open.
/// @param x set to its left
/// @param y set to its top
/// @param w set to its width
/// @param h set to its height
/// @param iy set to each item's y (MAX_ITEMS)
/// @param acc set to each item's key letter (its place in the text, -1: none)
/// @return its items
static int menu_layout(int *x, int *y, int *w, int *h, int *iy, int8_t *acc) {
    const mitem_t *items = menu_items();
    int n, tw = 0, kw = 0;
    for (n = 0; items[n].text; ++n) {
        if ((int)strlen(items[n].text) > tw)
            tw = (int)strlen(items[n].text);
        if (items[n].key && (int)strlen(items[n].key) > kw)
            kw = (int)strlen(items[n].key);
    }
    *w = (tw + kw + 6) * 8;
    *x = M->menu == NMENUS ? M->popx : bar_x(M->menu);
    *y = M->menu == NMENUS ? M->popy : BAR_H;
    *h = 6;
    for (int i = 0; i < n; ++i) {
        iy[i] = *y + 3 + *h - 6;
        *h += items[i].text[0] == '-' ? 8 : 18;
    }
    if (*x + *w + SHADOW > W)
        *x = W - *w - SHADOW;
    if (*y + *h + SHADOW > H) {
        int up = H - *h - SHADOW - *y;
        *y += up;
        for (int i = 0; i < n; ++i)
            iy[i] += up;
    }
    // Key letters: the first letter of a word not taken by an item above, else any letter not taken.
    bool used[26] = {false};
    for (int i = 0; i < n; ++i) {
        const char *t = items[i].text;
        acc[i] = -1;
        if (t[0] == '-')
            continue;
        for (int pass = 0; pass < 2; ++pass)
            for (int c = 0; t[c] && acc[i] < 0; ++c) {
                int l = (t[c] | 0x20) - 'a';
                if (l >= 0 && l < 26 && !used[l] && (pass || c == 0 || t[c - 1] == ' '))
                    used[l] = true, acc[i] = (int8_t)c;
            }
    }
    return n;
}

/// Draws the menu open, its shadow and its items.
static void draw_menu(void) {
    int x, y, w, h, iy[MAX_ITEMS];
    int8_t acc[MAX_ITEMS];
    int n = menu_layout(&x, &y, &w, &h, iy, acc);
    const mitem_t *items = menu_items();
    hit(0, 0, W, H, HIT_MENU_OFF, 0);
    if (M->menu < NMENUS)
        for (int i = 0; i < NMENUS; ++i)
            hit(bar_x(i), 0, ((int)strlen(menus[i].title) + 4) * 8, BAR_H, HIT_MENU, i);
    fill(x + SHADOW, y + SHADOW, w, h, C_BG);
    bevel(x, y, w, h, true, C_FACE);
    for (int i = 0; i < n; ++i) {
        const mitem_t *it = &items[i];
        if (it->text[0] == '-') {
            fill(x + 6, iy[i] + 3, w - 12, 1, C_DARK);
            fill(x + 6, iy[i] + 4, w - 12, 1, C_LIGHT);
            continue;
        }
        bool on = i == M->msel, off = !usable(it);
        uint8_t bg = on ? C_SEL : C_FACE, fg = off ? C_DIM : on ? C_TEXT : C_LABEL;
        int ty = iy[i] + 1;
        fill(x + 3, iy[i], w - 6, 18, bg);
        if (ticked(it)) {
            fill(x + 6, ty, 8, 16, bg);
            glyph(x + 6, ty, 0xfb, MD_FONT_8x16, 0, on ? C_YELLOW : C_GREEN, W);
        }
        text(x + 22, ty, it->text, fg, bg);
        if (acc[i] >= 0 && !off)
            fill(x + 22 + acc[i] * 8, ty + 14, 8, 1, on ? C_YELLOW : C_DIM);
        if (it->key && it->key[0])
            text(x + w - 8 - (int)strlen(it->key) * 8, ty, it->key, on ? C_YELLOW : C_DIM, bg);
        hit(x + 3, iy[i], w - 6, 18, HIT_ITEM, i);
    }
}

/// Opens a menu.
/// @param m the menu (NMENUS: the right button's)
/// @param armed opened by a button that is still down
static void menu_open(int m, bool armed) {
    M->menu = m;
    M->msel = -1;
    M->marmed = armed;
    M->dirty = true;
}

/// Closes the menu.
static void menu_close(void) {
    M->menu = -1;
    M->dirty = true;
}

static void command(int id);

/// Runs a menu item.
/// @param i the item
static void item_run(int i) {
    const mitem_t *it = &menu_items()[i];
    if (!usable(it))
        return;
    if (it->id == CMD_FOLLOW && M->menu == NMENUS)
        M->cur_link = M->pop_link;
    menu_close();
    command(it->id);
}

/// A key while a menu is open.
/// @param k the key
static void menu_key(int k) {
    int x, y, w, h, iy[MAX_ITEMS];
    int8_t acc[MAX_ITEMS];
    int n = menu_layout(&x, &y, &w, &h, iy, acc);
    const mitem_t *items = menu_items();
    M->dirty = true;
    if (k == KEY_ESC || k == KEY_F9 || k == KEY_F10 || k == CTRL('c')) {
        menu_close();
    } else if (k == KEY_UP || k == KEY_DOWN) {
        int d = k == KEY_UP ? -1 : 1, s = M->msel < 0 ? (d > 0 ? -1 : 0) : M->msel;
        for (int t = 0; t < n; ++t) {
            s = (s + d + n) % n;
            if (usable(&items[s]))
                break;
        }
        M->msel = s;
    } else if ((k == KEY_LEFT || k == KEY_RIGHT) && M->menu < NMENUS) {
        menu_open((M->menu + (k == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS, false);
    } else if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
        for (int i = 0; i < NMENUS; ++i)
            if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                menu_open(i, false);
    } else if (k == '\r' || k == '\n') {
        if (M->msel >= 0)
            item_run(M->msel);
    } else if (k >= 0x20 && k < 0x7f) {
        for (int i = 0; i < n; ++i)
            if (acc[i] >= 0 && (items[i].text[acc[i]] | 0x20) == (k | 0x20) && usable(&items[i])) {
                item_run(i);
                break;
            }
    }
}

// ---- The boxes ---------------------------------------------------------------------------------

/// A box's background, which swallows clicks around its buttons.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param title its title
static void box(int x, int y, int w, int h, const char *title) {
    bevel(x - 2, y - 2, w + 4, h + 4, true, C_LIGHT);
    bevel(x, y, w, h, true, C_FACE);
    hit(0, 0, W, H, HIT_SWALLOW, 0);
    text(x + 16, y + 10, title, C_YELLOW, C_FACE);
}

/// Draws the question.
static void draw_ask(void) {
    int w = 560, h = 120, x = (W - w) / 2, y = 170;
    box(x, y, w, h, M->ask);
    textf(x + 16, y + 36, C_CYAN, C_FACE, "%.66s", M->ask_url);
    button(x + 16, y + h - 36, 160, 26, "Open it (Enter)", true, HIT_ASK, 0);
    button(x + 184, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_ASK, 1);
}

/// The file box's and contents' rows shown.
#define LIST_ROWS 14

/// Keeps a list's choice in range and on show.
/// @param sel the row chosen
/// @param top the first shown
/// @param n the rows
static void list_keep(int *sel, int *top, int n) {
    if (*sel >= n)
        *sel = n - 1;
    if (*sel < 0)
        *sel = 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + LIST_ROWS)
        *top = *sel - LIST_ROWS + 1;
    if (*top < 0)
        *top = 0;
}

/// Draws the file box.
static void draw_file_box(void) {
    int w = 520, h = 400, x = (W - w) / 2, y = 40;
    box(x, y, w, h, "Open a file (Markdown files are blue)");
    textf(x + 16, y + 32, C_LABEL, C_FACE, "%.54s", M->fd_dir);
    button(x + w - 64, y + 30, 48, 20, "Up", false, HIT_FD_UP, 0);
    int ly = y + 56;
    bevel(x + 12, ly - 2, w - 24, LIST_ROWS * 18 + 4, false, C_FIELD);
    list_keep(&M->fd_sel, &M->fd_top, M->fd_n);
    for (int r = 0; r < LIST_ROWS; ++r) {
        int i = M->fd_top + r, ry = ly + r * 18;
        uint8_t bg = i == M->fd_sel && i < M->fd_n ? C_SEL : (r & 1) ? C_ROWALT : C_FIELD;
        fill(x + 14, ry, w - 28, 18, bg);
        if (i >= M->fd_n)
            continue;
        const fd_entry_t *e = &M->fd[i];
        const char *name = M->fd_pool + e->name;
        uint8_t fg = e->dir ? C_CYAN : ext_is(name, ".md .markdown .mkd .mdown ") ? C_MD : C_TEXT;
        textf(x + 20, ry + 1, fg, bg, "%s%.44s", e->dir ? "\x10 " : "  ", name);
        if (!e->dir) {
            char sz[16];
            snprintf(sz, sizeof sz, e->size < 10000000 ? "%lu" : "%lu MB",
                     (unsigned long)(e->size < 10000000 ? e->size : e->size >> 20));
            text(x + w - 24 - (int)strlen(sz) * 8, ry + 1, sz, C_LABEL, bg);
        }
        hit(x + 14, ry, w - 28, 18, HIT_ROW, i);
    }
    int ny = ly + LIST_ROWS * 18 + 12;
    text(x + 16, ny + 3, "Name", C_LABEL, C_FACE);
    bevel(x + 60, ny, w - 76, 22, false, C_FIELD);
    int n = (int)strlen(M->fd_name), skip = n > 52 ? n - 52 : 0;
    textf(x + 64, ny + 3, C_CYAN, C_FIELD, "%s_", M->fd_name + skip);
    button(x + 16, y + h - 36, 120, 26, "Open (Enter)", true, HIT_FD_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_FD_CANCEL, 0);
}

/// Draws the Find box.
static void draw_find_box(void) {
    int w = 500, h = 110, x = (W - w) / 2, y = 160;
    box(x, y, w, h, "Find (F3 finds the next; case doesn't matter)");
    bevel(x + 16, y + 36, w - 32, 22, false, C_FIELD);
    int n = (int)strlen(M->in), skip = n > 56 ? n - 56 : 0;
    textf(x + 20, y + 39, C_CYAN, C_FIELD, "%s_", M->in + skip);
    button(x + 16, y + h - 36, 120, 26, "Find (Enter)", true, HIT_IN_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_IN_CANCEL, 0);
}

/// Draws the table of contents.
static void draw_toc(void) {
    const md_doc_t *d = M->doc;
    int w = 560, h = 336, x = (W - w) / 2, y = 60;
    box(x, y, w, h, "Contents (Enter goes to the heading)");
    int ly = y + 36;
    bevel(x + 12, ly - 2, w - 24, LIST_ROWS * 18 + 4, false, C_FIELD);
    list_keep(&M->toc_sel, &M->toc_top, d->nheads);
    for (int r = 0; r < LIST_ROWS; ++r) {
        int i = M->toc_top + r, ry = ly + r * 18;
        uint8_t bg = i == M->toc_sel && i < d->nheads ? C_SEL : (r & 1) ? C_ROWALT : C_FIELD;
        fill(x + 14, ry, w - 28, 18, bg);
        if (i >= d->nheads)
            continue;
        const md_head_t *hd = &d->heads[i];
        int in = (hd->level - 1) * 2;
        static const uint8_t colours[] = {MD_C_H1, MD_C_H2, MD_C_H3, MD_C_H4, MD_C_H4, MD_C_H4};
        textf(x + 20 + in * 8, ry + 1, colours[hd->level - 1], bg, "%.*s", 62 - in, d->pool + hd->text);
        hit(x + 14, ry, w - 28, 18, HIT_TOC, i);
    }
    text(x + 16, y + h - 26, "Up, Down, PgUp, PgDn choose; Esc closes", C_DIM, C_FACE);
}

/// Draws the keys.
static void draw_help(void) {
    int w = 620, h = 440, x = (W - w) / 2, y = 20;
    box(x, y, w, h, "Keys (any key or click closes this)");
    static const char *const lines[] = {
        "Up, Down        scroll            PgUp, PgDn, Space, b   a page",
        "Home, End       the top, the end",
        "Tab, Shift-Tab  choose the next or previous link (the page follows)",
        "Enter, Right    follow it: a Markdown file opens here, #name goes",
        "                to the heading, a picture fills the screen, a web",
        "                address opens in www",
        "Bksp, Left, [   back              ]   forward",
        "Ctrl-T, t       contents: the headings, to go to one",
        "Ctrl-F, /       find text         F3, n   find it again",
        "F6, s           the source text, and back",
        "*, p            pictures, or boxes with their names",
        "F7, f           TrueType text (fonts from /FONTS), or the 8x16 font",
        "F5, r           read the file again",
        "Ctrl-O, o       open another file",
        "F10, F9         the menus (Alt and a letter opens one)",
        "Esc, q          quit",
        "Mouse: the wheel and the scroll bar scroll; click a link to",
        "follow it, a picture to see it on the whole screen; the right",
        "button gives a menu. Pictures: PNG, JPEG (baseline), BMP, on the",
        "drive, found from the file's folder.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 36 + (int)i * 20, lines[i], C_TEXT, C_FACE);
}

/// Draws the About box.
static void draw_about(void) {
    int w = 480, h = 270, x = (W - w) / 2, y = 100;
    box(x, y, w, h, "Markdown viewer " VER_MD);
    static const char *const lines[] = {
        "Shows Markdown files (CommonMark with GitHub's tables,",
        "task lists and strikethrough) with their pictures.",
        "Copyright (c) 2026 Ken Segler. MIT licence.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 40 + (int)i * 20, lines[i], i == 2 ? C_LABEL : C_TEXT, C_FACE);
    if (M->doc)
        textf(x + 16, y + 112, C_DIM, C_FACE, "%lu bytes, %d items, %d links, %d pictures (%lu KB drawn)",
              (unsigned long)M->len, M->doc->nitems, M->doc->nlinks, M->doc->nimgs,
              (unsigned long)(M->pic_bytes >> 10));
    textf(x + 16, y + 132, C_DIM, C_FACE, "PSRAM free %lu KB", (unsigned long)(psram_free_total() >> 10));
    textf(x + 16, y + 152, C_DIM, C_FACE, "%s, %s: frame %lu ms (page %lu ms)",
          md_ttf_on() ? "TrueType" : "8x16 font", M->strip && M->use_strip ? "SRAM strip" : "PSRAM buffer",
          (unsigned long)(M->frame_us / 1000), (unsigned long)M->page_ms);
    textf(x + 16, y + 172, C_DIM, C_FACE, "laid out in %lu ms", (unsigned long)M->layout_ms);
    if (md_ttf_loaded())
        textf(x + 216, y + 172, C_DIM, C_FACE, "fonts loaded in %lu ms", (unsigned long)M->ttf_ms);
    if (M->pic_n)
        textf(x + 16, y + 192, C_DIM, C_FACE, "%d pictures: read %lu, decode %lu, RGB %lu, dither %lu ms", M->pic_n,
              (unsigned long)(M->pic_read / 1000), (unsigned long)(M->pic_decode / 1000),
              (unsigned long)(M->pic_convert / 1000), (unsigned long)(M->pic_draw / 1000));
    if (M->pic_n)
        textf(x + 16, y + 212, C_DIM, C_FACE, "(the last decoded with %lu KB of SRAM)", (unsigned long)(M->pic_sram >> 10));
    text(x + 16, y + h - 26, "(any key or click closes this)", C_DIM, C_FACE);
}

/// Draws everything, clipped to the strip's rows (M->by0 to M->by1).
static void compose(void) {
    clip(0, H);
    uint32_t t0 = clock_us();
    draw_page();
    M->page_us += clock_us() - t0;
    draw_bar();
    draw_status();
    switch (M->dlg) {
    case DLG_ASK: draw_ask(); break;
    case DLG_FILE: draw_file_box(); break;
    case DLG_FIND: draw_find_box(); break;
    case DLG_TOC: draw_toc(); break;
    case DLG_HELP: draw_help(); break;
    case DLG_ABOUT: draw_about(); break;
    }
    if (M->menu >= 0)
        draw_menu();
}

/// Draws everything: through the SRAM strip a piece at a time, each piece
/// copied onto the screen when done (PSRAM is slow to fill and read back);
/// else into the off-screen picture, then copied; else onto the screen.
static void draw(void) {
    uint32_t t0 = clock_us();
    M->nhits = 0;
    M->page_us = 0;
    if (M->strip && M->use_strip) {
        for (int y0 = 0; y0 < H; y0 += STRIP) {
            M->t = M->strip - (ptrdiff_t)y0 * W;
            M->by0 = y0, M->by1 = y0 + STRIP;
            M->collect = y0 == 0;  // the places to click: once
            compose();
            memcpy(dvi_framebuf + y0 * W, M->strip, STRIP * W);
        }
    } else {
        M->t = M->g ? M->g : dvi_framebuf;
        M->by0 = 0, M->by1 = H;
        M->collect = true;
        compose();
        if (M->g)
            memcpy(dvi_framebuf, M->g, W * H);
    }
    M->collect = false;
    M->by0 = 0, M->by1 = H;
    clip(0, H);
    M->t = dvi_framebuf;
    M->page_ms = M->page_us / 1000;  // (About shows the frame before's)
    M->frame_us = clock_us() - t0;
}

// ---- Files -------------------------------------------------------------------------------------

/// Lays the text out again (after loading it, or a change of view).
static void relayout(void) {
    md_free(M->doc);
    pics_clear();
    M->cur_item = -1, M->cur_link = 0, M->hover_link = 0, M->find_item = -1;
    uint32_t t0 = now_ms();
    M->doc = md_layout(M->text, M->len, PAGE_W, M->source, size_of, NULL);
    M->layout_ms = now_ms() - t0;
    if (!M->doc)
        message("Not enough memory to show it");
    else if (M->doc->short_of_memory)
        message("Not enough memory for all of it: the end is missing");
    scroll_to(M->top);
    M->dirty = true;
}

/// Goes to a heading by its #name.
/// @param name it (without #)
/// @return it was found
static bool goto_anchor(const char *name) {
    if (!M->doc || !*name)
        return false;
    char want[120];
    int k = 0;
    for (const char *s = name; *s && k < (int)sizeof want - 1; ++s) {  // %XX undone, lower case
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hx[3] = {s[1], s[2], 0};
            want[k++] = (char)tolower((int)strtol(hx, NULL, 16));
            s += 2;
        } else {
            want[k++] = (char)tolower((unsigned char)*s);
        }
    }
    want[k] = 0;
    for (int i = 0; i < M->doc->nheads; ++i)
        if (!strcmp(M->doc->pool + M->doc->heads[i].slug, want)) {
            scroll_to(M->doc->heads[i].y - 4);
            return true;
        }
    return false;
}

/// Reads a file and shows it.
/// @param path the whole path
/// @param anchor a heading to go to (NULL: the top)
/// @param push put it in the history (else it is the one there, already)
/// @return it was read
static bool open_doc(const char *path, const char *anchor, bool push) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        message("Can't open %.60s", path);
        return false;
    }
    FSIZE_t size = f_size(&f);
    if (size > FILE_MAX) {
        f_close(&f);
        message("%.40s is too big (%lu MB; 8 MB at most)", path, (unsigned long)(size >> 20));
        return false;
    }
    char *t = psram_malloc((size_t)size + 1);
    UINT got = 0;
    if (!t || f_read(&f, t, (UINT)size, &got) != FR_OK || got != size) {
        f_close(&f);
        psram_free(t);
        message(t ? "Reading %.60s failed" : "Not enough memory for %.50s", path);
        return false;
    }
    f_close(&f);
    t[size] = 0;
    // The place in the file left, kept for Back.
    if (push && M->nhist)
        M->hist[M->hpos].top = M->top;
    psram_free(M->text);
    M->text = t;
    M->len = (uint32_t)size;
    snprintf(M->path, sizeof M->path, "%s", path);
    snprintf(M->dir, sizeof M->dir, "%s", path);
    char *slash = strrchr(M->dir, '/');
    if (slash && slash != M->dir)
        *slash = 0;
    else
        strcpy(M->dir, "/");
    if (push) {
        if (M->nhist)
            M->nhist = M->hpos + 1;  // forward is forgotten
        if (M->nhist == HIST_MAX) {
            memmove(M->hist, M->hist + 1, (HIST_MAX - 1) * sizeof *M->hist);
            --M->nhist;
        }
        M->hpos = M->nhist++;
        snprintf(M->hist[M->hpos].path, sizeof M->hist[M->hpos].path, "%s", path);
        M->hist[M->hpos].top = 0;
    }
    M->top = 0;
    message("Laying out %.60s", file_name());
    draw();  // (says so while it works)
    M->msg[0] = 0;
    relayout();
    if (anchor && *anchor && !goto_anchor(anchor))
        message("No heading #%.60s", anchor);
    return true;
}

/// Back or forward in the history.
/// @param d -1 back, 1 forward
static void history(int d) {
    int to = M->hpos + d;
    if (to < 0 || to >= M->nhist)
        return;
    M->hist[M->hpos].top = M->top;
    char path[PATH_MAX_];
    snprintf(path, sizeof path, "%s", M->hist[to].path);
    int32_t top = M->hist[to].top;
    int was = M->hpos;
    M->hpos = to;
    if (!open_doc(path, NULL, false)) {
        M->hpos = was;
        return;
    }
    scroll_to(top);
}

/// Opens the web browser at an address, then comes back.
/// @param url it
static void run_www(const char *url) {
    mouse_pointer(MOUSE_POINTER_OFF);
    memcpy(dvi_framebuf, M->saved, W * H);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    char *argv[] = {"www", (char *)url, NULL};
    www_command(2, argv);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    M->dirty = true;
}

/// Follows a link.
/// @param url its target
static void follow(const char *url) {
    if (url[0] == '#') {
        if (!goto_anchor(url + 1))
            message("No heading %.60s", url);
        return;
    }
    if (has_scheme(url)) {
        if (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8)) {
            snprintf(M->ask, sizeof M->ask, "Open this in the web browser?");
            snprintf(M->ask_url, sizeof M->ask_url, "%s", url);
            M->ask_act = ACT_WWW;
            M->dlg = DLG_ASK;
            M->dirty = true;
        } else {
            message("Can't open %.60s", url);
        }
        return;
    }
    char path[PATH_MAX_];
    resolve(url, path, sizeof path);
    const char *hash = strchr(url, '#');
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK && strcmp(path, "/")) {
        message("%.70s isn't on the drive", path);
        return;
    }
    if (strcmp(path, "/") && !(fi.fattrib & AM_DIR) && ext_is(path, PIC_EXTS)) {
        show_picture(path);
        return;
    }
    if (!strcmp(path, "/") || (fi.fattrib & AM_DIR)) {
        snprintf(M->fd_dir, sizeof M->fd_dir, "%s", path);
        M->dlg = DLG_FILE;
        M->fd_name[0] = 0;
        M->dirty = true;
        fd_read();
        return;
    }
    if (!strcmp(path, M->path)) {  // this file: only the #name
        if (hash)
            goto_anchor(hash + 1);
        return;
    }
    open_doc(path, hash ? hash + 1 : NULL, true);
}

// ---- The file box ---------------------------------------------------------------------------------

/// Sorts folders first, then by name.
/// @param a an entry
/// @param b another
/// @return their order
static int fd_compare(const void *a, const void *b) {
    const fd_entry_t *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(M->fd_pool + x->name, M->fd_pool + y->name);
}

/// Reads the folder shown into the list.
static void fd_read(void) {
    M->fd_n = M->fd_sel = M->fd_top = 0;
    DIR d;
    if (f_opendir(&d, M->fd_dir) != FR_OK) {
        message("Can't open %.60s", M->fd_dir);
        return;
    }
    FILINFO fi;
    int used = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] && M->fd_n < MAX_FILES) {
        int n = (int)strlen(fi.fname) + 1;
        if (fi.fname[0] == '.' || used + n > POOL)
            continue;
        fd_entry_t *e = &M->fd[M->fd_n++];
        memcpy(M->fd_pool + used, fi.fname, n);
        e->name = (uint16_t)used;
        e->dir = fi.fattrib & AM_DIR;
        e->size = (uint32_t)fi.fsize;
        used += n;
    }
    f_closedir(&d);
    qsort(M->fd, M->fd_n, sizeof *M->fd, fd_compare);
}

/// Opens the file box, in the file's folder (or the shell's).
static void fd_open(void) {
    if (M->path[0])
        snprintf(M->fd_dir, sizeof M->fd_dir, "%s", M->dir);
    else if (f_getcwd(M->fd_dir, sizeof M->fd_dir) != FR_OK)
        strcpy(M->fd_dir, "/");
    M->fd_name[0] = 0;
    M->dlg = DLG_FILE;
    fd_read();
    // The file shown is chosen.
    for (int i = 0; i < M->fd_n; ++i)
        if (!strcmp(M->fd_pool + M->fd[i].name, file_name()))
            M->fd_sel = i;
    M->dirty = true;
}

/// Goes into a folder of the list, or up.
/// @param name the folder, or ".." for up
static void fd_go(const char *name) {
    if (!strcmp(name, "..")) {
        char *s = strrchr(M->fd_dir, '/');
        if (s && s != M->fd_dir)
            *s = 0;
        else
            strcpy(M->fd_dir, "/");
    } else {
        char p[PATH_MAX_ * 2];
        if (name[0] == '/')
            snprintf(p, sizeof p, "%s", name);
        else
            snprintf(p, sizeof p, "%s%s%s", M->fd_dir, strcmp(M->fd_dir, "/") ? "/" : "", name);
        snprintf(M->fd_dir, sizeof M->fd_dir, "%s", p);
    }
    fd_read();
    M->dirty = true;
}

/// The file box's OK: opens the file typed or chosen (a folder: goes into it).
static void fd_ok(void) {
    const char *name = M->fd_name;
    if (!*name) {
        if (M->fd_sel >= M->fd_n)
            return;
        name = M->fd_pool + M->fd[M->fd_sel].name;
    }
    char p[PATH_MAX_ * 2];
    if (name[0] == '/')
        snprintf(p, sizeof p, "%s", name);
    else
        snprintf(p, sizeof p, "%s%s%s", M->fd_dir, strcmp(M->fd_dir, "/") ? "/" : "", name);
    FILINFO fi;
    if (!strcmp(p, "/") || (f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR))) {
        M->fd_name[0] = 0;
        fd_go(p);
        return;
    }
    M->dlg = DLG_NONE;
    M->dirty = true;
    if (ext_is(p, PIC_EXTS))
        show_picture(p);
    else
        open_doc(p, NULL, true);
}

// ---- Links and finding ---------------------------------------------------------------------------

/// Chooses the next or previous link (from the one chosen, or the screen's top), scrolling to it.
/// @param d 1 next, -1 previous
static void next_link(int d) {
    const md_doc_t *md = M->doc;
    if (!md)
        return;
    int i = M->cur_item;
    bool on_screen = i >= 0 && i < md->nitems && md->items[i].y + md->items[i].h > M->top &&
                     md->items[i].y < M->top + VIEW_H;
    if (!on_screen) {  // start from the screen
        i = d > 0 ? md_first_item(md, M->top) - 1 : md->nitems;
        while (d > 0 && i + 1 < md->nitems && md->items[i + 1].y < M->top)
            ++i;
        if (d < 0)
            while (i > 0 && md->items[i - 1].y >= M->top + VIEW_H)
                --i;
    }
    int from_link = on_screen ? M->cur_link : 0;
    for (i += d; i >= 0 && i < md->nitems; i += d) {
        const md_item_t *it = &md->items[i];
        if (it->link && it->link != from_link) {
            // Back: the link's first item.
            M->cur_item = i;
            M->cur_link = it->link;
            if (it->y < M->top + 8 || it->y + it->h > M->top + VIEW_H - 8)
                scroll_to(it->y - VIEW_H / 3);
            M->dirty = true;
            return;
        }
    }
    message(d > 0 ? "No more links below" : "No more links above");
}

/// Finds the text looked for, after the place it was last found (or from the screen's top).
/// @param again from the last place found
static void find_next(bool again) {
    const md_doc_t *d = M->doc;
    int n = (int)strlen(M->find);
    if (!d || !n)
        return;
    int start = again && M->find_item >= 0 ? M->find_item : md_first_item(d, M->top);
    int off = again && M->find_item >= 0 ? M->find_off + 1 : 0;
    if (!again)
        while (start < d->nitems && d->items[start].y < M->top)
            ++start;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = start; i < d->nitems; ++i, off = 0) {
            const md_item_t *it = &d->items[i];
            if (it->kind != MI_TEXT)
                continue;
            const char *s = d->pool + it->ref;
            for (int k = off; k + n <= it->len; ++k)
                if (!strncasecmp(s + k, M->find, n)) {
                    M->find_item = i;
                    M->find_off = k;
                    if (it->y < M->top || it->y + it->h > M->top + VIEW_H)
                        scroll_to(it->y - VIEW_H / 3);
                    if (pass)
                        message("Found again from the top");
                    M->dirty = true;
                    return;
                }
        }
        start = 0, off = 0;
    }
    M->find_item = -1;
    message("\"%.50s\" isn't there", M->find);
}

/// The item under a point on the screen (links and pictures first).
/// @param x the point
/// @param y the point
/// @return the item, or -1
static int item_at(int x, int y) {
    const md_doc_t *d = M->doc;
    if (!d || y < VIEW_Y || y >= STAT_Y || x >= SB_X)
        return -1;
    int32_t py = M->top + y - VIEW_Y;
    int px = x - MARGIN, best = -1;
    for (int i = md_first_item(d, py); i < d->nitems && d->items[i].y <= py; ++i) {
        const md_item_t *it = &d->items[i];
        if (px >= it->x - 1 && px < it->x + it->w + 1 && py >= it->y && py < it->y + it->h &&
            (it->kind != MI_RECT) && (best < 0 || it->link || it->kind == MI_IMG))
            best = i;
    }
    return best;
}

// ---- Commands, keys, the mouse ----------------------------------------------------------------------

/// Does a command from a menu or a key.
/// @param id CMD_*
static void command(int id) {
    M->dirty = true;
    switch (id) {
    case CMD_OPEN: fd_open(); break;
    case CMD_RELOAD:
        if (M->path[0]) {
            int32_t top = M->top;
            char p[PATH_MAX_];
            snprintf(p, sizeof p, "%s", M->path);
            if (open_doc(p, NULL, false))
                scroll_to(top);
        }
        break;
    case CMD_BACK: history(-1); break;
    case CMD_FORWARD: history(1); break;
    case CMD_QUIT: M->quit = true; break;
    case CMD_TOC:
        if (M->doc && M->doc->nheads && !M->source) {
            M->toc_sel = 0;
            for (int i = 0; i < M->doc->nheads; ++i)  // the heading the page is at
                if (M->doc->heads[i].y <= M->top + 8)
                    M->toc_sel = i;
            M->toc_top = M->toc_sel - LIST_ROWS / 2;
            M->dlg = DLG_TOC;
        } else {
            message("No headings");
        }
        break;
    case CMD_SOURCE:
        if (M->doc) {
            // The same part of the text in the other view, roughly.
            int32_t h = M->doc->height;
            int32_t at = M->top;
            M->source = !M->source;
            relayout();
            scroll_to(h ? (int32_t)((int64_t)at * M->doc->height / h) : 0);
        }
        break;
    case CMD_PICTURES:
        M->pictures = !M->pictures;
        if (!M->pictures)
            pics_clear();
        message(M->pictures ? "Pictures shown" : "Pictures as boxes");
        break;
    case CMD_TTF:
        if (!md_ttf_loaded()) {
            char msg[100];
            if (!md_ttf_open(msg, sizeof msg)) {
                message("%s", msg);
                break;
            }
        } else {
            md_ttf_use(!md_ttf_on());
        }
        if (M->doc) {
            int32_t h = M->doc->height, at = M->top;
            relayout();
            scroll_to(h ? (int32_t)((int64_t)at * M->doc->height / h) : 0);
        }
        message(md_ttf_on() ? "TrueType text" : "The 8x16 font");
        break;
    case CMD_TOP: scroll_to(0); break;
    case CMD_END: scroll_to(top_max()); break;
    case CMD_FIND:
        if (M->doc) {
            snprintf(M->in, sizeof M->in, "%s", M->find);
            M->dlg = DLG_FIND;
        }
        break;
    case CMD_NEXT: find_next(true); break;
    case CMD_HELP: M->dlg = DLG_HELP; break;
    case CMD_ABOUT: M->dlg = DLG_ABOUT; break;
    case CMD_FOLLOW: {
        const char *u = link_url(M->cur_link);
        if (u) {
            char url[512];
            snprintf(url, sizeof url, "%s", u);  // (the document may change under it)
            follow(url);
        }
        break;
    }
    case CMD_NEXT_LINK: next_link(1); break;
    case CMD_PREV_LINK: next_link(-1); break;
    }
}

/// A key while the file box is up.
/// @param k the key
static void fd_key(int k) {
    int n = (int)strlen(M->fd_name);
    switch (k) {
    case '\r': case '\n': fd_ok(); break;
    case KEY_ESC: M->dlg = DLG_NONE; break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: case KEY_HOME: case KEY_END:
        M->fd_sel += k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -LIST_ROWS
                   : k == KEY_PGDN ? LIST_ROWS : k == KEY_HOME ? -100000 : 100000;
        list_keep(&M->fd_sel, &M->fd_top, M->fd_n);
        M->fd_name[0] = 0;
        break;
    case 8: case 0x7f:
        if (n)
            M->fd_name[n - 1] = 0;
        else
            fd_go("..");
        break;
    default:
        if (k >= 0x20 && k < 0x7f && n < (int)sizeof M->fd_name - 1) {
            M->fd_name[n] = (char)k;
            M->fd_name[n + 1] = 0;
        }
        break;
    }
}

/// A key while the contents are up.
/// @param k the key
static void toc_key(int k) {
    switch (k) {
    case '\r': case '\n':
        M->dlg = DLG_NONE;
        if (M->toc_sel < M->doc->nheads)
            scroll_to(M->doc->heads[M->toc_sel].y - 4);
        break;
    case KEY_ESC: case 't': case CTRL('t'): M->dlg = DLG_NONE; break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: case KEY_HOME: case KEY_END:
        M->toc_sel += k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -LIST_ROWS
                    : k == KEY_PGDN ? LIST_ROWS : k == KEY_HOME ? -100000 : 100000;
        list_keep(&M->toc_sel, &M->toc_top, M->doc->nheads);
        break;
    }
}

/// A key while the Find box is up.
/// @param k the key
static void find_key(int k) {
    int n = (int)strlen(M->in);
    switch (k) {
    case '\r': case '\n':
        M->dlg = DLG_NONE;
        snprintf(M->find, sizeof M->find, "%s", M->in);
        M->find_item = -1;
        find_next(false);
        break;
    case KEY_ESC: M->dlg = DLG_NONE; break;
    case 8: case 0x7f:
        if (n)
            M->in[n - 1] = 0;
        break;
    default:
        if (k >= 0x20 && k < 0x7f && n < (int)sizeof M->find - 1) {
            M->in[n] = (char)k;
            M->in[n + 1] = 0;
        }
        break;
    }
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    if (M->menu >= 0) {
        menu_key(k);
        return;
    }
    M->dirty = true;
    switch (M->dlg) {
    case DLG_HELP: case DLG_ABOUT: M->dlg = DLG_NONE; return;
    case DLG_FILE: fd_key(k); return;
    case DLG_FIND: find_key(k); return;
    case DLG_TOC: toc_key(k); return;
    case DLG_ASK:
        if (k == '\r' || k == '\n') {
            M->dlg = DLG_NONE;
            M->www = true;
        } else if (k == KEY_ESC) {
            M->dlg = DLG_NONE;
        }
        return;
    }
    bool shift = (usb_keyboard_modifiers() & SHIFT_BITS) != 0;
    switch (k) {
    case KEY_F9: case KEY_F10: menu_open(0, false); M->msel = 0; break;
    case KEY_UP: scroll_to(M->top - STEP); break;
    case KEY_DOWN: scroll_to(M->top + STEP); break;
    case KEY_PGUP: case 'b': scroll_to(M->top - (VIEW_H - 32)); break;
    case KEY_PGDN: case ' ': scroll_to(M->top + (VIEW_H - 32)); break;
    case KEY_HOME: command(CMD_TOP); break;
    case KEY_END: command(CMD_END); break;
    case '\t': next_link(shift ? -1 : 1); break;
    case '\r': case '\n': case KEY_RIGHT: command(CMD_FOLLOW); break;
    case 8: case 0x7f: case KEY_LEFT: case '[': command(CMD_BACK); break;
    case ']': command(CMD_FORWARD); break;
    case CTRL('t'): case 't': command(CMD_TOC); break;
    case CTRL('f'): case '/': command(CMD_FIND); break;
    case KEY_F3: case 'n': command(CMD_NEXT); break;
    case KEY_F6: case 's': command(CMD_SOURCE); break;
    case '*': case 'p': command(CMD_PICTURES); break;
    case KEY_F7: case 'f': command(CMD_TTF); break;
    case KEY_F5: case 'r': case CTRL('r'): command(CMD_RELOAD); break;
    case CTRL('o'): case 'o': command(CMD_OPEN); break;
    case CTRL('b'):  // the SRAM strip or the PSRAM buffer (to compare their times in About)
        M->use_strip = !M->use_strip;
        message(M->use_strip ? "Drawn through an SRAM strip" : "Drawn in a PSRAM buffer");
        break;
    case KEY_F1: case '?': command(CMD_HELP); break;
    case KEY_ESC: case 'q': command(CMD_QUIT); break;
    default:
        if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z')
            for (int i = 0; i < NMENUS; ++i)
                if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                    menu_open(i, false);
        break;
    }
}

static void menu_mouse(const mouse_event_t *e);

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    M->mx = e->x, M->my = e->y;
    if (M->menu >= 0) {
        menu_mouse(e);
        return;
    }
    if (e->type == MOUSE_UP) {
        M->mdrag = 0;
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (M->mdrag) {
            int th = (int)((int64_t)VIEW_H * VIEW_H / (M->doc && M->doc->height > VIEW_H ? M->doc->height : VIEW_H));
            if (th < 16)
                th = 16;
            if (VIEW_H > th)
                scroll_to((int32_t)((int64_t)(e->y - VIEW_Y - M->drag_dy) * top_max() / (VIEW_H - th)));
            return;
        }
        int hl = 0;
        if (!M->dlg) {
            int i = item_at(e->x, e->y);
            hl = i >= 0 ? M->doc->items[i].link : 0;
        }
        if (hl != M->hover_link)
            M->hover_link = hl, M->dirty = true;
        return;
    }
    if (e->type == MOUSE_WHEEL) {
        int d = e->wheel > 0 ? -1 : 1;
        if (M->dlg == DLG_FILE) {
            M->fd_sel += d * 3;
            list_keep(&M->fd_sel, &M->fd_top, M->fd_n);
            M->dirty = true;
        } else if (M->dlg == DLG_TOC) {
            M->toc_sel += d * 3;
            list_keep(&M->toc_sel, &M->toc_top, M->doc->nheads);
            M->dirty = true;
        } else if (!M->dlg) {
            scroll_to(M->top + d * 3 * STEP / 2);
        }
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type != MOUSE_DOWN || !h)
        return;
    if (e->button == MOUSE_RIGHT && !M->dlg && h->id == HIT_PAGE) {
        int i = item_at(e->x, e->y);
        M->pop_link = i >= 0 ? M->doc->items[i].link : 0;
        M->popx = e->x, M->popy = e->y;
        menu_open(NMENUS, true);
        return;
    }
    M->dirty = true;
    switch (h->id) {
    case HIT_MENU: menu_open(h->arg, true); break;
    case HIT_PAGE: {
        int i = item_at(e->x, e->y);
        if (i < 0)
            break;
        const md_item_t *it = &M->doc->items[i];
        if (it->link) {
            M->cur_item = i;
            M->cur_link = it->link;
            command(CMD_FOLLOW);
        } else if (it->kind == MI_IMG) {
            char path[PATH_MAX_];
            resolve(M->doc->pool + M->doc->imgs[it->ref].src, path, sizeof path);
            show_picture(path);
        }
        break;
    }
    case HIT_SB: {
        int32_t total = M->doc ? M->doc->height : VIEW_H;
        int th = VIEW_H, ty = 0;
        if (total > VIEW_H) {
            th = (int)((int64_t)VIEW_H * VIEW_H / total);
            if (th < 16)
                th = 16;
            ty = (int)((int64_t)(VIEW_H - th) * M->top / top_max());
        }
        int y = e->y - VIEW_Y;
        if (y < ty)
            scroll_to(M->top - (VIEW_H - 32));
        else if (y >= ty + th)
            scroll_to(M->top + (VIEW_H - 32));
        else
            M->mdrag = 1, M->drag_dy = y - ty;
        break;
    }
    case HIT_ASK:
        M->dlg = DLG_NONE;
        M->www = h->arg == 0;
        break;
    case HIT_ROW: {
        M->fd_sel = h->arg;
        const fd_entry_t *fe = &M->fd[h->arg];
        if (fe->dir) {
            M->fd_name[0] = 0;
            fd_go(M->fd_pool + fe->name);
        } else {
            snprintf(M->fd_name, sizeof M->fd_name, "%s", M->fd_pool + fe->name);
            if (e->double_click)
                fd_ok();
        }
        break;
    }
    case HIT_FD_OK: fd_ok(); break;
    case HIT_FD_CANCEL: M->dlg = DLG_NONE; break;
    case HIT_FD_UP: fd_go(".."); break;
    case HIT_IN_OK: find_key('\r'); break;
    case HIT_IN_CANCEL: M->dlg = DLG_NONE; break;
    case HIT_TOC:
        M->toc_sel = h->arg;
        toc_key('\r');
        break;
    case HIT_SWALLOW:
        if (M->dlg == DLG_HELP || M->dlg == DLG_ABOUT || M->dlg == DLG_TOC)
            M->dlg = DLG_NONE;
        break;
    }
}

/// The mouse while a menu is open: moving chooses, a click on an item (or letting go on one, of the button
/// that opened it) runs it, a click elsewhere closes it.
/// @param e the event
static void menu_mouse(const mouse_event_t *e) {
    const hit_t *h = hit_at(e->x, e->y);
    int id = h ? h->id : 0;
    if (e->type == MOUSE_WHEEL) {
        menu_key(e->wheel > 0 ? KEY_UP : KEY_DOWN);
        return;
    }
    if (id == HIT_MENU && h->arg != M->menu && e->type != MOUSE_UP) {
        menu_open(h->arg, M->marmed || e->type == MOUSE_DOWN);
        return;
    }
    if (id == HIT_ITEM) {
        if (M->msel != h->arg)
            M->msel = h->arg, M->dirty = true;
        if (e->type == MOUSE_DOWN || (e->type == MOUSE_UP && M->marmed)) {
            item_run(h->arg);
            return;
        }
    } else if (M->msel >= 0 && e->type == MOUSE_MOVE) {
        M->msel = -1;
        M->dirty = true;
    }
    if (e->type == MOUSE_DOWN && id != HIT_ITEM)
        menu_close();
    if (e->type == MOUSE_UP)
        M->marmed = false;
}

// ---- The viewer ----------------------------------------------------------------------------------

/// The viewer's loop.
static void viewer(void) {
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    draw();
    bool msg_shown = M->msg[0] != 0;
    while (!M->quit) {
        int k;
        while (!M->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!M->quit && mouse_event(&e))
            mouse(&e);
        mouse_pointer_update();
        bool msg_on = M->msg[0] && now_ms() < M->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            M->dirty = true;
        }
        if (M->quit)
            break;
        if (M->www) {
            M->www = false;
            run_www(M->ask_url);
        }
        if (M->dirty) {
            M->dirty = false;
            draw();
        } else if (M->pic_wanted && M->menu < 0 && M->dlg == DLG_NONE) {
            // Pictures on the screen are drawn while nothing else is to be done.
            if (!pic_load_one())
                M->pic_wanted = false;
            M->dirty = true;
        } else {
            sleep_ms(M->mdrag ? 1 : 5);
        }
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
}

/// Frees everything.
static void stop(void) {
    pics_clear();
    md_free(M->doc);
    md_ttf_close();
    psram_free(M->text);
    psram_free(M->g);
    free(M->strip);
    if (M->saved) {
        memcpy(dvi_framebuf, M->saved, W * H);
        psram_free(M->saved);
    }
    psram_free(M);
    M = NULL;
}

/// Starts the state (PSRAM), with the screen as it is kept to be put back.
/// @return false if there is no memory for it
static bool start(void) {
    M = psram_malloc(sizeof *M);
    if (!M)
        return false;
    memset(M, 0, sizeof *M);
    M->menu = -1;
    M->cur_item = -1;
    M->find_item = -1;
    M->pictures = true;
    M->t = dvi_framebuf;
    M->cy1 = H;
    for (int i = 0; i < MAX_PICS; ++i)
        M->pics[i].item = -1;
    M->g = psram_malloc(W * H);
    M->saved = psram_malloc(W * H);
    M->strip = malloc(STRIP * W);  // 10 KB of SRAM while the viewer runs (without it: g)
    M->use_strip = true;
    M->by1 = H;
    if (!M->g || !M->saved) {
        psram_free(M->g);
        psram_free(M->saved);
        psram_free(M);
        M = NULL;
        return false;
    }
    memcpy(M->saved, dvi_framebuf, W * H);
    if (f_getcwd(M->dir, sizeof M->dir) != FR_OK)
        strcpy(M->dir, "/");
    uint32_t t0 = now_ms();
    md_ttf_open(M->msg, sizeof M->msg);  // TrueType text if there are fonts (else the 8x16 font)
    M->ttf_ms = now_ms() - t0;
    M->msg_until = now_ms() + 4000;
    return true;
}

void md_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        printf("usage: md [file.md]   the Markdown viewer (markdown too); F1 in it lists its keys\n");
        return;
    }
    if (!start()) {
        printf("md: not enough memory\n");
        return;
    }
    if (argc > 1) {
        char path[PATH_MAX_];
        resolve(argv[1], path, sizeof path);
        FILINFO fi;
        bool root = !strcmp(path, "/");
        if (!root && f_stat(path, &fi) != FR_OK) {
            printf("md: %s isn't there\n", argv[1]);
            stop();
            return;
        }
        if (root || (fi.fattrib & AM_DIR)) {
            snprintf(M->fd_dir, sizeof M->fd_dir, "%s", path);
            M->dlg = DLG_FILE;
            fd_read();
        } else if (!open_doc(path, NULL, true)) {
            printf("md: %s\n", M->msg);
            stop();
            return;
        }
    } else {
        fd_open();
    }
    viewer();
    stop();
}
