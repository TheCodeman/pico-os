/**
 * @file as_gfx.c
 * @brief Low- and high-resolution graphics, writing the Apple's screen memory as the monitor and Applesoft ROMs do.
 *
 * the same bytes for PLOT, HPLOT lines, the colour bytes of HCOLOR and shape
 * tables, so programs that PEEK the screen (or XDRAW to erase) see what they
 * expect. The hi-res drawing state lives in zero page where the ROM keeps
 * it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "as_internal.h"

// ---- Low resolution (the monitor's PLOT, HLINE, VLINE, SCRN) ---------------------

/// GBASCALC: the address of lo-res row row's start.
/// @param row the row (0-47)
/// @return the address
static uint16_t gbascalc(int row) {
    return (uint16_t)(0x400 + (row & 7) * 128 + (row >> 3) * 40);
}

void gfx_set_color(int c) {
    M[Z_COLOR] = (uint8_t)((c & 15) * 17);
}

void gfx_plot(int x, int y) {
    uint16_t a = (uint16_t)(gbascalc(y >> 1) + x);
    uint8_t mask = (y & 1) ? 0xf0 : 0x0f;
    M[Z_MASK] = mask;
    sw16(Z_GBASL, gbascalc(y >> 1));
    M[a] = (uint8_t)(((M[a] ^ M[Z_COLOR]) & mask) ^ M[a]);
    video_mark(a);
}

void gfx_hlin(int x1, int x2, int y) {
    M[Z_H2] = (uint8_t)x2;
    gfx_plot(x1, y);
    for (int x = x1 + 1; x <= x2; ++x)
        gfx_plot(x, y);
}

void gfx_vlin(int y1, int y2, int x) {
    M[Z_V2] = (uint8_t)y2;
    for (int y = y1;; ++y) {
        gfx_plot(x, y);
        if (y >= y2)
            break;
    }
}

int gfx_scrn(int x, int y) {
    uint8_t v = M[(uint16_t)(gbascalc(y >> 1) + x)];
    return (y & 1) ? v >> 4 : v & 15;
}

/// GR: lo-res, mixed, page 1; the top 40 rows cleared; COLOR 0; window rows
/// 20-23, cursor at the bottom.
void gfx_gr(void) {
    as_softswitch(0xc056, false, 0);
    as_softswitch(0xc053, false, 0);
    as_softswitch(0xc050, false, 0);
    as_softswitch(0xc054, false, 0);
    M[Z_COLOR] = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            gfx_plot(x, y);
    M[Z_WNDTOP] = 20;
    M[Z_WNDLFT] = 0;
    M[Z_WNDBTM] = 24;
    extern bool text_80col(void);
    M[Z_WNDWDTH] = text_80col() ? 80 : 40;
    M[Z_CV] = 23;
    as_vtab();
}

// ---- High resolution -----------------------------------------------------------
// Zero page: $1C the colour byte for the current byte column, $26/$27 the
// row's base address, $30 the bit mask (with bit 7), $E5 the byte column,
// $E0-$E2 the last point HPLOT was given.

/// Zero page: the colour byte.
#define HCOLOR1 0x1c

/// HCOLOR's 8 colour bytes.
static const uint8_t hcolors[8] = {0x00, 0x2a, 0x55, 0x7f, 0x80, 0xaa, 0xd5, 0xff};

void gfx_hcolor(int c) {
    M[Z_HCOLOR1] = hcolors[c & 7];
}

/// A hi-res row's base address.
/// @param page the page's high byte ($20 or $40)
/// @param y the row
/// @return the address
static uint16_t hrow(uint8_t page, int y) {
    return (uint16_t)((page << 8) + ((y & 7) << 10) + (((y >> 3) & 7) << 7) + (y >> 6) * 40);
}

/// The row number of a row base address.
/// @param base the address
/// @return the row
static int hrow_y(uint16_t base) {
    uint8_t hi = base >> 8, lo = (uint8_t)base;
    int y = (hi >> 2) & 7;
    y |= (((hi & 3) << 1) | (lo >> 7)) << 3;
    y |= ((lo & 0x7f) / 40) << 6;
    return y;
}

/// The colour byte flips between even and odd byte columns (for the two
/// colours of each group).
static void flip_color(void) {
    uint8_t c = M[HCOLOR1];
    uint8_t a = (uint8_t)(c << 1);
    if ((int8_t)(a - 0xc0) < 0)
        M[HCOLOR1] = c ^ 0x7f;
}

/// HPOSN: sets the position to (x, y).
/// @param x the column
/// @param y the row
static void hposn(int x, int y) {
    M[Z_HGRY] = (uint8_t)y;
    M[Z_HGRX] = (uint8_t)x;
    M[Z_HGRX + 1] = (uint8_t)(x >> 8);
    sw16(Z_GBASL, hrow(M[Z_HPAG], y));
    int col = x / 7, bit = x % 7;
    M[0xe5] = (uint8_t)col;
    M[0x30] = (uint8_t)(0x80 | (1 << bit));
    M[HCOLOR1] = M[Z_HCOLOR1];
    if (col & 1)
        flip_color();
}

/// Plots the point at the position in the colour.
static void plot_here(void) {
    uint16_t a = (uint16_t)(w16(Z_GBASL) + M[0xe5]);
    uint8_t b = M[a];
    M[a] = (uint8_t)(((b ^ M[HCOLOR1]) & M[0x30]) ^ b);
    video_mark(a);
}

/// One pixel left or right, wrapping round the 40 byte columns.
static void move_left(void) {
    uint8_t m = M[0x30];
    uint8_t s = m >> 1;
    if (!(m & 1)) {
        M[0x30] = s ^ 0xc0;
        return;
    }
    M[0xe5] = M[0xe5] == 0 ? 39 : (uint8_t)(M[0xe5] - 1);
    M[0x30] = 0xc0;
    flip_color();
}

/// Moves the position one dot right.
static void move_right(void) {
    uint8_t m = M[0x30];
    uint8_t s = (uint8_t)((m << 1) ^ 0x80);
    if (s & 0x80) {
        M[0x30] = s;
        return;
    }
    M[0xe5] = M[0xe5] >= 39 ? 0 : (uint8_t)(M[0xe5] + 1);
    M[0x30] = 0x81;
    flip_color();
}

/// One row up or down, wrapping round the 192 rows.
static void move_up(void) {
    uint16_t base = w16(Z_GBASL);
    int y = hrow_y(base);
    y = y == 0 ? 191 : y - 1;
    sw16(Z_GBASL, hrow(base >> 8 & 0xe0, y));
}

/// Moves the position one row down, wrapping to the top.
static void move_down(void) {
    uint16_t base = w16(Z_GBASL);
    int y = hrow_y(base);
    uint8_t page = base >> 8 & 0xe0;
    if (y == 191) {
        y = 0;
        page = M[Z_HPAG];
    } else {
        y++;
    }
    sw16(Z_GBASL, hrow(page, y));
}

void gfx_hplot(int x, int y) {
    hposn(x, y);
    plot_here();
}

void gfx_hposn(int x, int y) {
    hposn(x, y);
}

// HGLIN: a line from the last point to (x, y), one pixel step at a time
// (across or down, never both), as the ROM draws them.
void gfx_hplot_to(int x, int y) {
    int x0 = M[Z_HGRX] | M[Z_HGRX + 1] << 8;
    int y0 = M[Z_HGRY];
    int dx = x - x0;
    bool left = dx < 0;
    if (left)
        dx = -dx;
    int dy = y - y0;
    bool down = dy > 0;
    if (dy < 0)
        dy = -dy;
    M[Z_HGRX] = (uint8_t)x;
    M[Z_HGRX + 1] = (uint8_t)(x >> 8);
    M[Z_HGRY] = (uint8_t)y;
    int err = dx - dy;
    int steps = dx + dy + 1;
    bool across = err >= 0;
    plot_here();
    while (--steps) {
        if (across) {
            if (left)
                move_left();
            else
                move_right();
            err -= dy;
        } else {
            if (down)
                move_down();
            else
                move_up();
            err += dx;
        }
        across = err >= 0;
        plot_here();
    }
}

// HGR and HGR2: hi-res on, page cleared to black.
void gfx_hgr(int page) {
    if (page == 2) {
        as_softswitch(0xc055, false, 0);
        as_softswitch(0xc052, false, 0);
        M[Z_HPAG] = 0x40;
    } else {
        as_softswitch(0xc054, false, 0);
        as_softswitch(0xc053, false, 0);
        M[Z_HPAG] = 0x20;
    }
    as_softswitch(0xc057, false, 0);
    as_softswitch(0xc050, false, 0);
    M[HCOLOR1] = 0;
    extern void gfx_hclear(void);
    gfx_hclear();
}

/// HCLR/BKGND: fills the page with the colour byte, alternating it for the
/// odd byte columns.
void gfx_hclear(void) {
    uint16_t base = (uint16_t)(M[Z_HPAG] << 8);
    for (int i = 0; i < 0x2000; ++i) {
        M[(uint16_t)(base + i)] = M[HCOLOR1];
        flip_color();
    }
    video_mark_all();
}

// DRAW and XDRAW: shape `shape` from the table at $E8, scaled by SCALE and
// rotated by ROT (64ths of a turn), from the current position or (x, y).
void gfx_draw(int shape, bool xdraw, bool at, int x, int y) {
    uint16_t table = w16(Z_HSHAPE);
    if (shape > M[table])
        as_error(ERR_ILLEGAL_QUANTITY);
    uint16_t addr = (uint16_t)(table + w16((uint16_t)(table + 2 * shape)));
    if (at)
        hposn(x, y);
    uint8_t rot = M[Z_ROT];
    static const uint8_t steps[17] = {0xff, 0xfe, 0xfa, 0xf4, 0xec, 0xe1, 0xd4, 0xc5,
                                      0xb4, 0xa1, 0x8d, 0x78, 0x61, 0x49, 0x31, 0x18, 0xff};
    uint8_t quad = rot >> 4;
    uint8_t frac = rot & 15;
    uint8_t d0 = steps[frac];                         // the "cosine" step
    uint8_t d2 = (uint8_t)(steps[(frac ^ 15) + 1] + 1);  // and the "sine"
    M[0xea] = 0;  // collisions
    uint8_t scale = M[Z_SCALE];
    uint8_t vec = M[addr];
    for (;;) {
        uint8_t acc1 = 0x80, acc2 = 0x80;
        int n = scale ? scale : 256;
        for (int i = 0; i < n; ++i) {
            for (int part = 0; part < 2; ++part) {
                int sum = part == 0 ? acc1 + d0 + 1 : acc2 + d2;
                if (part == 0)
                    acc1 = (uint8_t)sum;
                else
                    acc2 = (uint8_t)sum;
                if (sum < 256)
                    continue;
                // Plot (if this vector plots), then move.
                if (vec & 4) {
                    uint16_t a = (uint16_t)(w16(Z_GBASL) + M[0xe5]);
                    uint8_t b = M[a];
                    if (xdraw) {
                        uint8_t bit = 0x7f & M[0x30];
                        if (!(bit & b))
                            M[0xea]++;
                        M[a] = b ^ bit;
                    } else {
                        uint8_t d = (uint8_t)((b ^ M[HCOLOR1]) & M[0x30]);
                        if (!d)
                            M[0xea]++;
                        M[a] = d ^ b;
                    }
                    video_mark(a);
                }
                int dir = (vec + quad + part) & 3;
                switch (dir) {
                case 0: move_up(); break;
                case 1: move_right(); break;
                case 2: move_down(); break;
                case 3: move_left(); break;
                }
            }
        }
        vec >>= 3;
        if (vec)
            continue;
        addr++;
        vec = M[addr];
        if (!vec)
            break;
    }
}
