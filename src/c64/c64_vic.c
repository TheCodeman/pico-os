/**
 * @file c64_vic.c
 * @brief Implementation of c64_vic.h: the PAL VIC-II (6569), drawn a line at a time (in pieces where registers change part way along it).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_vic.h"

#include <string.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#define TABLE   __attribute__((section("app_ov_c64.tables")))  ///< in SRAM with the code
#else
/// PC builds: the hot functions stay where they are.
#define FAST(f) f
/// PC builds: the tables stay in flash.
#define TABLE   const
#endif

const uint32_t c64_palette[16] = {
    0x000000, 0xffffff, 0x68372b, 0x70a4b2, 0x6f3d86, 0x588d43, 0x352879, 0xb8c76f,
    0x6f4f25, 0x433900, 0x9a6759, 0x444444, 0x6c6c6c, 0x9ad284, 0x6c5eb5, 0x959595,
};

/// Line buffer columns: x = sprite X coordinate - C64_FIRST_X. The 320
/// pixels start at 32 (plus the scroll); the 40-column window is 32-351, the
/// 38-column one 39-342.
#define WIN_X 32

static void draw_to(c64_vic *v, int x);

/// Sets the IRQ line from the flags and the enables.
/// @param v the VIC-II
static void update_irq(c64_vic *v) {
    v->irq = (v->irq_flags & v->reg[0x1a] & 0x0f) != 0;
}

/// The raster line the interrupt compares with.
/// @param v the VIC-II
/// @return the line
static uint16_t raster_cmp(const c64_vic *v) {
    return v->reg[0x12] | (uint16_t)(v->reg[0x11] & 0x80) << 1;
}

void c64_vic_reset(c64_vic *v) {
    const uint8_t *ram = v->ram, *charrom = v->charrom, *color = v->color;
    uint8_t *frame = v->frame;
    void (*line_out)(void *, int, const uint8_t *) = v->line_out;
    void *out_ctx = v->out_ctx;
    uint16_t bank = v->bank, from = v->draw_from, to = v->draw_to;
    memset(v, 0, sizeof *v);
    v->line_out = line_out;
    v->out_ctx = out_ctx;
    v->draw_from = to ? from : 0;  // kept, once set
    v->draw_to = to ? to : C64_SCREEN_H;
    v->ram = ram;
    v->charrom = charrom;
    v->color = color;
    v->frame = frame;
    v->bank = bank;
    v->vborder = true;
}

uint8_t c64_vic_read(c64_vic *v, uint8_t reg) {
    reg &= 0x3f;
    switch (reg) {
    case 0x11: return (v->reg[0x11] & 0x7f) | (uint8_t)((v->raster & 0x100) >> 1);
    case 0x12: return v->raster & 0xff;
    case 0x13: case 0x14: return 0;  // light pen
    case 0x16: return v->reg[0x16] | 0xc0;
    case 0x18: return v->reg[0x18] | 0x01;
    case 0x19: return v->irq_flags | 0x70 | (v->irq ? 0x80 : 0);
    case 0x1a: return v->reg[0x1a] | 0xf0;
    case 0x1e: {
        uint8_t c = v->ss_coll;
        v->ss_coll = 0;
        return c;
    }
    case 0x1f: {
        uint8_t c = v->sb_coll;
        v->sb_coll = 0;
        return c;
    }
    default:
        if (reg >= 0x2f)
            return 0xff;
        if (reg >= 0x20)
            return v->reg[reg] | 0xf0;
        return v->reg[reg];
    }
}

void c64_vic_write(c64_vic *v, uint8_t reg, uint8_t value) {
    reg &= 0x3f;
    // A register that changes the picture: what is drawn so far keeps the
    // old value.
    if (v->line_on && reg != 0x12 && reg != 0x19 && reg != 0x1a && reg < 0x2f && reg != 0x1e && reg != 0x1f &&
        v->reg[reg] != value)
        draw_to(v, v->write_x);
    switch (reg) {
    case 0x11:
    case 0x12: {
        uint16_t was = raster_cmp(v);
        v->reg[reg] = value;
        if (reg == 0x11 && v->raster == 0x30 && (value & 0x10))
            v->den_frame = true;
        // Setting the compare to the line being drawn interrupts at once.
        if (raster_cmp(v) != was && raster_cmp(v) == v->raster) {
            v->irq_flags |= 1;
            update_irq(v);
        }
        break;
    }
    case 0x19:
        v->irq_flags &= ~value & 0x0f;
        update_irq(v);
        break;
    case 0x1a:
        v->reg[0x1a] = value & 0x0f;
        update_irq(v);
        break;
    case 0x1e: case 0x1f:
        break;
    default:
        if (reg < 0x2f)
            v->reg[reg] = value;
        break;
    }
}

void c64_vic_line_start(c64_vic *v) {
    if (v->raster == 0)
        v->vcbase = 0;
    if (v->raster == 0x30)
        v->den_frame = v->reg[0x11] & 0x10;
    if (v->raster == raster_cmp(v)) {
        v->irq_flags |= 1;
        update_irq(v);
    }
}

/// A byte as the VIC-II sees it: the character ROM in banks 0 and 2, else RAM.
/// @param v the VIC-II
/// @param a the address in its bank
/// @return the byte
static inline uint8_t vread(const c64_vic *v, uint16_t a) {
    a &= 0x3fff;
    if (!(v->bank & 0x4000) && (a & 0x3000) == 0x1000)
        return v->charrom[a & 0x0fff];
    return v->ram[v->bank | a];
}

/// Draws the line's sprites over the graphics, with their priorities and collisions.
/// @param v the VIC-II
/// @param pix the line
/// @param fg which pixels are foreground
static void FAST(draw_sprites)(c64_vic *v, uint8_t *pix, const uint8_t *fg) {
    uint8_t mask[C64_SCREEN_W], col[C64_SCREEN_W];
    bool any = false;
    uint16_t vm = (uint16_t)(v->reg[0x18] & 0xf0) << 6;
    for (int n = 7; n >= 0; --n) {
        if (!v->sp_active[n])
            continue;
        if (!any) {
            memset(mask, 0, sizeof mask);
            any = true;
        }
        uint16_t ptr = (uint16_t)vread(v, vm | 0x3f8 | n) << 6;
        uint32_t bits = (uint32_t)vread(v, ptr + v->sp_mcbase[n]) << 16 |
                        (uint32_t)vread(v, ptr + v->sp_mcbase[n] + 1) << 8 |
                        vread(v, ptr + v->sp_mcbase[n] + 2);
        int x = v->reg[2 * n] | ((v->reg[0x10] >> n) & 1) << 8;
        if (x >= 0x1f8)
            x -= 0x1f8;
        x -= C64_FIRST_X;
        bool wide = (v->reg[0x1d] >> n) & 1;
        bool multi = (v->reg[0x1c] >> n) & 1;
        uint8_t colors[4] = {0, v->reg[0x25] & 15, v->reg[0x27 + n] & 15, v->reg[0x26] & 15};
        int w = wide ? 2 : 1;
        for (int i = 0; i < 24; ++i) {
            unsigned c;
            if (multi)
                c = (bits >> (22 - (i & ~1))) & 3;
            else
                c = (bits >> (23 - i)) & 1 ? 2 : 0;
            if (!c)
                continue;
            for (int k = 0; k < w; ++k) {
                int px = x + i * w + k;
                if (px < 0 || px >= C64_SCREEN_W)
                    continue;
                mask[px] |= 1u << n;
                col[px] = colors[c];
            }
        }
    }
    if (!any)
        return;
    uint8_t ss = 0, sb = 0;
    for (int x = 0; x < C64_SCREEN_W; ++x) {
        uint8_t m = mask[x];
        if (!m)
            continue;
        if (m & (m - 1))
            ss |= m;
        if (fg[x])
            sb |= m;
        unsigned n = (unsigned)__builtin_ctz(m);  // the lowest-numbered sprite is in front
        if (!(fg[x] && ((v->reg[0x1b] >> n) & 1)))
            pix[x] = col[x];
    }
    if (ss) {
        if (!v->ss_coll)
            v->irq_flags |= 4;
        v->ss_coll |= ss;
    }
    if (sb) {
        if (!v->sb_coll)
            v->irq_flags |= 2;
        v->sb_coll |= sb;
    }
    update_irq(v);
}

/// Each 4 bits of graphics as a mask of 4 pixels (the first pixel is the top
/// bit and the lowest byte).
static TABLE uint32_t nibble_mask[16] = {
    0x00000000, 0xff000000, 0x00ff0000, 0xffff0000, 0x0000ff00, 0xff00ff00, 0x00ffff00, 0xffffff00,
    0x000000ff, 0xff0000ff, 0x00ff00ff, 0xffff00ff, 0x0000ffff, 0xff00ffff, 0x00ffffff, 0xffffffff,
};

/// Stores 4 pixels at once.
/// @param p where
/// @param w the pixels
static inline void put4(uint8_t *p, uint32_t w) {
    memcpy(p, &w, 4);
}

/// The 320 pixels of graphics (and the scroll's gap before them) into pix,
/// and into fg (if not NULL) which of them are foreground, for sprites.
/// @param v the VIC-II
/// @param pix the line
/// @param fg where to mark foreground pixels, or NULL
static void FAST(draw_graphics)(c64_vic *v, uint8_t *pix, uint8_t *fg) {
    uint8_t d011 = v->reg[0x11], d016 = v->reg[0x16], d018 = v->reg[0x18];
    uint8_t bg[4] = {v->reg[0x21] & 15, v->reg[0x22] & 15, v->reg[0x23] & 15, v->reg[0x24] & 15};
    bool ecm = d011 & 0x40, bmm = d011 & 0x20, mcm = d016 & 0x10;
    bool invalid = ecm && (bmm || mcm);
    int x0 = WIN_X + (d016 & 7);
    memset(pix + WIN_X, bg[0], (size_t)(x0 - WIN_X));
    if (fg)
        memset(fg, 0, C64_SCREEN_W);
    uint16_t chars = (uint16_t)(d018 & 0x0e) << 10, bitmap = (uint16_t)(d018 & 0x08) << 10;
    uint8_t idle = v->display ? 0 : vread(v, ecm ? 0x39ff : 0x3fff);
    for (int i = 0; i < 40; ++i) {
        uint8_t *p = pix + x0 + i * 8;
        uint8_t g, c = v->matrix[i], col = v->colors[i];
        uint32_t c1, c0;  // hi-res colours for 1 and 0 bits
        uint8_t mc[4];
        bool multi = false;
        if (!v->display) {
            g = idle;
            c1 = 0;
            c0 = bg[0];
        } else if (!bmm) {
            g = vread(v, chars | (uint16_t)(ecm ? c & 0x3f : c) << 3 | v->rc);
            c1 = col;
            c0 = ecm ? bg[c >> 6] : bg[0];
            if (mcm && (col & 8)) {
                multi = true;
                mc[0] = bg[0], mc[1] = bg[1], mc[2] = bg[2], mc[3] = col & 7;
            } else if (mcm) {
                c1 = col & 7;
            }
        } else {
            g = vread(v, bitmap | (uint16_t)((v->vc + i) & 0x3ff) << 3 | v->rc);
            c1 = c >> 4;
            c0 = c & 15;
            if (mcm) {
                multi = true;
                mc[0] = bg[0], mc[1] = c >> 4, mc[2] = c & 15, mc[3] = col;
            }
        }
        if (invalid)
            c1 = c0 = mc[0] = mc[1] = mc[2] = mc[3] = 0;
        if (multi) {
            // Two pixels for each pair of bits; the pairs 10 and 11 are
            // foreground.
            uint8_t b0 = g >> 6, b1 = (g >> 4) & 3, b2 = (g >> 2) & 3, b3 = g & 3;
            put4(p, (mc[b0] | (uint32_t)mc[b1] << 16) * 0x0101u);
            put4(p + 4, (mc[b2] | (uint32_t)mc[b3] << 16) * 0x0101u);
            if (fg) {
                put4(fg + x0 + i * 8, ((b0 >> 1) | (uint32_t)(b1 >> 1) << 16) * 0x0101u);
                put4(fg + x0 + i * 8 + 4, ((b2 >> 1) | (uint32_t)(b3 >> 1) << 16) * 0x0101u);
            }
        } else {
            uint32_t hi = nibble_mask[g >> 4], lo = nibble_mask[g & 15];
            c1 *= 0x01010101u;
            c0 *= 0x01010101u;
            put4(p, (c1 & hi) | (c0 & ~hi));
            put4(p + 4, (c1 & lo) | (c0 & ~lo));
            if (fg) {
                put4(fg + x0 + i * 8, hi & 0x01010101u);
                put4(fg + x0 + i * 8 + 4, lo & 0x01010101u);
            }
        }
    }
}

int FAST(c64_vic_line_fetch)(c64_vic *v) {
    uint16_t line = v->raster;
    uint8_t d011 = v->reg[0x11], d018 = v->reg[0x18];
    bool bad = v->den_frame && line >= 0x30 && line <= 0xf7 && (line & 7) == (d011 & 7);
    int stolen = 0;

    // Vertical border: 51-250 open with 25 rows, 55-246 with 24.
    bool rsel = d011 & 0x08;
    if (line == (rsel ? 251 : 247))
        v->vborder = true;
    else if (line == (rsel ? 51 : 55) && (d011 & 0x10))
        v->vborder = false;

    v->vc = v->vcbase;
    uint16_t vm = (uint16_t)(d018 & 0xf0) << 6;
    if (bad) {
        v->display = true;
        v->rc = 0;
        for (int i = 0; i < 40; ++i) {
            uint16_t a = (v->vc + i) & 0x3ff;
            v->matrix[i] = vread(v, vm | a);
            v->colors[i] = v->color[a] & 0x0f;
        }
        stolen += 40;
    }
    for (int n = 0; n < 8; ++n)
        if (v->sp_active[n])
            stolen += 2;
    v->bad = bad;
    int row = (int)line - C64_FIRST_LINE;
    v->line_on = row >= v->draw_from && row < v->draw_to && !v->no_draw;
    v->done_x = 0;
    return stolen;
}

/// The whole line with the registers as they are now.
/// @param v the VIC-II
/// @param pix the line
static void FAST(render)(c64_vic *v, uint8_t *pix) {
    uint8_t fg[C64_SCREEN_W];
    bool sprites = false;
    for (int n = 0; n < 8; ++n)
        sprites |= v->sp_active[n];
    if (!v->vborder)
        draw_graphics(v, pix, sprites ? fg : NULL);
    if (sprites && !v->vborder)
        draw_sprites(v, pix, fg);

    // The border over the top.
    bool csel = v->reg[0x16] & 0x08;
    uint8_t border = v->reg[0x20] & 15;
    int left = csel ? WIN_X : WIN_X + 7, right = csel ? WIN_X + 320 : WIN_X + 311;
    if (v->vborder) {
        memset(pix, border, C64_SCREEN_W);
    } else {
        memset(pix, border, (size_t)left);
        memset(pix + right, border, (size_t)(C64_SCREEN_W - right));
    }
}

/// The line drawn up to x, with the registers as they are.
/// @param v the VIC-II
/// @param x the pixel to draw up to
static void FAST(draw_to)(c64_vic *v, int x) {
    if (x > C64_SCREEN_W)
        x = C64_SCREEN_W;
    if (!v->line_on || x <= v->done_x)
        return;
    if (v->done_x == 0 && x == C64_SCREEN_W) {
        render(v, v->line);  // the usual case: all of it at once
    } else {
        uint8_t pix[C64_SCREEN_W];
        render(v, pix);
        memcpy(v->line + v->done_x, pix + v->done_x, (size_t)(x - v->done_x));
    }
    v->done_x = (int16_t)x;
}

int c64_vic_cycle_x(int cycle) {
    // The beam's sprite X in a PAL line's cycle c (0 here is cycle 1 in
    // Bauer's tables, X $194): 8 pixels a cycle, the line 504 wide. A
    // write shows from the next cycle's pixels.
    if (cycle <= 12)
        return 0;  // before the picture starts: all of it
    int x = (0x194 + (cycle + 1) * 8) % 504 - C64_FIRST_X;
    if (cycle >= 59 || x > C64_SCREEN_W)
        return C64_SCREEN_W;  // past it: the next line
    return x;
}

void FAST(c64_vic_line_finish)(c64_vic *v) {
    if (v->line_on) {
        draw_to(v, C64_SCREEN_W);
        int row = (int)v->raster - C64_FIRST_LINE;
        if (v->frame) {
            uint8_t *out = v->frame + row * C64_SCREEN_W;
            if (memcmp(out, v->line, C64_SCREEN_W)) {
                memcpy(out, v->line, C64_SCREEN_W);
                v->dirty[row >> 5] |= 1u << (row & 31);
            }
        }
        if (v->line_out)
            v->line_out(v->out_ctx, row, v->line);
        v->line_on = false;
    }

    // Cycle 58: after the eighth row of a character line, idle (unless this
    // is a bad line) and move on to the next.
    if (v->display)
        v->vc = (v->vc + 40) & 0x3ff;
    if (v->rc == 7) {
        v->vcbase = v->vc;
        if (!v->bad)
            v->display = false;
    }
    if (v->display)
        v->rc = (v->rc + 1) & 7;
}

bool c64_vic_line_end(c64_vic *v) {
    // Sprites shown on this line move on to their next row (each row twice
    // when expanded vertically), and those whose Y is this line start.
    for (int n = 0; n < 8; ++n) {
        bool yexp = (v->reg[0x17] >> n) & 1;
        if (v->sp_active[n]) {
            if (!yexp || v->sp_yexp_ff[n])
                v->sp_mcbase[n] += 3;
            v->sp_yexp_ff[n] = yexp && !v->sp_yexp_ff[n];
            if (v->sp_mcbase[n] >= 63)
                v->sp_active[n] = false;
        }
        if (!v->sp_active[n] && ((v->reg[0x15] >> n) & 1) && v->reg[1 + 2 * n] == (v->raster & 0xff)) {
            v->sp_active[n] = true;
            v->sp_mcbase[n] = 0;
            v->sp_yexp_ff[n] = false;
        }
    }
    if (++v->raster == C64_LINES) {
        v->raster = 0;
        return true;
    }
    return false;
}
