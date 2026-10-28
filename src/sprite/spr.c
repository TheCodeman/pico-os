/**
 * @file spr.c
 * @brief Sprite files: see spr.h.
 *
 * The PNG is written with stored (uncompressed) deflate blocks: a sheet of
 * 64 frames of 32x32 is about 256 KB, small enough not to need squeezing.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "spr.h"

#include <stdlib.h>
#include <string.h>

#include "ff.h"

/// The frame buffers both ways: colours and a byte a pixel for drawn.
typedef struct {
    uint8_t px[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< colours
    uint8_t on[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< 1 where drawn
    uint8_t bits[SPR_MAX_SIZE * SPR_MAX_SIZE / 8];  ///< `on` as bits, as in the file
} frame_buf_t;

/// Whether a size and frame count can be in a file.
/// @param i them
/// @return they can
static bool info_ok(const spr_info_t *i) {
    return i->w >= 1 && i->w <= SPR_MAX_SIZE && i->h >= 1 && i->h <= SPR_MAX_SIZE && i->frames >= 1 &&
           i->frames <= SPR_MAX_FRAMES;
}

bool spr_load(const char *path, spr_info_t *info, spr_put_fn put, void *ctx, const char **err) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        *err = "can't open it";
        return false;
    }
    uint8_t head[8];
    UINT got;
    if (f_read(&f, head, 8, &got) != FR_OK || got != 8 || memcmp(head, "SPR1", 4)) {
        f_close(&f);
        *err = "not a sprite file";
        return false;
    }
    info->w = head[4];
    info->h = head[5];
    info->frames = head[6];
    if (!info_ok(info)) {
        f_close(&f);
        *err = "its size is wrong";
        return false;
    }
    frame_buf_t *b = calloc(1, sizeof *b);  // what a small sprite leaves of the buffers stays 0
    if (!b) {
        f_close(&f);
        *err = "not enough memory";
        return false;
    }
    int n = info->w * info->h, nb = (n + 7) / 8;
    bool ok = true;
    *err = NULL;
    for (int i = 0; ok && i < info->frames; ++i) {
        UINT g1, g2;
        if (f_read(&f, b->px, n, &g1) != FR_OK || f_read(&f, b->bits, nb, &g2) != FR_OK || (int)g1 != n ||
            (int)g2 != nb) {
            *err = "it is cut short";
            ok = false;
            break;
        }
        for (int k = 0; k < n; ++k) {
            b->on[k] = b->bits[k >> 3] >> (7 - (k & 7)) & 1;
            if (!b->on[k])
                b->px[k] = 0;
        }
        if (!put(ctx, i, b->px, b->on)) {
            *err = "stopped";
            ok = false;
        }
    }
    free(b);
    f_close(&f);
    return ok;
}

bool spr_save(const char *path, const spr_info_t *info, spr_get_fn get, void *ctx, const char **err) {
    if (!info_ok(info)) {
        *err = "its size is wrong";
        return false;
    }
    frame_buf_t *b = malloc(sizeof *b);
    if (!b) {
        *err = "not enough memory";
        return false;
    }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        free(b);
        *err = "can't create it";
        return false;
    }
    uint8_t head[8] = {'S', 'P', 'R', '1', (uint8_t)info->w, (uint8_t)info->h, (uint8_t)info->frames, 0};
    UINT put;
    bool ok = f_write(&f, head, 8, &put) == FR_OK && put == 8;
    int n = info->w * info->h, nb = (n + 7) / 8;
    for (int i = 0; ok && i < info->frames; ++i) {
        get(ctx, i, b->px, b->on);
        memset(b->bits, 0, nb);
        for (int k = 0; k < n; ++k) {
            if (b->on[k])
                b->bits[k >> 3] |= 0x80 >> (k & 7);
            else
                b->px[k] = 0;
        }
        UINT p1, p2;
        ok = f_write(&f, b->px, n, &p1) == FR_OK && f_write(&f, b->bits, nb, &p2) == FR_OK && (int)p1 == n &&
             (int)p2 == nb;
    }
    if (f_close(&f) != FR_OK)
        ok = false;
    free(b);
    if (!ok)
        *err = "the drive is full or failing";
    return ok;
}

// ---- PNG -------------------------------------------------------------------------------------

/// A PNG being written: buffered, with the chunk's CRC and the deflate stream's Adler-32 kept as it goes.
typedef struct {
    FIL f;              ///< the file
    uint8_t buf[512];   ///< bytes waiting to be written
    int n;              ///< how many
    uint32_t crc;       ///< the chunk's CRC so far
    uint32_t a, b;      ///< Adler-32's two sums
    bool ok;            ///< no write has failed
} png_out_t;

/// Updates a CRC-32 (PNG's) with a byte.
/// @param crc the CRC so far (inverted, as kept while going)
/// @param v the byte
/// @return the new one
static uint32_t crc_byte(uint32_t crc, uint8_t v) {
    crc ^= v;
    for (int k = 0; k < 8; ++k)
        crc = crc & 1 ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    return crc;
}

/// Writes a byte (into the chunk's CRC).
/// @param o the PNG
/// @param v the byte
static void out_byte(png_out_t *o, uint8_t v) {
    o->crc = crc_byte(o->crc, v);
    o->buf[o->n++] = v;
    if (o->n == (int)sizeof o->buf) {
        UINT w;
        if (f_write(&o->f, o->buf, o->n, &w) != FR_OK || (int)w != o->n)
            o->ok = false;
        o->n = 0;
    }
}

/// Writes a big-endian 32-bit number.
/// @param o the PNG
/// @param v it
static void out_u32(png_out_t *o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out_byte(o, (uint8_t)(v >> s));
}

/// Starts a chunk: its length and type (the CRC starts at the type).
/// @param o the PNG
/// @param len the data's length
/// @param type the four letters
static void chunk_start(png_out_t *o, uint32_t len, const char *type) {
    out_u32(o, len);
    o->crc = 0xffffffffu;
    for (int i = 0; i < 4; ++i)
        out_byte(o, (uint8_t)type[i]);
}

/// Ends a chunk with its CRC.
/// @param o the PNG
static void chunk_end(png_out_t *o) {
    out_u32(o, ~o->crc);
}

/// Writes a byte of the picture's data (into the Adler-32 too).
/// @param o the PNG
/// @param v the byte
static void out_data(png_out_t *o, uint8_t v) {
    o->a = (o->a + v) % 65521;
    o->b = (o->b + o->a) % 65521;
    out_byte(o, v);
}

bool spr_save_png(const char *path, const spr_info_t *info, spr_get_fn get, void *ctx, const char **err) {
    if (!info_ok(info)) {
        *err = "its size is wrong";
        return false;
    }
    png_out_t *o = malloc(sizeof *o);
    frame_buf_t *fb = malloc(sizeof *fb);
    if (!o || !fb) {
        free(o);
        free(fb);
        *err = "not enough memory";
        return false;
    }
    memset(o, 0, sizeof *o);
    if (f_open(&o->f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        free(o);
        free(fb);
        *err = "can't create it";
        return false;
    }
    o->ok = true;
    uint32_t w = (uint32_t)(info->w * info->frames), h = (uint32_t)info->h;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (int i = 0; i < 8; ++i)
        out_byte(o, sig[i]);
    chunk_start(o, 13, "IHDR");
    out_u32(o, w);
    out_u32(o, h);
    out_byte(o, 8);  // bits
    out_byte(o, 6);  // RGBA
    out_byte(o, 0);
    out_byte(o, 0);
    out_byte(o, 0);
    chunk_end(o);
    // The data: a zlib header, stored blocks of up to 65535 bytes, Adler-32.
    uint32_t raw = h * (1 + 4 * w), blocks = (raw + 65534) / 65535;
    chunk_start(o, 2 + raw + 5 * blocks + 4, "IDAT");
    out_byte(o, 0x78);
    out_byte(o, 0x01);
    o->a = 1;
    o->b = 0;
    uint32_t left = raw, in_block = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (int fr = -1; fr < info->frames; ++fr) {  // -1: the row's filter byte
            if (fr >= 0)
                get(ctx, fr, fb->px, fb->on);
            int bytes = fr < 0 ? 1 : 4 * info->w;
            for (int i = 0; i < bytes; ++i) {
                if (!in_block) {  // a stored block's header
                    in_block = left < 65535 ? left : 65535;
                    left -= in_block;
                    out_byte(o, left ? 0 : 1);
                    out_byte(o, (uint8_t)in_block);
                    out_byte(o, (uint8_t)(in_block >> 8));
                    out_byte(o, (uint8_t)~in_block);
                    out_byte(o, (uint8_t)(~in_block >> 8));
                }
                uint8_t v = 0;
                if (fr >= 0) {
                    int k = (int)y * info->w + i / 4;
                    uint8_t c = fb->px[k];
                    switch (i & 3) {
                    case 0: v = (uint8_t)((c >> 5) * 255 / 7); break;
                    case 1: v = (uint8_t)((c >> 2 & 7) * 255 / 7); break;
                    case 2: v = (uint8_t)((c & 3) * 85); break;
                    case 3: v = fb->on[k] ? 255 : 0; break;
                    }
                    if (!fb->on[k])
                        v = 0;
                }
                out_data(o, v);
                --in_block;
            }
        }
    }
    out_u32(o, o->b << 16 | o->a);
    chunk_end(o);
    chunk_start(o, 0, "IEND");
    chunk_end(o);
    if (o->n) {
        UINT wr;
        if (f_write(&o->f, o->buf, o->n, &wr) != FR_OK || (int)wr != o->n)
            o->ok = false;
    }
    if (f_close(&o->f) != FR_OK)
        o->ok = false;
    bool ok = o->ok;
    free(o);
    free(fb);
    if (!ok)
        *err = "the drive is full or failing";
    return ok;
}
