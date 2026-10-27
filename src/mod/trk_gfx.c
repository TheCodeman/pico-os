/**
 * @file trk_gfx.c
 * @brief The tracker's drawing: rectangles, bevels and 8x16 text straight into the framebuffer.
 *
 * Everything is drawn opaque, so drawing again over what is there never
 * flickers.
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

#include "dvi.h"
#include "font8x16.h"
#include "trk.h"

/// Clips a rectangle to the screen.
/// @param x its left
/// @param y its top
/// @param w its width
/// @param h its height
/// @return any of it is left
static inline bool clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > DVI_WIDTH) *w = DVI_WIDTH - *x;
    if (*y + *h > DVI_HEIGHT) *h = DVI_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

uint8_t *gfx_target = dvi_framebuf;

void gfx_fill(int x, int y, int w, int h, uint8_t c) {
    if (!clip(&x, &y, &w, &h))
        return;
    for (int r = 0; r < h; ++r)
        memset(gfx_target + (y + r) * DVI_WIDTH + x, c, w);
}

void gfx_hline(int x, int y, int w, uint8_t c) {
    gfx_fill(x, y, w, 1, c);
}

void gfx_vline(int x, int y, int h, uint8_t c) {
    gfx_fill(x, y, 1, h, c);
}

void gfx_char(int x, int y, uint8_t ch, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x > DVI_WIDTH - 8 || y > DVI_HEIGHT - 16)
        return;
    const uint8_t *g = font8x16[ch];
    uint8_t *d = gfx_target + y * DVI_WIDTH + x;
    for (int r = 0; r < 16; ++r, d += DVI_WIDTH) {
        uint8_t bits = g[r];
        d[0] = bits & 0x80 ? fg : bg;
        d[1] = bits & 0x40 ? fg : bg;
        d[2] = bits & 0x20 ? fg : bg;
        d[3] = bits & 0x10 ? fg : bg;
        d[4] = bits & 0x08 ? fg : bg;
        d[5] = bits & 0x04 ? fg : bg;
        d[6] = bits & 0x02 ? fg : bg;
        d[7] = bits & 0x01 ? fg : bg;
    }
}

void gfx_char14(int x, int y, uint8_t ch, uint8_t fg, uint8_t bg) {
    // The glyph without its top and bottom rows (blank for capitals and
    // digits): 14 pixels high, for the pattern's rows.
    if (x < 0 || y < 0 || x > DVI_WIDTH - 8 || y > DVI_HEIGHT - 14)
        return;
    const uint8_t *g = font8x16[ch] + 1;
    uint8_t *d = gfx_target + y * DVI_WIDTH + x;
    for (int r = 0; r < 14; ++r, d += DVI_WIDTH) {
        uint8_t bits = g[r];
        for (int i = 0; i < 8; ++i)
            d[i] = bits & (0x80 >> i) ? fg : bg;
    }
}

int gfx_text14(int x, int y, const char *s, int n, uint8_t fg, uint8_t bg) {
    for (int i = 0; i < n; ++i, x += 8)
        gfx_char14(x, y, *s ? (uint8_t)*s++ : ' ', fg, bg);
    return x;
}

int gfx_text(int x, int y, const char *s, int n, uint8_t fg, uint8_t bg) {
    if (n < 0)
        n = strlen(s);
    for (int i = 0; i < n; ++i, x += 8)
        gfx_char(x, y, *s ? (uint8_t)*s++ : ' ', fg, bg);
    return x;
}

int gfx_printf(int x, int y, int n, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, n, fg, bg);
}

void gfx_bevel_edges(int x, int y, int w, int h, bool raised) {
    uint8_t a = raised ? C_LIGHT : C_DARK, b = raised ? C_DARK : C_LIGHT;
    gfx_hline(x, y, w, a);
    gfx_vline(x, y, h, a);
    gfx_hline(x, y + h - 1, w, b);
    gfx_vline(x + w - 1, y, h, b);
}

void gfx_bevel(int x, int y, int w, int h, bool raised, uint8_t face) {
    gfx_fill(x + 1, y + 1, w - 2, h - 2, face);
    gfx_bevel_edges(x, y, w, h, raised);
}

// ---- Places to click ------------------------------------------------------------------------------

void hit_clear(void) {
    T->nhits = 0;
}

void hit_add(int x, int y, int w, int h, int id, int arg) {
    // The same place again (drawn each frame) replaces the old one.
    for (int i = 0; i < T->nhits; ++i) {
        hit_t *t = &T->hits[i];
        if (t->x == x && t->y == y && t->w == w && t->h == h) {
            t->id = id;
            t->arg = arg;
            return;
        }
    }
    if (T->nhits < (int)(sizeof T->hits / sizeof T->hits[0]))
        T->hits[T->nhits++] = (hit_t){x, y, w, h, id, arg};
}

const hit_t *hit_find(int x, int y) {
    // The last added wins (drawn on top).
    for (int i = T->nhits - 1; i >= 0; --i) {
        const hit_t *t = &T->hits[i];
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h)
            return t;
    }
    return NULL;
}

void gfx_button(int x, int y, int w, int h, const char *label, int id, int arg, bool lit) {
    uint8_t face = lit ? C_BLUE : C_FACE;
    if (T->full)
        gfx_bevel(x, y, w, h, !lit, face);
    else
        gfx_bevel_edges(x, y, w, h, !lit);
    int n = strlen(label);
    if (n > (w - 4) / 8)
        n = (w - 4) / 8;
    // The label's row, filled across so a changed label leaves nothing behind.
    int tx = x + (w - n * 8) / 2, ty = y + (h - 16) / 2;
    gfx_fill(x + 1, ty, tx - x - 1, 16, face);
    int end = gfx_text(tx, ty, label, n, lit ? C_YELLOW : C_TEXT, face);
    gfx_fill(end, ty, x + w - 1 - end, 16, face);
    hit_add(x, y, w, h, id, arg);
}
