/**
 * @file genesis_launcher.c
 * @brief The genesis command's game launcher (genesis_launcher.h).
 *
 * It looks like the tracker's load dialog: a raised panel on the tracker's
 * colours, a black list with a blue bar on the highlighted line, sizes in
 * cyan, a scroll bar, and a menu-bar line at the top and a status line at
 * the bottom.
 *
 * The list is the folder's subfolders and its .bin, .md, .gen and .smd
 * files, folders first, then by name (case ignored). Only the highlighted
 * game's header is read (512 bytes, or two 256-byte pieces of an .smd's
 * first block), when the cursor reaches it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is cut to fit its field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "genesis_launcher.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dvi.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "trk.h"
#include "usb_keyboard.h"

#define MAX_ENTRIES 1024
#define NAME_LEN    256

typedef struct {
    char name[NAME_LEN];
    uint32_t size;
    bool dir;
} entry;

static entry *list;  ///< in PSRAM while the launcher is up
static int count, folders, games;  ///< entries (".." too), subfolders, games

/// The layout, in pixels (the 8x16 font: 80 x 30 characters).
#define BAR_H    18
#define BOX_X  8
#define BOX_Y  22
#define BOX_W  624
#define BOX_H  434
#define PATH_Y   (BOX_Y + 26)
#define COUNT_Y  (PATH_Y + 24)
#define LIST_X   24
#define LIST_Y   (COUNT_Y + 24)
#define LIST_COLS 44
#define LIST_W   (LIST_COLS * 8 + 8)
#define ROWS     19
#define LIST_H   (ROWS * 16 + 4)
#define SCROLL_X (LIST_X + LIST_W)
#define INFO_X   (SCROLL_X + 24)
#define INFO_W   (BOX_X + BOX_W - 16 - INFO_X)
#define INFO_COLS ((INFO_W - 8) / 8)
#define BUTTON_Y (LIST_Y + LIST_H + 12)
#define STATUS_Y (DVI_HEIGHT - 18)

// ---- The folder ----

static bool is_game(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".gen") ||
                   !strcasecmp(dot, ".smd"));
}

static int entry_cmp(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    if (!strcmp(x->name, ".."))
        return -1;
    if (!strcmp(y->name, ".."))
        return 1;
    return strcasecmp(x->name, y->name);
}

static bool is_root(const char *dir) {
    return !dir[0] || !strcmp(dir, "/");
}

/// Reads dir into list; false (and why in err) if it can't.
static bool read_folder(const char *dir, char *err, size_t err_len) {
    count = folders = games = 0;
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK) {
        snprintf(err, err_len, "Can't open %s", dir);
        return false;
    }
    if (!is_root(dir)) {
        strcpy(list[count].name, "..");
        list[count].dir = true;
        list[count++].size = 0;
    }
    while (count < MAX_ENTRIES && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS) || fi.fname[0] == '.')
            continue;
        bool dir_entry = fi.fattrib & AM_DIR;
        if (!dir_entry && !is_game(fi.fname))
            continue;
        entry *e = &list[count++];
        snprintf(e->name, sizeof e->name, "%s", fi.fname);
        e->dir = dir_entry;
        e->size = (uint32_t)fi.fsize;
        folders += dir_entry;
        games += !dir_entry;
    }
    f_closedir(&d);
    qsort(list, count, sizeof *list, entry_cmp);
    return true;
}

static void join(char *out, size_t n, const char *dir, const char *name) {
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name);
}

// ---- A cartridge's header ----

typedef struct {
    bool ok;
    char system[17], title[49], domestic[49], serial[15], region[24];
} rom_info;

/// A header field: printable, runs of spaces made one, trimmed.
static void field(char *out, const uint8_t *in, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) {
        char c = in[i] >= 32 && in[i] < 127 ? (char)in[i] : ' ';
        if (c != ' ' || (n && out[n - 1] != ' '))
            out[n++] = c;
    }
    while (n && out[n - 1] == ' ')
        --n;
    out[n] = 0;
}

/// The countries at $1F0: the letters J, U and E, or one hex digit of bits
/// (1 Japan, 4 USA, 8 Europe) on later cartridges.
static void region(char *out, size_t n, const uint8_t *r) {
    bool j = false, u = false, e = false;
    if (r[1] == ' ' && isxdigit(r[0]) && !strchr("JUE", r[0])) {
        int bits = isdigit(r[0]) ? r[0] - '0' : toupper(r[0]) - 'A' + 10;
        j = bits & 1, u = bits & 4, e = bits & 8;
    } else {
        for (int i = 0; i < 3; i++)
            j |= r[i] == 'J', u |= r[i] == 'U', e |= r[i] == 'E';
    }
    snprintf(out, n, "%s%s%s%s%s", j ? "Japan" : "", j && (u || e) ? ", " : "", u ? "USA" : "",
             u && e ? ", " : "", e ? "Europe" : "");
    if (!out[0])
        snprintf(out, n, "?");
}

static bool read_at(FIL *f, FSIZE_t at, uint8_t *buf, UINT n) {
    UINT got;
    return f_lseek(f, at) == FR_OK && f_read(f, buf, n, &got) == FR_OK && got == n;
}

static void read_info(const char *path, rom_info *ri) {
    memset(ri, 0, sizeof *ri);
    uint8_t h[0x200];
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return;
    bool ok = read_at(&f, 0, h, sizeof h);
    const char *dot = strrchr(path, '.');
    if (ok && ((dot && !strcasecmp(dot, ".smd")) || (h[1] == 3 && h[8] == 0xaa && h[9] == 0xbb))) {
        // An .smd: a 512-byte header, then 16 KB blocks with the odd bytes
        // in the first half and the even bytes in the second.
        uint8_t odd[0x100], even[0x100];
        ok = read_at(&f, 0x200, odd, sizeof odd) && read_at(&f, 0x200 + 0x2000, even, sizeof even);
        for (int i = 0; ok && i < 0x100; i++) {
            h[i * 2] = even[i];
            h[i * 2 + 1] = odd[i];
        }
    }
    f_close(&f);
    if (!ok)
        return;
    field(ri->system, h + 0x100, 16);
    field(ri->domestic, h + 0x120, 48);
    field(ri->title, h + 0x150, 48);
    field(ri->serial, h + 0x180, 14);
    region(ri->region, sizeof ri->region, h + 0x1f0);
    ri->ok = true;
}

// ---- Drawing ----

static void size_text(char *out, size_t n, uint32_t size) {
    if (size >= 1024 * 1024)
        snprintf(out, n, "%lu.%luM", (unsigned long)(size >> 20), (unsigned long)((size & 0xfffff) * 10 >> 20));
    else
        snprintf(out, n, "%luK", (unsigned long)((size + 1023) >> 10));
}

static void draw_frame(const char *dir) {
    gfx_fill(0, 0, DVI_WIDTH, DVI_HEIGHT, C_BG);
    // The menu bar's line.
    gfx_fill(0, 0, DVI_WIDTH, BAR_H, C_FACE);
    gfx_text(16, 1, "Games", -1, C_TEXT, C_FACE);
    gfx_text(DVI_WIDTH - 8 - 12 * 8, 1, "Pico Genesis", -1, C_YELLOW, C_FACE);
    // The panel, with its title, the folder and the counts.
    gfx_bevel(BOX_X, BOX_Y, BOX_W, BOX_H, true, C_FACE);
    gfx_text(BOX_X + 8, BOX_Y + 4, "Choose a game", -1, C_YELLOW, C_FACE);
    gfx_fill(LIST_X, PATH_Y, BOX_X + BOX_W - 16 - LIST_X, 16, C_FIELD);
    gfx_text(LIST_X + 4, PATH_Y, dir, (BOX_X + BOX_W - 24 - LIST_X) / 8, C_CYAN, C_FIELD);
    gfx_printf(LIST_X, COUNT_Y, 40, C_LABEL, C_FACE, "%d folder%s, %d game%s", folders, folders == 1 ? "" : "s",
               games, games == 1 ? "" : "s");
    // The list's and the information's sunken boxes, and the buttons.
    gfx_bevel(LIST_X - 2, LIST_Y - 2, LIST_W + 20, LIST_H + 4, false, C_FIELD);
    gfx_bevel(INFO_X - 2, LIST_Y - 2, INFO_W + 4, LIST_H + 4, false, C_FIELD);
    gfx_bevel(LIST_X + LIST_W - 2 * 128 - 16, BUTTON_Y, 120, 22, true, C_BLUE);
    gfx_text(LIST_X + LIST_W - 2 * 128 - 16 + 44, BUTTON_Y + 3, "Play", -1, C_YELLOW, C_BLUE);
    gfx_bevel(LIST_X + LIST_W - 128 + 4, BUTTON_Y, 120, 22, true, C_FACE);
    gfx_text(LIST_X + LIST_W - 128 + 4 + 36, BUTTON_Y + 3, "Cancel", -1, C_TEXT, C_FACE);
}

static void draw_list(int top, int cur) {
    for (int r = 0; r < ROWS; r++) {
        int i = top + r, y = LIST_Y + r * 16;
        bool lit = i == cur;
        uint8_t bg = lit ? C_CURROW : C_FIELD;
        if (i >= count) {
            gfx_fill(LIST_X, y, LIST_W, 16, C_FIELD);
            continue;
        }
        const entry *e = &list[i];
        char size[12];
        if (!strcmp(e->name, ".."))
            snprintf(size, sizeof size, "<UP>");
        else if (e->dir)
            snprintf(size, sizeof size, "<DIR>");
        else
            size_text(size, sizeof size, e->size);
        gfx_fill(LIST_X, y, 4, 16, bg);
        int name_cols = LIST_COLS - 7;
        int end = gfx_text(LIST_X + 4, y, e->name, name_cols, lit ? C_YELLOW : e->dir ? C_YELLOW : C_TEXT, bg);
        gfx_printf(end, y, 7, lit ? C_YELLOW : e->dir ? C_YELLOW : C_CYAN, bg, "%7s", size);
        gfx_fill(end + 7 * 8, y, LIST_X + LIST_W - end - 7 * 8, 16, bg);
    }
    // The scroll bar: arrows at the ends, the thumb where the view is.
    int sx = SCROLL_X, sy = LIST_Y, sh = LIST_H - 4;
    gfx_fill(sx, sy, 16, sh, C_FACE);
    gfx_bevel(sx, sy, 16, 16, true, C_FACE);
    gfx_char(sx + 4, sy, 0x18, C_TEXT, C_FACE);
    gfx_bevel(sx, sy + sh - 16, 16, 16, true, C_FACE);
    gfx_char(sx + 4, sy + sh - 16, 0x19, C_TEXT, C_FACE);
    int track = sh - 32, thumb = count > ROWS ? track * ROWS / count : track;
    if (thumb < 8)
        thumb = 8;
    int at = count > ROWS ? (track - thumb) * top / (count - ROWS) : 0;
    gfx_bevel(sx, sy + 16 + at, 16, thumb, true, C_BLUE);
}

/// The highlighted game's header (or what the line is), in the box on the right.
static void draw_info(const char *dir, int cur) {
    gfx_fill(INFO_X, LIST_Y, INFO_W, LIST_H, C_FIELD);
    int x = INFO_X + 4, y = LIST_Y + 2;
    if (cur >= count)
        return;
    const entry *e = &list[cur];
    if (e->dir) {
        gfx_text(x, y, strcmp(e->name, "..") ? "Folder" : "Up a folder", -1, C_LABEL, C_FIELD);
        gfx_text(x, y + 20, e->name, INFO_COLS, C_YELLOW, C_FIELD);
        return;
    }
    char path[NAME_LEN + 320];
    join(path, sizeof path, dir, e->name);
    rom_info ri;
    read_info(path, &ri);
    // A label, and its text over as many lines as it needs (at most max).
    struct {
        const char *label, *text;
        uint8_t colour;
        int max;
    } rows[] = {
        {"Title", ri.title, C_YELLOW, 3},
        {"Japanese title", ri.domestic, C_TEXT, 3},
        {"Serial", ri.serial, C_CYAN, 1},
        {"Countries", ri.region, C_GREEN, 1},
        {"Made for", ri.system, C_TEXT, 1},
    };
    if (!ri.ok) {
        gfx_text(x, y, "Can't read its header", -1, C_ORANGE, C_FIELD);
        y += 24;
    } else {
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            gfx_text(x, y, rows[i].label, -1, C_LABEL, C_FIELD);
            y += 16;
            const char *t = rows[i].text[0] ? rows[i].text : "-";
            for (int line = 0; line < rows[i].max && *t; line++, y += 16) {
                int n = (int)strlen(t);
                if (n > INFO_COLS) {  // break at a space if there is one
                    n = INFO_COLS;
                    for (int k = INFO_COLS; k > INFO_COLS / 2; k--)
                        if (t[k] == ' ') {
                            n = k;
                            break;
                        }
                }
                gfx_text(x, y, t, n, rows[i].colour, C_FIELD);
                t += n;
                while (*t == ' ')
                    t++;
            }
            y += 6;
        }
    }
    char size[12];
    size_text(size, sizeof size, e->size);
    gfx_text(x, y, "Size", -1, C_LABEL, C_FIELD);
    gfx_text(x, y + 16, size, -1, C_CYAN, C_FIELD);
    y += 38;
    gfx_text(x, y, "File", -1, C_LABEL, C_FIELD);
    gfx_text(x, y + 16, e->name, INFO_COLS, C_TEXT, C_FIELD);
}

static void draw_status(const char *text, uint8_t colour) {
    gfx_fill(0, STATUS_Y, DVI_WIDTH, 18, C_FACE);
    gfx_text(8, STATUS_Y + 1, text, 78, colour, C_FACE);
}

static void draw_help(void) {
    draw_status("Return/Start play  Esc/F10 leave  Backspace up  F5 read again", C_LABEL);
}

void genesis_launcher_progress(const char *what, int percent) {
    char text[80];
    snprintf(text, sizeof text, "Loading %s  %d%%", what, percent);
    draw_status(text, C_YELLOW);
}

// ---- Keys ----

enum {
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END, K_F5, K_F10,
    K_PLAY, K_BACK, K_LEAVE,
};

/// A key from the console (USB keyboard or serial), or -1.
static int get_key(void) {
    int c = getchar_timeout_us(0);
    if (c != 0x1b)
        return c == '\r' || c == '\n' ? K_PLAY : c == 8 || c == 127 ? K_BACK : c;
    int c2 = getchar_timeout_us(2000);
    if (c2 != '[' && c2 != 'O')
        return K_LEAVE;  // Esc on its own
    int num = 0, fin;
    while ((fin = getchar_timeout_us(2000)) >= '0' && fin <= '9')
        num = num * 10 + (fin - '0');
    switch (fin) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        switch (num) {
        case 1: return K_HOME;
        case 4: return K_END;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        case 15: return K_F5;
        case 21: return K_F10;
        }
    }
    return -1;
}

/// A USB gamepad: the D-pad (with repeat), button 1 or a Start button plays,
/// button 2 goes up a folder.
static int pad_key(void) {
    static uint32_t held_since, last_repeat;
    static int held = -1;
    uint8_t x, y;
    uint32_t b;
    int k = -1;
    if (usb_gamepad_read(&x, &y, &b)) {
        k = y < 64 ? K_UP : y > 191 ? K_DOWN : x < 64 ? K_PGUP : x > 191 ? K_PGDN
            : b & (1 | 0xf80) ? K_PLAY : b & 2 ? K_BACK : -1;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (k != held) {
        held = k;
        held_since = last_repeat = now;
        return k;
    }
    // Moving keys repeat after 400 ms, every 70 ms.
    if (k >= K_UP && k <= K_PGDN && now - held_since > 400 && now - last_repeat > 70) {
        last_repeat = now;
        return k;
    }
    return -1;
}

// ---- The launcher ----

bool genesis_launcher(char *dir, size_t dir_len, const char *last, const char *message, char *path,
                      size_t path_len) {
    list = psram_malloc(MAX_ENTRIES * sizeof *list);
    if (!list)
        return false;
    gfx_target = dvi_framebuf;
    char err[80] = "";
    char want[NAME_LEN];  // the name to put the cursor on after reading a folder
    snprintf(want, sizeof want, "%s", last ? last : "");
    bool ok = false, reread = true;
    int cur = 0, top = 0;
    while (getchar_timeout_us(0) >= 0) {  // keys typed while the game ran
    }
    for (;;) {
        if (reread) {
            reread = false;
            if (!read_folder(dir, err, sizeof err) && !is_root(dir)) {
                snprintf(dir, dir_len, "/");  // a folder that's gone: start at the top
                read_folder(dir, err, sizeof err);
            }
            cur = 0;
            for (int i = 0; i < count; i++)
                if (want[0] && !strcmp(list[i].name, want))
                    cur = i;
            want[0] = 0;
            top = cur - ROWS / 2;
            draw_frame(dir);
            if (err[0])
                draw_status(err, C_ORANGE);
            else if (message)
                draw_status(message, C_ORANGE);
            else
                draw_help();
            message = NULL;
            err[0] = 0;
            draw_info(dir, cur);
        }
        if (top > count - ROWS)
            top = count - ROWS;
        if (top < 0)
            top = 0;
        draw_list(top, cur);

        int k;
        while ((k = get_key()) < 0 && (k = pad_key()) < 0)
            sleep_ms(10);
        int was = cur;
        switch (k) {
        case K_UP: cur--; break;
        case K_DOWN: cur++; break;
        case K_PGUP: case K_LEFT: cur -= ROWS - 1; break;
        case K_PGDN: case K_RIGHT: cur += ROWS - 1; break;
        case K_HOME: cur = 0; break;
        case K_END: cur = count - 1; break;
        case K_F5:
            if (cur < count)
                snprintf(want, sizeof want, "%s", list[cur].name);
            reread = true;
            break;
        case K_LEAVE: case K_F10: goto done;
        case K_BACK:
            if (is_root(dir))
                break;
            cur = 0;  // "..", always first
            /* fall through */
        case K_PLAY:
            if (cur >= count)
                break;
            if (list[cur].dir) {
                if (!strcmp(list[cur].name, "..")) {
                    char *slash = strrchr(dir, '/');
                    snprintf(want, sizeof want, "%s", slash ? slash + 1 : "");
                    if (slash && slash != dir)
                        *slash = 0;
                    else
                        snprintf(dir, dir_len, "/");
                } else {
                    char sub[NAME_LEN + 320];
                    join(sub, sizeof sub, dir, list[cur].name);
                    snprintf(dir, dir_len, "%s", sub);
                }
                reread = true;
                break;
            }
            join(path, path_len, dir, list[cur].name);
            ok = true;
            goto done;
        default:
            break;
        }
        if (cur >= count)
            cur = count - 1;
        if (cur < 0)
            cur = 0;
        if (cur < top)
            top = cur;
        if (cur >= top + ROWS)
            top = cur - ROWS + 1;
        if (cur != was && !reread)
            draw_info(dir, cur);
    }
done:
    psram_free(list);
    list = NULL;
    return ok;
}
