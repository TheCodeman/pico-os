/**
 * @file png_rows.c
 * @brief PNGs decoded a row at a time straight into an RGB332 picture (see image_rows.h).
 *
 * LodePNG inflates the whole picture into memory, unfilters it into another
 * buffer and then it is dithered from that: with the picture in PSRAM that
 * is several passes over a megabyte through a 16 KB cache. Here the deflate
 * stream is inflated into a window of its own size (32 KB at most), each
 * scanline is unfiltered as soon as it is complete (two rows kept), and the
 * rows the picture needs are dithered into it at once. The window and the
 * rows are in SRAM when the heap has room to spare, else in PSRAM.
 *
 * Opaque (no alpha channel, no tRNS), non-interlaced PNGs of any bit depth
 * and colour type are decoded; the rest are left to LodePNG.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "image_rows.h"

#define FAST_BITS 10                 ///< Huffman codes this long or shorter are found with one look-up
#define FAST_SIZE (1u << FAST_BITS)  ///< entries in a look-up table

// ---- Memory ----------------------------------------------------------------------------------------

#ifdef PICO_BUILD
#include <malloc.h>
#include <unistd.h>

#include "psram_heap.h"

#define SRAM_KEEP (24 * 1024)  ///< the SRAM heap is only used if this much is left free after

extern char __StackLimit;  ///< top of the heap, from the SDK's linker script

/// A buffer: from the SRAM heap if there is room to spare, else PSRAM.
/// @param n its size
/// @return it, or NULL
static void *buf_alloc(size_t n) {
    struct mallinfo mi = mallinfo();
    long room = (long)(&__StackLimit - (char *)sbrk(0)) + (long)mi.fordblks;
    if (room > (long)(n + SRAM_KEEP)) {
        void *p = malloc(n);
        if (p)
            return p;
    }
    return psram_malloc(n);
}

/// Frees a buffer from buf_alloc.
/// @param p it (NULL is fine)
static void buf_free(void *p) {
    if ((uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x12000000u)
        psram_free(p);
    else
        free(p);
}
#else
/// A buffer (host builds: malloc).
/// @param n its size
/// @return it, or NULL
static void *buf_alloc(size_t n) {
    return malloc(n);
}
/// Frees a buffer.
/// @param p it
static void buf_free(void *p) {
    free(p);
}
#endif

// ---- The state ---------------------------------------------------------------------------------------

/// A Huffman code: puff's counts and symbols, and a look-up table for short codes.
typedef struct {
    int16_t count[16];        ///< codes of each length
    int16_t symbol[288];      ///< the symbols, in canonical order
    uint16_t fast[FAST_SIZE]; ///< by the next FAST_BITS bits (reversed): symbol << 4 | length, 0 if longer
} huff_t;

/// The decoder.
typedef struct {
    // The compressed bytes: the IDAT chunks one after another.
    const uint8_t *d;   ///< the file
    size_t len;         ///< its size
    size_t at;          ///< the next byte
    uint32_t left;      ///< bytes left in this IDAT
    uint32_t bitbuf;    ///< bits read, not used yet
    int bitcnt;         ///< how many
    int over;           ///< bits made up past the end (0s)
    huff_t lit, dist;   ///< the codes
    // The inflated bytes.
    uint8_t *win;       ///< the last wsize of them
    uint32_t wsize;     ///< a power of two
    uint32_t wpos;      ///< how many so far
    // The scanlines.
    uint8_t *cur, *prev;  ///< this one and the one before (filter byte first)
    uint8_t *rgb;       ///< a row as RGB (or cur + 1 itself)
    int rowlen;         ///< a scanline's bytes, filter byte too
    int rpos;           ///< bytes of this one so far
    int fbpp;           ///< bytes a pixel for the filters (at least 1)
    int iw, ih;         ///< the picture's size
    int ctype, depth;   ///< its colour type and bit depth
    uint8_t pal[256 * 3];  ///< the palette
    int sy;             ///< the next scanline
    // The picture made.
    uint8_t *px;        ///< w * h bytes
    int w, h;           ///< its size
    int dy;             ///< its next row
    int64_t step;       ///< picture columns a column, 16.16
    bool bad;           ///< the stream is damaged
} ps_t;

/// @param p four bytes, big-endian
/// @return them
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// ---- Reading bits -----------------------------------------------------------------------------------

/// The next compressed byte (from the next IDAT chunk when this one ends).
/// @param s the decoder
/// @return it, or -1 at the end
static int in_byte(ps_t *s) {
    while (s->left == 0) {
        if (s->at + 12 > s->len || memcmp(s->d + s->at + 4, "IDAT", 4))
            return -1;
        uint32_t n = be32(s->d + s->at);
        if (n > s->len - s->at - 12)
            return -1;
        s->at += 8;
        s->left = n;
        if (!n)
            s->at += 4;  // an empty one: its CRC
    }
    int b = s->d[s->at++];
    if (--s->left == 0)
        s->at += 4;  // the CRC (not checked)
    return b;
}

/// Makes sure n bits (up to 24) are in the buffer; past the end, 0s.
/// @param s the decoder
/// @param n how many
static inline void need(ps_t *s, int n) {
    while (s->bitcnt < n) {
        int b = in_byte(s);
        if (b < 0) {
            b = 0;
            s->over += 8;
        }
        s->bitbuf |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
}

/// Takes n bits (up to 24), first bit lowest.
/// @param s the decoder
/// @param n how many
/// @return them
static inline uint32_t bits(ps_t *s, int n) {
    need(s, n);
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

// ---- Huffman codes --------------------------------------------------------------------------------------

/// Builds a code from its lengths (incomplete codes are allowed: an unused
/// code then fails when met).
/// @param h filled in
/// @param len each symbol's code length (0: none)
/// @param n how many symbols
/// @return false if the lengths make no code (too many of a length)
static bool build(huff_t *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; ++i)
        h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left = (left << 1) - h->count[l];
        if (left < 0)
            return false;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l)
        offs[l + 1] = (int16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; ++i)
        if (len[i])
            h->symbol[offs[len[i]]++] = (int16_t)i;
    // The look-up table: each short code, reversed (the bits come lowest first).
    memset(h->fast, 0, sizeof h->fast);
    int code = 0, k = 0;
    for (int l = 1; l <= FAST_BITS; ++l) {
        for (int c = 0; c < h->count[l]; ++c, ++code) {
            int rev = 0;
            for (int b = 0; b < l; ++b)
                rev |= ((code >> b) & 1) << (l - 1 - b);
            for (int j = rev; j < (int)FAST_SIZE; j += 1 << l)
                h->fast[j] = (uint16_t)(h->symbol[k] << 4 | l);
            ++k;
        }
        code <<= 1;
    }
    return true;
}

/// Decodes a symbol.
/// @param s the decoder
/// @param h the code
/// @return the symbol, or -1 for a code that isn't in it
static int decode(ps_t *s, const huff_t *h) {
    need(s, 16);
    uint16_t e = h->fast[s->bitbuf & (FAST_SIZE - 1)];
    if (e) {
        s->bitbuf >>= e & 15;
        s->bitcnt -= e & 15;
        return e >> 4;
    }
    // A longer code: bit by bit (as puff does).
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= (int)bits(s, 1);
        int count = h->count[l];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// ---- Scanlines -----------------------------------------------------------------------------------------

/// Paeth's predictor.
/// @param a left
/// @param b above
/// @param c above left
/// @return the one nearest a + b - c
static inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/// A scanline is complete: unfilters it and dithers the picture's rows that show it.
/// @param s the decoder
static void row_done(ps_t *s) {
    uint8_t *x = s->cur + 1, *p = s->prev + 1;
    int n = s->rowlen - 1, f = s->fbpp;
    switch (s->cur[0]) {
    case 0: break;
    case 1:
        for (int i = f; i < n; ++i)
            x[i] = (uint8_t)(x[i] + x[i - f]);
        break;
    case 2:
        for (int i = 0; i < n; ++i)
            x[i] = (uint8_t)(x[i] + p[i]);
        break;
    case 3:
        for (int i = 0; i < n; ++i)
            x[i] = (uint8_t)(x[i] + ((i >= f ? x[i - f] : 0) + p[i]) / 2);
        break;
    case 4:
        for (int i = 0; i < n; ++i)
            x[i] = (uint8_t)(x[i] + paeth(i >= f ? x[i - f] : 0, p[i], i >= f ? p[i - f] : 0));
        break;
    default: s->bad = true; break;
    }
    // The picture's rows that show this scanline (none when shrinking a lot).
    bool wanted = s->dy < s->h && (int)((long long)s->dy * s->ih / s->h) == s->sy;
    if (wanted) {
        const uint8_t *rgb = s->rgb;
        if (s->ctype == 2 && s->depth == 8) {
            rgb = x;  // RGB already
        } else {
            uint8_t *o = s->rgb;
            for (int i = 0; i < s->iw; ++i, o += 3) {
                int v;
                if (s->depth == 16) {
                    if (s->ctype == 2) {
                        o[0] = x[i * 6], o[1] = x[i * 6 + 2], o[2] = x[i * 6 + 4];
                        continue;
                    }
                    v = x[i * 2];
                } else if (s->depth == 8) {
                    v = x[i];
                } else {  // 1, 2 or 4 bits, the first pixel highest
                    int per = 8 / s->depth, sh = 8 - s->depth * (i % per + 1);
                    v = x[i / per] >> sh & ((1 << s->depth) - 1);
                    if (s->ctype == 0)
                        v = v * 255 / ((1 << s->depth) - 1);
                }
                if (s->ctype == 3) {
                    memcpy(o, s->pal + v * 3, 3);
                } else {
                    o[0] = o[1] = o[2] = (uint8_t)v;
                }
            }
        }
        uint8_t line[640];
        while (s->dy < s->h && (int)((long long)s->dy * s->ih / s->h) == s->sy) {
            uint8_t *dst = s->px + (size_t)s->dy * s->w;
            // Made in SRAM, copied out whole (the picture is likely in PSRAM).
            bool via = s->w <= (int)sizeof line;
            image_dither_row(rgb, s->iw, via ? line : dst, 0, 0, s->w, s->dy, s->step);
            if (via)
                memcpy(dst, line, (size_t)s->w);
            s->dy++;
        }
    }
    s->sy++;
    uint8_t *t = s->prev;
    s->prev = s->cur;
    s->cur = t;
    s->rpos = 0;
}

/// An inflated byte: into the window and the scanline.
/// @param s the decoder
/// @param b the byte
static inline void out(ps_t *s, uint8_t b) {
    s->win[s->wpos++ & (s->wsize - 1)] = b;
    s->cur[s->rpos++] = b;
    if (s->rpos == s->rowlen)
        row_done(s);
}

// ---- Inflating ------------------------------------------------------------------------------------------

/// Length codes 257-285: base lengths and extra bits.
static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
/// Their extra bits.
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
/// Distance codes 0-29: base distances.
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
/// Their extra bits.
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// Reads a dynamic block's codes.
/// @param s the decoder
/// @return false if they are damaged
static bool dynamic_codes(ps_t *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    uint8_t lengths[320];
    memset(lengths, 0, 19);
    for (int i = 0; i < ncode; ++i)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (!build(&s->lit, lengths, 19))  // (the code lengths' code, in lit for now)
        return false;
    for (int i = 0; i < nlen + ndist;) {
        int sym = decode(s, &s->lit);
        if (sym < 0)
            return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int rep, v = 0;
        if (sym == 16) {
            if (!i)
                return false;
            v = lengths[i - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return false;
        while (rep--)
            lengths[i++] = (uint8_t)v;
    }
    if (!lengths[256])
        return false;  // no end-of-block code
    return build(&s->lit, lengths, nlen) && build(&s->dist, lengths + nlen, ndist);
}

/// Inflates until the picture's last row is made (or the stream ends).
/// @param s the decoder
/// @return false if the stream is damaged
static bool inflate(ps_t *s) {
    bool last = false;
    while (!last && s->dy < s->h) {
        last = bits(s, 1);
        int type = (int)bits(s, 2);
        if (type == 0) {  // stored
            s->bitbuf >>= s->bitcnt & 7;
            s->bitcnt -= s->bitcnt & 7;
            uint32_t n = bits(s, 16), nn = bits(s, 16);
            if ((n ^ 0xffff) != nn)
                return false;
            while (n-- && s->dy < s->h)
                out(s, (uint8_t)bits(s, 8));
        } else if (type == 1 || type == 2) {
            if (type == 1) {  // the fixed codes
                uint8_t l[288];
                memset(l, 8, 144);
                memset(l + 144, 9, 112);
                memset(l + 256, 7, 24);
                memset(l + 280, 8, 8);
                build(&s->lit, l, 288);
                memset(l, 5, 30);
                build(&s->dist, l, 30);
            } else if (!dynamic_codes(s)) {
                return false;
            }
            for (;;) {
                int sym = decode(s, &s->lit);
                if (sym < 0 || s->over > 64 || s->bad)
                    return false;
                if (sym < 256) {
                    out(s, (uint8_t)sym);
                    continue;
                }
                if (sym == 256)
                    break;
                sym -= 257;
                if (sym >= 29)
                    return false;
                int n = len_base[sym] + (int)bits(s, len_extra[sym]);
                int ds = decode(s, &s->dist);
                if (ds < 0 || ds >= 30)
                    return false;
                uint32_t dist = dist_base[ds] + bits(s, dist_extra[ds]);
                if (dist > s->wpos || dist > s->wsize)
                    return false;
                while (n--)
                    out(s, s->win[(s->wpos - dist) & (s->wsize - 1)]);
                if (s->dy >= s->h)
                    break;
            }
        } else {
            return false;
        }
        if (s->over > 64 || s->bad)
            return false;
    }
    return true;
}

// ---- The file --------------------------------------------------------------------------------------------

int png_rows_decode(const uint8_t *d, size_t len, uint8_t *px, int w, int h, const char **err) {
    if (len < 45 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) || memcmp(d + 12, "IHDR", 4) || w <= 0 || h <= 0)
        return 0;
    int iw = (int)be32(d + 16), ih = (int)be32(d + 20), depth = d[24], ctype = d[25];
    if (iw <= 0 || ih <= 0 || iw > PNG_ROWS_MAX_W || ih > 65535 || d[28] != 0 || d[26] || d[27])
        return 0;  // interlaced, or odd: LodePNG
    if (ctype == 4 || ctype == 6)
        return 0;  // an alpha channel
    int chans = ctype == 2 ? 3 : 1;
    bool ok_depth = ctype == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                    : ctype == 2 ? (depth == 8 || depth == 16)
                    : ctype == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                                 : false;
    if (!ok_depth)
        return 0;
    ps_t *s = buf_alloc(sizeof *s);
    if (!s)
        return 0;
    memset(s, 0, sizeof *s);
    s->d = d, s->len = len, s->iw = iw, s->ih = ih, s->ctype = ctype, s->depth = depth;
    s->px = px, s->w = w, s->h = h;
    s->step = ((int64_t)iw << 16) / w;
    // The chunks before the pixels: the palette, and no tRNS.
    size_t at = 8;
    int r = -1;
    while (at + 12 <= len) {
        uint32_t n = be32(d + at);
        const uint8_t *t = d + at + 4;
        if (n > len - at - 12)
            break;
        if (!memcmp(t, "tRNS", 4)) {
            r = 0;  // see-through parts: LodePNG
            break;
        }
        if (!memcmp(t, "PLTE", 4))
            memcpy(s->pal, d + at + 8, n < sizeof s->pal ? n : sizeof s->pal);
        if (!memcmp(t, "IDAT", 4)) {
            s->at = at;
            r = 1;
            break;
        }
        at += 12 + (size_t)n;
    }
    if (r <= 0) {
        buf_free(s);
        if (r < 0)
            *err = "no picture in the PNG";
        return r;
    }
    s->rowlen = 1 + (int)(((long long)iw * chans * depth + 7) / 8);
    s->fbpp = chans * depth / 8 > 0 ? chans * depth / 8 : 1;
    // The zlib header: deflate, its window's size.
    int cmf = in_byte(s), flg = in_byte(s);
    if (cmf < 0 || flg < 0 || (cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 32)) {
        buf_free(s);
        *err = "damaged PNG";
        return -1;
    }
    s->wsize = 1u << ((cmf >> 4) + 8);
    s->win = buf_alloc(s->wsize);
    s->cur = buf_alloc((size_t)s->rowlen);
    s->prev = buf_alloc((size_t)s->rowlen);
    s->rgb = buf_alloc((size_t)iw * 3);
    int result = -1;
    if (!s->win || !s->cur || !s->prev || !s->rgb) {
        *err = "not enough memory";
    } else {
        memset(s->prev, 0, (size_t)s->rowlen);
        if (inflate(s) && s->dy == s->h)
            result = 1;
        else
            *err = "damaged PNG";
    }
    if (result < 0 && s->px)  // what wasn't drawn: black
        for (; s->dy < s->h; ++s->dy)
            memset(px + (size_t)s->dy * w, 0, (size_t)w);
    buf_free(s->rgb);
    buf_free(s->prev);
    buf_free(s->cur);
    buf_free(s->win);
    buf_free(s);
    return result;
}
