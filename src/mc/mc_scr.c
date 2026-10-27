/**
 * @file mc_scr.c
 * @brief mc's screen and keyboard.
 *
 * Everything is drawn into `want`; scr_flush sends only the cells that
 * changed straight to the console (no escape sequences, no flicker). The
 * buffers are in PSRAM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "console.h"
#include "mouse.h"
#include "dvi.h"
#include "mc.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

#define CELLS (SCR_W * SCR_H)

typedef struct {
    ///@{
    uint8_t ch[CELLS], at[CELLS];
    ///@}
    /// the cursor
    ///@{
    int row, col;
    ///@}
} screen_t;

static screen_t *want, *have, *user;
static int cur_row = -1, cur_col;
/// Overlay mode: the picture under the cells, put back where a cell stops
/// being drawn (a dialog closing).
static const uint8_t *under;

void scr_begin(void) {
    want = psram_malloc(sizeof *want);
    have = psram_malloc(sizeof *have);
    user = psram_malloc(sizeof *user);
    scr_grab_user();
    memcpy(have, user, sizeof *have);
    memcpy(want, user, sizeof *want);
    printf("\x1b[?25l");
    fflush(stdout);
}

bool scr_active(void) {
    return want != NULL;
}

void scr_end(void) {
    under = NULL;
    scr_show_user();
    psram_free(want);
    psram_free(have);
    psram_free(user);
    want = have = user = NULL;
}

void scr_overlay(const uint8_t *picture) {
    under = picture;
    if (!picture)
        return;
    // Cells marked "not mine" (attribute 0xff) in both buffers.
    memset(want->ch, 0, CELLS);
    memset(want->at, 0xff, CELLS);
    memcpy(have->ch, want->ch, CELLS);
    memcpy(have->at, want->at, CELLS);
}

static void put_back(int i) {
    int x = i % SCR_W * 8, y = i / SCR_W * 16;
    for (int k = 0; k < 16; ++k)
        memcpy(dvi_framebuf + (y + k) * DVI_WIDTH + x, under + (y + k) * DVI_WIDTH + x, 8);
}

void scr_put(int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= SCR_H || col < 0 || col >= SCR_W)
        return;
    want->ch[row * SCR_W + col] = ch;
    want->at[row * SCR_W + col] = attr;
}

void scr_get(int row, int col, uint8_t *ch, uint8_t *attr) {
    *ch = want->ch[row * SCR_W + col];
    *attr = want->at[row * SCR_W + col];
}

void scr_fill(int row, int col, int h, int w, uint8_t ch, uint8_t attr) {
    for (int r = row; r < row + h; ++r)
        for (int c = col; c < col + w; ++c)
            scr_put(r, c, ch, attr);
}

void scr_text(int row, int col, int width, uint8_t attr, const char *s) {
    if (width < 0)
        width = strlen(s);
    for (int i = 0; i < width; ++i)
        scr_put(row, col + i, *s ? (uint8_t)*s++ : ' ', attr);
}

void scr_printf(int row, int col, int width, uint8_t attr, const char *fmt, ...) {
    char text[SCR_W * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    scr_text(row, col, width, attr, text);
}

void scr_box(int row, int col, int h, int w, bool dbl, uint8_t attr) {
    uint8_t hz = dbl ? D_H : S_H, vt = dbl ? D_V : S_V;
    scr_put(row, col, dbl ? D_TL : 0xda, attr);
    scr_put(row, col + w - 1, dbl ? D_TR : 0xbf, attr);
    scr_put(row + h - 1, col, dbl ? D_BL : 0xc0, attr);
    scr_put(row + h - 1, col + w - 1, dbl ? D_BR : 0xd9, attr);
    for (int c = col + 1; c < col + w - 1; ++c) {
        scr_put(row, c, hz, attr);
        scr_put(row + h - 1, c, hz, attr);
    }
    for (int r = row + 1; r < row + h - 1; ++r) {
        scr_put(r, col, vt, attr);
        scr_put(r, col + w - 1, vt, attr);
    }
}

void scr_hline(int row, int col, int w, uint8_t left, uint8_t mid, uint8_t right, uint8_t attr) {
    scr_put(row, col, left, attr);
    for (int c = col + 1; c < col + w - 1; ++c)
        scr_put(row, c, mid, attr);
    scr_put(row, col + w - 1, right, attr);
}

void scr_cursor(int row, int col) {
    cur_row = row;
    cur_col = col;
}

void scr_flush(void) {
    static bool cursor_on;
    // An arrow pointer (over the tracker's graphics) keeps the pixels under
    // it: off while the cells change, then drawn again on top.
    mouse_pointer_hide();
    // The console cursor blinks by drawing on the screen: off while drawing.
    if (cursor_on) {
        printf("\x1b[?25l");
        fflush(stdout);
        cursor_on = false;
    }
    for (int i = 0; i < CELLS; ++i) {
        if (want->ch[i] != have->ch[i] || want->at[i] != have->at[i]) {
            if (want->at[i] == 0xff && under)
                put_back(i);
            else
                console_set_cell(i / SCR_W, i % SCR_W, want->ch[i], want->at[i]);
            have->ch[i] = want->ch[i];
            have->at[i] = want->at[i];
        }
    }
    if (cur_row >= 0) {
        // Its colours come from the console's current ones: the command line's.
        printf("\x1b[0;37;40m\x1b[%d;%dH\x1b[?25h", cur_row + 1, cur_col + 1);
        fflush(stdout);
        cursor_on = true;
    }
    mouse_pointer_show();
}

void *scr_save(void) {
    screen_t *s = psram_malloc(sizeof *s);
    if (s)
        memcpy(s, want, sizeof *s);
    return s;
}

void scr_restore(void *saved) {
    if (!saved)
        return;
    memcpy(want->ch, ((screen_t *)saved)->ch, CELLS);
    memcpy(want->at, ((screen_t *)saved)->at, CELLS);
    psram_free(saved);
}

void scr_grab_user(void) {
    for (int i = 0; i < CELLS; ++i)
        console_get_cell(i / SCR_W, i % SCR_W, &user->ch[i], &user->at[i]);
    console_get_cursor(&user->row, &user->col);
    if (have) {  // that is what the console shows now
        memcpy(have->ch, user->ch, CELLS);
        memcpy(have->at, user->at, CELLS);
    }
}

void scr_show_user(void) {
    for (int i = 0; i < CELLS; ++i) {
        console_set_cell(i / SCR_W, i % SCR_W, user->ch[i], user->at[i]);
        have->ch[i] = user->ch[i];
        have->at[i] = user->at[i];
    }
    printf("\x1b[0m\x1b[%d;%dH\x1b[?25h", user->row + 1, user->col + 1);
    fflush(stdout);
}

// ---- Keys -----------------------------------------------------------------------------------------

static int pending[8], npending;

static int raw(uint32_t us) {
    if (npending) {
        int c = pending[0];
        memmove(pending, pending + 1, --npending * sizeof *pending);
        return c;
    }
    int c = getchar_timeout_us(us);
    return c < 0 ? -1 : c;
}

static int escape(void) {
    // A key's sequence arrives at once; Esc typed by hand is followed (or
    // not) by a digit or letter within half a second, as in mc.
    int c = raw(500000);
    if (c < 0 || c == 0x1b)
        return KEY_ESC;
    if (c == '\r' || c == '\n')
        return KEY_ALT + '\r';
    if (c >= '0' && c <= '9')
        return c == '0' ? KEY_F10 : KEY_F1 + (c - '1');
    if (c != '[' && c != 'O')
        return c >= 0x20 && c < 0x7f ? KEY_ALT + (c >= 'A' && c <= 'Z' ? c + 32 : c) : KEY_ESC;
    int n = 0, f = raw(30000);
    while (f >= '0' && f <= '9') {
        n = n * 10 + (f - '0');
        f = raw(30000);
    }
    switch (f) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    case '~':
        switch (n) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 2: return KEY_INS;
        case 3: return KEY_DEL;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        case 11: case 12: case 13: case 14: return KEY_F1 + (n - 11);
        case 15: return KEY_F5;
        case 17: case 18: case 19: case 20: case 21: return KEY_F6 + (n - 17);
        case 23: return KEY_F11;
        case 24: return KEY_F12;
        }
    }
    return KEY_NONE;
}

int key_poll(void) {
    int c = raw(0);
    if (c < 0)
        return KEY_NONE;
    if (c == 0x1b)
        return escape();
    if (c == 0x7f)
        return '\b';
    if (c == '\n')
        return '\r';
    return c;
}

int key_get_mouse(mouse_event_t *e) {
    for (;;) {
        int k = key_poll();
        if (k != KEY_NONE)
            return k;
        mouse_pointer_update();  // the arrow over a graphics screen follows
        // Mouse events for the caller; without one (e NULL) they are dropped.
        mouse_event_t m;
        while (mouse_event(&m))
            if (e) {
                *e = m;
                return KEY_MOUSE;
            }
        sleep_ms(5);
    }
}

int key_get(void) {
    return key_get_mouse(NULL);
}

bool key_stop(void) {
    int c = raw(0);
    if (c < 0)
        return false;
    if (c == 3)
        return true;
    if (c == 0x1b) {
        // Esc alone stops; Esc [ ... is a key like an arrow: kept for later.
        int n = raw(30000);
        if (n != '[' && n != 'O')
            return true;
        if (npending < 6) {
            pending[npending++] = c;
            pending[npending++] = n;
        }
        return false;
    }
    if (npending < 8)
        pending[npending++] = c;
    return false;
}
