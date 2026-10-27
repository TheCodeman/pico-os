/**
 * @file a2e_video.c
 * @brief The Apple IIe's display drawn into an RGB332 framebuffer.
 *
 * 40- and 80-column text (normal, inverse, flashing, the alternate set),
 * lo-res and double lo-res, hi-res with NTSC-style colour, double hi-res in
 * 16 colours, mixed mode and page 2. The picture is 560 dots across and 192
 * lines, each line drawn once (640x240, for a scanout that doubles lines) or
 * twice (640x480). The font and colours are the Applesoft app's
 * (as_video.c); MouseText is in a2e_mousetext.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "a2e.h"
#include "a2e_mousetext.h"
#include "as_font.h"

/// The left margin: 560 dots centred in 640.
#define X0  40
/// The framebuffer's width.
#define FBW A2E_FB_WIDTH

/// Where drawing goes: the framebuffer, the first picture line's row, and
/// framebuffer rows per Apple line (1 or 2).
typedef struct {
    uint8_t *fb;  ///< the framebuffer
    /// the first row, and lines per Apple line (1 or 2)
    ///@{
    int y0, ys;
    ///@}
} target;

/// A colour as RGB332.
#define RGB(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

/// The 16 lo-res colours.
static const uint8_t lores_rgb[16] = {
    RGB(0, 0, 0), RGB(4, 0, 1), RGB(2, 1, 2), RGB(6, 2, 3), RGB(0, 3, 1), RGB(4, 4, 2),
    RGB(1, 4, 3), RGB(5, 5, 3), RGB(2, 2, 0), RGB(6, 3, 0), RGB(4, 4, 2), RGB(7, 4, 2),
    RGB(1, 5, 0), RGB(5, 6, 1), RGB(3, 7, 2), RGB(7, 7, 3),
};
/// Black.
#define C_BLACK  RGB(0, 0, 0)
/// White.
#define C_WHITE  RGB(7, 7, 3)
/// Hi-res violet.
#define C_VIOLET RGB(6, 2, 3)
/// Hi-res green.
#define C_GREEN  RGB(1, 5, 0)
/// Hi-res blue.
#define C_BLUE   RGB(1, 4, 3)
/// Hi-res orange.
#define C_ORANGE RGB(6, 3, 0)

/// A text (or lo-res) row's address.
/// @param r the row
/// @param page2 page 2
/// @return the address
static uint16_t text_row_addr(int r, bool page2) {
    return (uint16_t)((page2 ? 0x800 : 0x400) + (r & 7) * 128 + (r >> 3) * 40);
}

/// A hi-res line's address.
/// @param y the line
/// @param page2 page 2
/// @return the address
static uint16_t hgr_line_addr(int y, bool page2) {
    return (uint16_t)((page2 ? 0x4000 : 0x2000) + ((y & 7) << 10) + (((y >> 3) & 7) << 7) +
                      (y >> 6) * 40);
}

/// MouseText: with the alternate set, $40-$5F are its graphics, shown as
/// they are (not inverse).
/// @param code the screen code
/// @param altchar the alternate set is on
/// @return true for MouseText
static bool is_mousetext(uint8_t code, bool altchar) {
    return altchar && code >= 0x40 && code < 0x60;
}

/// A screen code as the character generator shows it: the glyph ($20-$7F) and
/// whether it is inverse. Flashing characters are inverse half the time.
/// @param code the screen code
/// @param altchar the alternate set
/// @param flash_on flashing characters are inverse now
/// @param inv set if it is inverse
/// @return the glyph
static uint8_t glyph(uint8_t code, bool altchar, bool flash_on, bool *inv) {
    *inv = false;
    if (code < 0x40) {
        *inv = true;
        return code < 0x20 ? (uint8_t)(code + 0x40) : code;
    }
    if (code < 0x80) {
        if (altchar) {
            // $40-$5F is MouseText (see is_mousetext); $60-$7F inverse
            // lower case.
            *inv = code >= 0x60;
            return code;
        }
        *inv = flash_on;
        return code < 0x60 ? code : (uint8_t)(code - 0x40);
    }
    return code < 0xa0 ? (uint8_t)(code - 0x40) : (uint8_t)(code - 0x80);
}

/// One character cell: w dots wide (14 in 40 columns, 7 in 80), 8 lines high,
/// at framebuffer row y.
/// @param t where to draw
/// @param x the left
/// @param y the top row
/// @param code the screen code
/// @param altchar the alternate set
/// @param flash_on the flash phase
/// @param w the cell's width
static void draw_char(const target *t, int x, int y, uint8_t code, bool altchar, bool flash_on,
                      int w) {
    bool inv;
    uint8_t g = glyph(code, altchar, flash_on, &inv);
    bool mouse = is_mousetext(code, altchar);
    const uint8_t *rows = mouse ? a2e_mousetext[code - 0x40] : as_font5x7[(g & 0x7f) - 0x20];
    uint8_t fg = inv ? C_BLACK : C_WHITE, bg = inv ? C_WHITE : C_BLACK;
    int dot = w / 7;
    for (int r = 0; r < 8; ++r) {
        // The font's 5 dots sit in the middle of the 7; MouseText uses all 7.
        uint8_t bits = mouse ? rows[r] : r < 7 ? (uint8_t)(rows[r] << 1) : 0;
        uint8_t *p = t->fb + (y + t->ys * r) * FBW + x;
        for (int c = 0; c < 7; ++c) {
            bool on = bits & (0x40 >> c);
            uint8_t col = on ? fg : bg;
            for (int k = 0; k < dot; ++k)
                p[c * dot + k] = col;
        }
        if (t->ys == 2)
            memcpy(p + FBW, p, (size_t)w);
    }
}

/// Draws a row of 40 or 80-column text.
/// @param m the machine
/// @param t where
/// @param r the row
/// @param page2 page 2
/// @param flash_on the flash phase
static void draw_text_row(const a2e *m, const target *t, int r, bool page2, bool flash_on) {
    uint16_t base = text_row_addr(r, page2);
    int y = t->y0 + r * 8 * t->ys;
    if (m->col80) {
        for (int c = 0; c < 80; ++c) {
            uint16_t a = (uint16_t)(base + c / 2);
            uint8_t code = (c & 1) ? m->main[a] : m->aux[a];
            draw_char(t, X0 + c * 7, y, code, m->altchar, flash_on, 7);
        }
    } else {
        for (int c = 0; c < 40; ++c)
            draw_char(t, X0 + c * 14, y, m->main[base + c], m->altchar, flash_on, 14);
    }
}

/// A block w dots wide and h Apple lines high from framebuffer row y.
/// @param t where
/// @param x the left
/// @param y the top row
/// @param w the width
/// @param h the height
/// @param col the colour
static void fill_block(const target *t, int x, int y, int w, int h, uint8_t col) {
    for (int k = 0; k < h * t->ys; ++k)
        memset(t->fb + (y + k) * FBW + x, col, (size_t)w);
}

/// Draws a row of lo-res (or double lo-res) blocks.
/// @param m the machine
/// @param t where
/// @param r the row
/// @param page2 page 2
/// @param dbl double lo-res
static void draw_lores_row(const a2e *m, const target *t, int r, bool page2, bool dbl) {
    uint16_t base = text_row_addr(r, page2);
    int y = t->y0 + r * 8 * t->ys, half_y = 4 * t->ys;
    for (int c = 0; c < 40; ++c) {
        uint8_t v = m->main[base + c];
        if (dbl) {
            // Double lo-res: aux then main, 7 dots each. The aux colours are
            // rotated one bit, as the hardware shows them.
            uint8_t a = m->aux[base + c];
            for (int half = 0; half < 2; ++half) {
                uint8_t n = half ? a >> 4 : a & 15;
                n = (uint8_t)(((n << 1) | (n >> 3)) & 15);
                fill_block(t, X0 + c * 14, y + half * half_y, 7, 4, lores_rgb[n]);
                fill_block(t, X0 + c * 14 + 7, y + half * half_y, 7, 4,
                           lores_rgb[half ? v >> 4 : v & 15]);
            }
        } else {
            fill_block(t, X0 + c * 14, y, 14, 4, lores_rgb[v & 15]);
            fill_block(t, X0 + c * 14, y + half_y, 14, 4, lores_rgb[v >> 4]);
        }
    }
}

/// Draws a hi-res line with its NTSC colours.
/// @param m the machine
/// @param t where
/// @param y the line
/// @param page2 page 2
static void draw_hgr_line(const a2e *m, const target *t, int y, bool page2) {
    uint16_t base = hgr_line_addr(y, page2);
    uint8_t bits[282], hb[282];
    bits[0] = bits[281] = 0;
    hb[0] = hb[281] = 0;
    for (int c = 0; c < 40; ++c) {
        uint8_t v = m->main[base + c];
        for (int b = 0; b < 7; ++b) {
            bits[1 + c * 7 + b] = (v >> b) & 1;
            hb[1 + c * 7 + b] = v >> 7;
        }
    }
    uint8_t *p = t->fb + (t->y0 + y * t->ys) * FBW + X0;
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
            int n = i - 1, nx = x - 1;
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
    if (t->ys == 2)
        memcpy(p + FBW, p, 560);
}

/// Draws a double hi-res line (16 colours, 140 across).
/// @param m the machine
/// @param t where
/// @param y the line
/// @param page2 page 2
static void draw_dhgr_line(const a2e *m, const target *t, int y, bool page2) {
    // 560 dots: aux byte then main byte for each column, 7 dots each, low bit
    // first. Every 4 dots make one of 16 colours (140 across).
    uint16_t base = hgr_line_addr(y, page2);
    uint8_t dots[560];
    for (int c = 0; c < 40; ++c) {
        uint8_t a = m->aux[base + c], v = m->main[base + c];
        for (int b = 0; b < 7; ++b) {
            dots[c * 14 + b] = (a >> b) & 1;
            dots[c * 14 + 7 + b] = (v >> b) & 1;
        }
    }
    uint8_t *p = t->fb + (t->y0 + y * t->ys) * FBW + X0;
    for (int x = 0; x < 560; x += 4) {
        uint8_t n = (uint8_t)(dots[x] | dots[x + 1] << 1 | dots[x + 2] << 2 | dots[x + 3] << 3);
        memset(p + x, lores_rgb[n], 4);
    }
    if (t->ys == 2)
        memcpy(p + FBW, p, 560);
}

/// What one text row (8 Apple lines) shows.
enum { ROW_TEXT, ROW_LORES, ROW_HIRES };

/// What a row shows: text, lo-res or hi-res.
/// @param m the machine
/// @param r the row
/// @return a ROW_ kind
static int row_kind(const a2e *m, int r) {
    if (m->text || (m->mixed && r >= 20))
        return ROW_TEXT;
    return m->hires ? ROW_HIRES : ROW_LORES;
}

/// Whether flashing characters are inverse now.
/// @param m the machine
/// @return the flash phase
static bool flash_phase(const a2e *m) {
    return (m->cycles / (A2E_FRAME_CYCLES * 16)) & 1;
}

/// Draws a row (its lines in line_mask, for hi-res).
/// @param m the machine
/// @param t where
/// @param r the row
/// @param kind a ROW_ kind
/// @param page2 page 2
/// @param flash_on the flash phase
/// @param line_mask the lines to draw
static void draw_row(const a2e *m, const target *t, int r, int kind, bool page2, bool flash_on,
                     uint8_t line_mask) {
    bool dbl = m->col80 && m->dhires;
    if (kind == ROW_TEXT)
        draw_text_row(m, t, r, page2, flash_on);
    else if (kind == ROW_LORES)
        draw_lores_row(m, t, r, page2, dbl);
    else
        for (int k = 0; k < 8; ++k)
            if (line_mask & (1u << k)) {
                if (dbl)
                    draw_dhgr_line(m, t, r * 8 + k, page2);
                else
                    draw_hgr_line(m, t, r * 8 + k, page2);
            }
}

void a2e_video_draw(const a2e *m, uint8_t *fb) {
    target t = {fb, (A2E_FB_HEIGHT - 384) / 2, 2};
    bool page2 = m->page2 && !m->store80;
    bool flash_on = flash_phase(m);
    for (int r = 0; r < 24; ++r)
        draw_row(m, &t, r, row_kind(m, r), page2, flash_on, 0xff);
}

// ---- Drawing only what changed ---------------------------------------------------

/// A hash of 40 bytes of screen memory (always 8-byte aligned).
/// @param p the bytes
/// @param h the hash so far
/// @return the hash
static uint32_t hash40(const uint8_t *p, uint32_t h) {
    const uint32_t *w = (const uint32_t *)p;
    for (int i = 0; i < 10; ++i)
        h = (h ^ w[i]) * 0x01000193u;
    return h;
}

uint32_t a2e_video_update(const a2e *m, a2e_screen *s) {
    target t = {s->fb, s->y0, s->line_double ? 1 : 2};
    bool page2 = m->page2 && !m->store80;
    bool flash_on = flash_phase(m);
    bool aux = m->col80;  // 80 columns, double lo-res and double hi-res show aux too
    uint32_t mode = (uint32_t)page2 | (uint32_t)aux << 1 | (uint32_t)m->dhires << 2 |
                    (uint32_t)m->altchar << 3 | 0x80000000u;
    uint32_t drawn = 0;
    for (int r = 0; r < 24; ++r) {
        int kind = row_kind(m, r);
        uint32_t base_key = (mode | (uint32_t)kind << 4) * 0x9e3779b1u;
        if (kind == ROW_HIRES) {
            uint8_t mask = 0;
            for (int k = 0; k < 8; ++k) {
                int y = r * 8 + k;
                uint16_t a = hgr_line_addr(y, page2);
                uint32_t h = hash40(m->main + a, base_key);
                if (aux)
                    h = hash40(m->aux + a, h);
                if (h != s->key[y]) {
                    s->key[y] = h;
                    mask |= (uint8_t)(1u << k);
                }
            }
            if (mask) {
                draw_row(m, &t, r, kind, page2, flash_on, mask);
                drawn += (uint32_t)__builtin_popcount(mask);
            }
        } else {
            uint16_t a = text_row_addr(r, page2);
            uint32_t h = hash40(m->main + a, base_key ^ (kind == ROW_TEXT && flash_on));
            if (aux)
                h = hash40(m->aux + a, h);
            if (h != s->key[r * 8]) {
                for (int k = 0; k < 8; ++k)
                    s->key[r * 8 + k] = k ? 0 : h;
                draw_row(m, &t, r, kind, page2, flash_on, 0xff);
                drawn += 8;
            }
        }
    }
    return drawn;
}

void a2e_video_invalidate(a2e_screen *s) {
    memset(s->key, 0, sizeof s->key);
}

void a2e_text_dump(const a2e *m, char *buf) {
    bool page2 = m->page2 && !m->store80;
    char *o = buf;
    for (int r = 0; r < 24; ++r) {
        char *line = o;
        uint16_t base = text_row_addr(r, page2);
        int cols = m->col80 ? 80 : 40;
        for (int c = 0; c < cols; ++c) {
            uint8_t code;
            if (m->col80)
                code = (c & 1) ? m->main[base + c / 2] : m->aux[base + c / 2];
            else
                code = m->main[base + c];
            bool inv;
            uint8_t g = glyph(code, m->altchar, false, &inv);
            *o++ = (char)(g >= 0x20 && g < 0x7f ? g : ' ');
        }
        while (o > line && o[-1] == ' ')  // trailing blanks go
            --o;
        *o++ = '\n';
    }
    *o = 0;
}
