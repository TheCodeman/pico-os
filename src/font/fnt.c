/**
 * @file fnt.c
 * @brief Anti-aliased text from TrueType, CFF OpenType and PFN fonts, with a glyph cache (see fnt.h).
 *
 * PFN, the pre-drawn format (all numbers little-endian, offsets from the
 * file's start; made by tools/fonttest/mkpfn.py):
 *
 *     header   "PFN1", u16 sizes, u16 bits per pixel (4), u32 offset of each size
 *     a size   u16 px (per em), s16 ascent, s16 descent, s16 line gap,
 *              u16 glyphs, u16 kerning pairs, u32 glyph table, u32 pairs, u32 pixels
 *     a glyph  u16 character, u8 w, u8 h, s8 x offset, s8 y offset (from the pen
 *              on the baseline to the top left), u16 advance (1/64 px), u32 pixels
 *              (from the size's pixels): sorted by character
 *     a pair   u16 left, u16 right, s16 adjustment (1/64 px): sorted
 *     pixels   rows of 4-bit coverage, the high nibble first, each row whole bytes
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "fnt.h"

#include <stdlib.h>
#include <string.h>

#include "fnt_stb.h"
#include "psram_heap.h"

#define SLOTS      2048         ///< glyph cache entries (a power of two)
#define SLOTS_FULL 1536         ///< emptied when this many are used
#define BUDGET     (1u << 20)   ///< and by default when its pixels pass this
#define MAX_SIZES  16           ///< sizes a PFN file can have

/// A cached glyph.
typedef struct {
    uint32_t key;     ///< character | size << 21, 0: free
    int32_t adv;      ///< advance, 1/64 px
    int16_t w, h;     ///< its pixels' size
    int16_t xoff;     ///< from the pen to its left
    int16_t yoff;     ///< from the baseline to its top
    int32_t gi;       ///< glyph index
    uint8_t *bm;      ///< w * h bytes of coverage (PSRAM), NULL if empty
} slot_t;

/// A glyph about to be drawn.
typedef struct {
    int w, h, xoff, yoff;  ///< as slot_t
    int32_t adv;           ///< advance, 1/64 px
    int gi;                ///< glyph index (PFN: its entry)
    const uint8_t *bm;     ///< its pixels
    bool nib;              ///< 4-bit pixels (PFN), else 8-bit
} glyph_t;

/// A PFN file's size.
typedef struct {
    int px, asc, desc, gap;  ///< as the header
    int n, nk;               ///< glyphs, pairs
    const uint8_t *g;        ///< the glyph table
    const uint8_t *k;        ///< the pairs
    const uint8_t *bits;     ///< the pixels
} strike_t;

/// An open font.
struct fnt {
    fnt_kind_t kind;          ///< what it is
    stbtt_fontinfo info;      ///< outline fonts
    strike_t strike[MAX_SIZES];  ///< PFN sizes
    int nstrikes;             ///< how many
    bool cache, kern, dither; ///< settings
    slot_t *slots;            ///< the cache (PSRAM)
    int used;                 ///< entries in it
    size_t budget;            ///< see fnt_cache_budget
    fnt_stats_t st;           ///< figures
};

/// The SRAM arena, shared by the fonts open (they draw one glyph at a time).
static uint8_t *Ar;
static int ar_users;    ///< fonts open that use it
static size_t ar_used;  ///< bytes of it given out for this glyph

// ---- stb_truetype's memory ------------------------------------------------------------------------

void *fnt_stb_alloc(size_t n, void *user) {
    fnt_t *f = user;
    n = (n + 7) & ~(size_t)7;
    if (Ar && ar_used + n <= FNT_ARENA) {
        void *p = Ar + ar_used;
        ar_used += n;
        if (ar_used > f->st.arena_peak)
            f->st.arena_peak = ar_used;
        return p;
    }
    f->st.spills++;
    return psram_malloc(n);
}

void fnt_stb_free(void *p, void *user) {
    (void)user;
    if (p && !(Ar && (uint8_t *)p >= Ar && (uint8_t *)p < Ar + FNT_ARENA))
        psram_free(p);  // the arena's blocks go all at once, at the next glyph
}

// ---- Reading files ---------------------------------------------------------------------------------

/// @param p two bytes, little-endian
/// @return them
static int rd16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

/// @param p two bytes, little-endian, signed
/// @return them
static int rds16(const uint8_t *p) {
    return (int16_t)(p[0] | p[1] << 8);
}

/// @param p four bytes, little-endian
/// @return them
static uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/// Reads a PFN file's sizes.
/// @param f the font
/// @param d the file
/// @param size its length
/// @return false if it isn't a good one
static bool pfn_open(fnt_t *f, const uint8_t *d, size_t size) {
    if (size < 8 || memcmp(d, "PFN1", 4) || rd16(d + 6) != 4)
        return false;
    int n = rd16(d + 4);
    if (n < 1 || n > MAX_SIZES || 8 + 4 * (size_t)n > size)
        return false;
    for (int i = 0; i < n; ++i) {
        uint32_t o = rd32(d + 8 + 4 * i);
        if (o + 24 > size)
            return false;
        const uint8_t *s = d + o;
        strike_t *t = &f->strike[i];
        t->px = rd16(s);
        t->asc = rds16(s + 2);
        t->desc = rds16(s + 4);
        t->gap = rds16(s + 6);
        t->n = rd16(s + 8);
        t->nk = rd16(s + 10);
        uint32_t go = rd32(s + 12), ko = rd32(s + 16), bo = rd32(s + 20);
        if (go + 12u * t->n > size || ko + 6u * t->nk > size || bo > size || t->n < 1)
            return false;
        for (int j = 0; j < t->n; ++j) {  // every glyph's pixels in the file
            const uint8_t *e = d + go + 12 * j;
            if (bo + rd32(e + 8) + (size_t)((e[2] + 1) / 2) * e[3] > size)
                return false;
        }
        t->g = d + go;
        t->k = d + ko;
        t->bits = d + bo;
    }
    f->nstrikes = n;
    f->kind = FNT_PFN;
    return true;
}

/// The PFN size nearest to one asked for.
/// @param f the font
/// @param px the size
/// @return the strike
static strike_t *strike_for(fnt_t *f, int px) {
    strike_t *best = &f->strike[0];
    for (int i = 1; i < f->nstrikes; ++i)
        if (abs(f->strike[i].px - px) < abs(best->px - px))
            best = &f->strike[i];
    return best;
}

/// A character's entry in a PFN size.
/// @param t the size
/// @param cp the character
/// @return its entry, or -1
static int pfn_find(const strike_t *t, int cp) {
    int i = cp - rd16(t->g);  // straight there when the characters run on without gaps
    if (i >= 0 && i < t->n && rd16(t->g + 12 * i) == cp)
        return i;
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = rd16(t->g + 12 * mid);
        if (c == cp)
            return mid;
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/// A PFN pair's adjustment.
/// @param t the size
/// @param a the left character
/// @param b the right one
/// @return the adjustment, 1/64 px
static int pfn_kern(const strike_t *t, int a, int b) {
    uint32_t want = (uint32_t)a << 16 | (uint32_t)b;
    int lo = 0, hi = t->nk - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *e = t->k + 6 * mid;
        uint32_t k = (uint32_t)rd16(e) << 16 | (uint32_t)rd16(e + 2);
        if (k == want)
            return rds16(e + 4);
        if (k < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

// ---- Opening ---------------------------------------------------------------------------------------

fnt_t *fnt_open(const uint8_t *data, size_t size, const char **err) {
    fnt_t *f = psram_malloc(sizeof *f);
    if (!f) {
        *err = "no memory";
        return NULL;
    }
    memset(f, 0, sizeof *f);
    f->cache = f->kern = f->dither = true;
    f->budget = BUDGET;
    if (size >= 4 && !memcmp(data, "PFN1", 4)) {
        if (!pfn_open(f, data, size)) {
            *err = "not a good PFN file";
            psram_free(f);
            return NULL;
        }
        return f;
    }
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    f->info.userdata = f;
    if (size < 12 || off < 0 || !stbtt_InitFont(&f->info, data, off)) {
        *err = "not a TrueType or OpenType font";
        psram_free(f);
        return NULL;
    }
    f->kind = f->info.cff.size > 0 ? FNT_CFF : FNT_TTF;
    f->slots = psram_malloc(SLOTS * sizeof(slot_t));
    if (!Ar)
        Ar = malloc(FNT_ARENA);
    if (Ar)
        ar_users++;
    if (!f->slots || !Ar) {
        *err = "no memory";
        fnt_close(f);
        return NULL;
    }
    memset(f->slots, 0, SLOTS * sizeof(slot_t));
    return f;
}

void fnt_close(fnt_t *f) {
    if (!f)
        return;
    if (f->slots) {
        fnt_cache_clear(f);
        psram_free(f->slots);
    }
    if (f->kind != FNT_PFN && Ar && --ar_users == 0) {
        free(Ar);
        Ar = NULL;
    }
    psram_free(f);
}

fnt_kind_t fnt_kind(const fnt_t *f) {
    return f->kind;
}

int fnt_glyphs(const fnt_t *f) {
    return f->kind == FNT_PFN ? f->strike[0].n : f->info.numGlyphs;
}

int fnt_sizes(const fnt_t *f, int *out, int max) {
    int n = 0;
    for (int i = 0; i < f->nstrikes && n < max; ++i)
        out[n++] = f->strike[i].px;
    for (int i = 1; i < n; ++i)  // smallest first
        for (int j = i; j > 0 && out[j - 1] > out[j]; --j) {
            int t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

int fnt_size_used(const fnt_t *f, int px) {
    return f->kind == FNT_PFN ? strike_for((fnt_t *)f, px)->px : px;
}

void fnt_vmetrics(fnt_t *f, int px, int *ascent, int *descent, int *gap) {
    if (f->kind == FNT_PFN) {
        strike_t *t = strike_for(f, px);
        *ascent = t->asc;
        *descent = t->desc;
        *gap = t->gap;
        return;
    }
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    float sc = stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
    *ascent = (int)ceilf(a * sc);
    *descent = (int)ceilf(-d * sc);
    *gap = (int)(g * sc + 0.5f);
}

void fnt_set_cache(fnt_t *f, bool on) {
    f->cache = on;
}

void fnt_set_kerning(fnt_t *f, bool on) {
    f->kern = on;
}

void fnt_set_dither(fnt_t *f, bool on) {
    f->dither = on;
}

void fnt_cache_clear(fnt_t *f) {
    if (!f->slots)
        return;
    for (int i = 0; i < SLOTS; ++i)
        psram_free(f->slots[i].bm);
    memset(f->slots, 0, SLOTS * sizeof(slot_t));
    f->used = 0;
    f->st.cache_bytes = 0;
}

void fnt_cache_budget(fnt_t *f, size_t bytes) {
    f->budget = bytes;
}

void fnt_get_stats(const fnt_t *f, fnt_stats_t *out) {
    *out = f->st;
}

// ---- Glyphs ----------------------------------------------------------------------------------------

/// Draws a glyph from its outline into the arena (or PSRAM if it is big).
/// @param f an outline font
/// @param sc the scale
/// @param gi the glyph
/// @param g filled in; g->bm valid until the next glyph
static void outline_glyph(fnt_t *f, float sc, int gi, glyph_t *g) {
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&f->info, gi, sc, sc, &x0, &y0, &x1, &y1);
    g->adv = (int32_t)(adv * sc * 64.0f + 0.5f);
    g->gi = gi;
    g->xoff = x0;
    g->yoff = y0;
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->nib = false;
    g->bm = NULL;
    ar_used = 0;
    if (g->w <= 0 || g->h <= 0) {
        g->w = g->h = 0;
        return;
    }
    uint8_t *px = fnt_stb_alloc((size_t)g->w * g->h, f);
    if (!px) {
        g->w = g->h = 0;
        return;
    }
    stbtt_MakeGlyphBitmap(&f->info, px, g->w, g->h, g->w, sc, sc, gi);
    f->st.drawn++;
    g->bm = px;
}

/// Gets a glyph to draw: from the cache, the outline or the PFN file.
/// @param f the font
/// @param px the size
/// @param sc the outline scale for it
/// @param cp the character
/// @param g filled in
static void glyph_get(fnt_t *f, int px, float sc, int cp, glyph_t *g) {
    if (f->kind == FNT_PFN) {
        strike_t *t = strike_for(f, px);
        int i = pfn_find(t, cp);
        if (i < 0)
            i = pfn_find(t, '?');
        memset(g, 0, sizeof *g);
        g->nib = true;
        g->gi = cp;
        if (i < 0)
            return;
        const uint8_t *e = t->g + 12 * i;
        g->w = e[2];
        g->h = e[3];
        g->xoff = (int8_t)e[4];
        g->yoff = (int8_t)e[5];
        g->adv = rd16(e + 6);
        g->bm = t->bits + rd32(e + 8);
        return;
    }
    if (!f->cache) {
        outline_glyph(f, sc, stbtt_FindGlyphIndex(&f->info, cp), g);
        return;
    }
    uint32_t key = (uint32_t)cp | (uint32_t)px << 21;
    unsigned h = (key * 2654435761u) >> 21 & (SLOTS - 1);
    while (f->slots[h].key && f->slots[h].key != key)
        h = (h + 1) & (SLOTS - 1);
    slot_t *s = &f->slots[h];
    if (!s->key) {
        if (f->used >= SLOTS_FULL || f->st.cache_bytes > f->budget) {
            fnt_cache_clear(f);  // full: start again (then it fills with what is in use)
            f->st.flushes++;
            glyph_get(f, px, sc, cp, g);
            return;
        }
        outline_glyph(f, sc, stbtt_FindGlyphIndex(&f->info, cp), g);
        uint8_t *bm = NULL;
        size_t n = (size_t)g->w * g->h;
        if (n && (bm = psram_malloc(n)) == NULL)
            return;  // drawn but not kept
        if (n)
            memcpy(bm, g->bm, n);
        *s = (slot_t){key, g->adv, (int16_t)g->w, (int16_t)g->h, (int16_t)g->xoff, (int16_t)g->yoff, g->gi, bm};
        f->used++;
        f->st.cache_bytes += n;
        g->bm = bm;
        return;
    }
    f->st.hits++;
    *g = (glyph_t){s->w, s->h, s->xoff, s->yoff, s->adv, s->gi, s->bm, false};
}

int fnt_raster_one(fnt_t *f, int px, int cp) {
    if (f->kind == FNT_PFN)
        return -1;
    glyph_t g;
    outline_glyph(f, stbtt_ScaleForMappingEmToPixels(&f->info, (float)px), stbtt_FindGlyphIndex(&f->info, cp), &g);
    return g.w * g.h;
}

int fnt_sdf_one(fnt_t *f, int px, int cp) {
    if (f->kind == FNT_PFN)
        return -1;
    int w = 0, h = 0, xo, yo;
    ar_used = 0;
    unsigned char *s = stbtt_GetCodepointSDF(&f->info, stbtt_ScaleForMappingEmToPixels(&f->info, (float)px), cp, 4,
                                             128, 32.0f, &w, &h, &xo, &yo);
    if (s)
        stbtt_FreeSDF(s, f);
    return w * h;
}

int fnt_find(fnt_t *f, int cp) {
    if (f->kind == FNT_PFN)
        return pfn_find(&f->strike[0], cp);
    return stbtt_FindGlyphIndex(&f->info, cp);
}

// ---- Drawing ---------------------------------------------------------------------------------------

/// RGB332's levels as 8 bits.
static const uint8_t lv3[8] = {0, 36, 73, 109, 146, 182, 219, 255};
/// And blue's.
static const uint8_t lv2[4] = {0, 85, 170, 255};
/// The 4x4 ordered-dither thresholds, 0..15.
static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/// The next character of UTF-8 text (a bad byte is itself).
/// @param s the text, moved on
/// @return the character, 0 at the end
static int utf8_next(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    int c = *p++;
    if (c >= 0xc0 && c < 0xf8) {
        int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        int v = c & (0x3f >> n);
        int i;
        for (i = 0; i < n && (p[i] & 0xc0) == 0x80; ++i)
            v = v << 6 | (p[i] & 0x3f);
        if (i == n) {
            *s = (const char *)p + n;
            return v;
        }
    }
    *s = (const char *)p;
    return c;
}

/// Blends a glyph's pixels into the picture.
/// @param f the font (for dithering)
/// @param g the glyph
/// @param dst the picture
/// @param stride its row bytes
/// @param cw its width
/// @param ch its height
/// @param x the glyph's left
/// @param y its top
/// @param fg the colour
static void blit(const fnt_t *f, const glyph_t *g, uint8_t *dst, int stride, int cw, int ch, int x, int y,
                 uint8_t fg) {
    int fr = lv3[fg >> 5], fgr = lv3[fg >> 2 & 7], fb = lv2[fg & 3];
    int r0 = y < 0 ? -y : 0, r1 = y + g->h > ch ? ch - y : g->h;
    int c0 = x < 0 ? -x : 0, c1 = x + g->w > cw ? cw - x : g->w;
    int rowb = g->nib ? (g->w + 1) / 2 : g->w;
    for (int r = r0; r < r1; ++r) {
        const uint8_t *src = g->bm + r * rowb;
        uint8_t *d = dst + (y + r) * stride + x;
        const uint8_t *th = bayer[(y + r) & 3];
        for (int c = c0; c < c1; ++c) {
            int a = g->nib ? (c & 1 ? src[c >> 1] & 15 : src[c >> 1] >> 4) * 17 : src[c];
            if (a == 0)
                continue;
            if (a == 255) {
                d[c] = fg;
                continue;
            }
            int p = d[c];
            int a1 = a + (a >> 7);  // 0..256
            int r8 = lv3[p >> 5], g8 = lv3[p >> 2 & 7], b8 = lv2[p & 3];
            r8 += ((fr - r8) * a1) >> 8;
            g8 += ((fgr - g8) * a1) >> 8;
            b8 += ((fb - b8) * a1) >> 8;
            int t = f->dither ? th[(x + c) & 3] * 16 + 8 : 127;
            // (v * levels + t) / 255, as a multiply: exact for these ranges
            int q3r = ((r8 * 7 + t) * 257 + 257) >> 16, q3g = ((g8 * 7 + t) * 257 + 257) >> 16;
            int q2 = ((b8 * 3 + t) * 257 + 257) >> 16;
            d[c] = (uint8_t)(q3r << 5 | q3g << 2 | q2);
        }
    }
}

int fnt_draw(fnt_t *f, int px, uint8_t *dst, int stride, int cw, int ch, int x, int y, const char *s,
             uint8_t fg) {
    float sc = f->kind == FNT_PFN ? 0 : stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
    strike_t *t = f->kind == FNT_PFN ? strike_for(f, px) : NULL;
    int32_t pen = (int32_t)x * 64;
    int prev = -1;
    for (int cp; (cp = utf8_next(&s)) != 0;) {
        glyph_t g;
        glyph_get(f, px, sc, cp, &g);
        if (f->kern && prev >= 0)
            pen += t ? pfn_kern(t, prev, cp)
                     : (int32_t)(stbtt_GetGlyphKernAdvance(&f->info, prev, g.gi) * sc * 64.0f);
        prev = t ? cp : g.gi;
        if (dst && g.bm)
            blit(f, &g, dst, stride, cw, ch, ((pen + 32) >> 6) + g.xoff, y + g.yoff, fg);
        pen += g.adv;
    }
    return (pen + 32) >> 6;
}

int32_t fnt_advance(fnt_t *f, int px, int cp) {
    if (f->kind == FNT_PFN) {
        strike_t *t = strike_for(f, px);
        int i = pfn_find(t, cp);
        return i < 0 ? 0 : rd16(t->g + 12 * i + 6);
    }
    int gi = stbtt_FindGlyphIndex(&f->info, cp), adv, lsb;
    if (!gi)
        return 0;
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    return (int32_t)(adv * stbtt_ScaleForMappingEmToPixels(&f->info, (float)px) * 64.0f + 0.5f);
}

void fnt_draw_cp(fnt_t *f, int px, uint8_t *dst, int stride, int cw, int ch, int x, int y, int cp, uint8_t fg) {
    float sc = f->kind == FNT_PFN ? 0 : stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
    glyph_t g;
    glyph_get(f, px, sc, cp, &g);
    if (g.bm)
        blit(f, &g, dst, stride, cw, ch, x + g.xoff, y + g.yoff, fg);
}

int fnt_width(fnt_t *f, int px, const char *s) {
    return fnt_draw(f, px, NULL, 0, 0, 0, 0, 0, s, 0);
}
