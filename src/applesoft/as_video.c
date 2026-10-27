/**
 * @file as_video.c
 * @brief The Apple's display, drawn from its screen memory into the 640x480 RGB332 framebuffer.
 *
 * 40- and 80-column text (normal, inverse, flashing), lo-res, hi-res with
 * NTSC-style colour, mixed mode and both pages. The 280x192 picture is
 * doubled and centred. Only rows whose memory changed are redrawn, at most
 * about 60 times a second.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "as_font.h"
#include "as_internal.h"
#include "as_port.h"

struct as_video vid;

#define X0 40
#define Y0 48
#define FBW ASP_FB_WIDTH

#define RGB(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

static const uint8_t lores_rgb[16] = {
    RGB(0, 0, 0), RGB(4, 0, 1), RGB(2, 1, 2), RGB(6, 2, 3), RGB(0, 3, 1), RGB(4, 4, 2),
    RGB(1, 4, 3), RGB(5, 5, 3), RGB(2, 2, 0), RGB(6, 3, 0), RGB(4, 4, 2), RGB(7, 4, 2),
    RGB(1, 5, 0), RGB(5, 6, 1), RGB(3, 7, 2), RGB(7, 7, 3),
};
#define C_BLACK  RGB(0, 0, 0)
#define C_WHITE  RGB(7, 7, 3)
#define C_VIOLET RGB(6, 2, 3)
#define C_GREEN  RGB(1, 5, 0)
#define C_BLUE   RGB(1, 4, 3)
#define C_ORANGE RGB(6, 3, 0)

static uint32_t dirty_text;      ///< text rows 0-23
static uint32_t dirty_hgr[6];    ///< hi-res lines 0-191
static bool all_dirty = true;
static uint64_t last_draw_us, flash_us;
static bool flash_phase;
static bool cur_on;
static int cur_row, cur_col;

void video_init(void) {
    memset(&vid, 0, sizeof vid);
    vid.text = true;
    memset(asp_framebuffer(), C_BLACK, ASP_FB_WIDTH * ASP_FB_HEIGHT);
    all_dirty = true;
}

void video_mark_all(void) {
    all_dirty = true;
}

void video_mark(uint16_t a) {
    if (a >= 0x400 && a < 0xc00) {
        unsigned off = a & 0x3ff, third = (off & 0x7f) / 40;
        if (third < 3)
            dirty_text |= 1u << ((off >> 7) + third * 8);
    } else if (a >= 0x2000 && a < 0x6000) {
        unsigned off = a & 0x1fff, third = (off & 0x7f) / 40;
        if (third < 3) {
            unsigned y = ((off >> 10) & 7) | ((off >> 7) & 7) << 3 | third << 6;
            dirty_hgr[y >> 5] |= 1u << (y & 31);
        }
    }
}

void video_cursor(bool on, int row, int col) {
    if (on == cur_on && row == cur_row && col == cur_col)
        return;
    if (cur_row >= 0 && cur_row < 24)
        dirty_text |= 1u << cur_row;
    cur_on = on;
    cur_row = row;
    cur_col = col;
    if (row >= 0 && row < 24)
        dirty_text |= 1u << row;
}

static uint16_t text_row_addr(int r, bool page2) {
    return (uint16_t)((page2 ? 0x800 : 0x400) + (r & 7) * 128 + (r >> 3) * 40);
}

/// One character cell: w dots wide (14 or 7), 16 high, at pixel (x, y).
static void draw_char(uint8_t *fb, int x, int y, uint8_t code, bool cursor, int w) {
    uint8_t g;
    bool inv = false, flash = false;
    if (code < 0x40) {
        inv = true;
        g = code < 0x20 ? (uint8_t)(code + 0x40) : code;
    } else if (code < 0x80) {
        if (vid.altchar) {
            inv = true;
            g = code < 0x60 ? (uint8_t)(code) : (uint8_t)code;  // MouseText shown as letters
        } else {
            flash = true;
            g = code < 0x60 ? code : (uint8_t)(code - 0x40);
        }
    } else {
        g = code < 0xa0 ? (uint8_t)(code - 0x40) : (uint8_t)(code - 0x80);
    }
    if (flash && flash_phase)
        inv = !inv;
    if (cursor)
        inv = !inv;
    const uint8_t *rows = as_font5x7[(g & 0x7f) - 0x20];
    uint8_t fg = inv ? C_BLACK : C_WHITE, bg = inv ? C_WHITE : C_BLACK;
    int dot = w / 7;  // pixels per dot across: 2 in 40 columns, 1 in 80
    for (int r = 0; r < 8; ++r) {
        uint8_t bits = r < 7 ? rows[r] : 0;
        uint8_t *p = fb + (y + 2 * r) * FBW + x;
        for (int c = 0; c < 7; ++c) {
            bool on = c >= 1 && c <= 5 && (bits & (0x10 >> (c - 1)));
            uint8_t col = on ? fg : bg;
            for (int k = 0; k < dot; ++k)
                p[c * dot + k] = col;
        }
        memcpy(p + FBW, p, w);
    }
}

static void draw_text_row(uint8_t *fb, int r, bool page2) {
    uint16_t base = text_row_addr(r, page2);
    int y = Y0 + r * 16;
    if (vid.col80) {
        extern uint8_t *as_aux_text(void);
        const uint8_t *aux = as_aux_text();
        for (int c = 0; c < 80; ++c) {
            uint16_t a = (uint16_t)(base + c / 2);
            uint8_t code = (c & 1) ? M[a] : aux[(a - 0x400) & 0x3ff];
            draw_char(fb, X0 + c * 7, y, code, cur_on && cur_row == r && cur_col == c, 7);
        }
    } else {
        for (int c = 0; c < 40; ++c)
            draw_char(fb, X0 + c * 14, y, M[(uint16_t)(base + c)],
                      cur_on && cur_row == r && cur_col == c, 14);
    }
}

static void draw_lores_row(uint8_t *fb, int r, bool page2) {
    uint16_t base = text_row_addr(r, page2);
    for (int c = 0; c < 40; ++c) {
        uint8_t v = M[(uint16_t)(base + c)];
        for (int half = 0; half < 2; ++half) {
            uint8_t col = lores_rgb[half ? v >> 4 : v & 15];
            uint8_t *p = fb + (Y0 + r * 16 + half * 8) * FBW + X0 + c * 14;
            for (int k = 0; k < 8; ++k)
                memset(p + k * FBW, col, 14);
        }
    }
}

static void draw_hgr_line(uint8_t *fb, int y, bool page2) {
    uint16_t base = (uint16_t)((page2 ? 0x4000 : 0x2000) + ((y & 7) << 10) +
                               (((y >> 3) & 7) << 7) + (y >> 6) * 40);
    uint8_t bits[282], hb[282];
    bits[0] = bits[281] = 0;
    for (int c = 0; c < 40; ++c) {
        uint8_t v = M[(uint16_t)(base + c)];
        for (int b = 0; b < 7; ++b) {
            bits[1 + c * 7 + b] = (v >> b) & 1;
            hb[1 + c * 7 + b] = v >> 7;
        }
    }
    uint8_t *p = fb + (Y0 + y * 2) * FBW + X0;
    for (int x = 0; x < 280; ++x) {
        int i = x + 1;
        uint8_t col;
        if (bits[i]) {
            if (bits[i - 1] || bits[i + 1])
                col = C_WHITE;
            else if (hb[i])
                col = (x & 1) ? C_ORANGE : C_BLUE;
            else
                col = (x & 1) ? C_GREEN : C_VIOLET;
        } else if (bits[i - 1] && bits[i + 1] && x > 0) {
            // between two dots of one colour: the colour shows through
            int n = i - 1;
            int nx = x - 1;
            if (bits[n - 1])
                col = C_BLACK;
            else if (hb[n])
                col = (nx & 1) ? C_ORANGE : C_BLUE;
            else
                col = (nx & 1) ? C_GREEN : C_VIOLET;
        } else {
            col = C_BLACK;
        }
        p[2 * x] = p[2 * x + 1] = col;
    }
    memcpy(p + FBW, p, 560);
}

void video_update(bool force) {
    uint64_t now = asp_micros();
    if (now - flash_us >= 267000) {
        flash_us = now;
        flash_phase = !flash_phase;
        dirty_text = 0xffffff;
    }
    if (!force && now - last_draw_us < 16000)
        return;
    if (!all_dirty && !dirty_text && !(dirty_hgr[0] | dirty_hgr[1] | dirty_hgr[2] |
                                       dirty_hgr[3] | dirty_hgr[4] | dirty_hgr[5]))
        return;
    last_draw_us = now;
    uint8_t *fb = asp_framebuffer();
    if (all_dirty) {
        dirty_text = 0xffffff;
        memset(dirty_hgr, 0xff, sizeof dirty_hgr);
        all_dirty = false;
    }
    bool page2 = vid.page2 && !vid.store80;
    for (int r = 0; r < 24; ++r) {
        bool text_row = vid.text || (vid.mixed && r >= 20);
        if (text_row) {
            if (dirty_text & (1u << r))
                draw_text_row(fb, r, page2);
        } else if (!vid.hires) {
            if (dirty_text & (1u << r))
                draw_lores_row(fb, r, page2);
        } else {
            for (int y = r * 8; y < r * 8 + 8; ++y)
                if (dirty_hgr[y >> 5] & (1u << (y & 31)))
                    draw_hgr_line(fb, y, page2);
        }
    }
    dirty_text = 0;
    memset(dirty_hgr, 0, sizeof dirty_hgr);
}
