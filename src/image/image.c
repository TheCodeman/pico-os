/**
 * @file image.c
 * @brief Implementation of image.h: pictures on the 640x480 RGB332 screen: loading BMP, PNG and JPEG files from the drive, drawing them scaled and dithered, and saving the screen as a BMP.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "image.h"

#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "lodepng.h"
#include "tjpgd.h"

/// The screen's width.
#define SCREEN_W 640
/// Its height.
#define SCREEN_H 480
#define MAX_PIXELS (2600u * 1000u)  ///< ~7.8 MB as RGB: what PSRAM can hold

// ---- Memory: pictures go in PSRAM when there is some ----------------------

#ifdef PICO_BUILD
#include "psram_heap.h"

static bool have_psram(void) {
    return psram_present();
}

static void *big_alloc(size_t n) {
    if (have_psram())
        return psram_malloc(n);
    // No PSRAM: only small pictures, and never so much that malloc (which
    // panics when it runs out) could fail.
    return n <= 64 * 1024 ? malloc(n) : NULL;
}

static bool in_psram(const void *p) {
    return (uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x12000000u;
}

static void big_free(void *p) {
    if (in_psram(p))
        psram_free(p);
    else
        free(p);
}

static void *big_realloc(void *p, size_t n) {
    if (!p)
        return big_alloc(n);
    if (in_psram(p))
        return psram_realloc(p, n);
    return n <= 64 * 1024 ? realloc(p, n) : NULL;
}
#else
/// Allocates a big buffer (host builds: malloc).
/// @param n its size
/// @return it, or NULL
static void *big_alloc(size_t n) {
    return malloc(n);
}
/// Frees a big buffer.
/// @param p it
static void big_free(void *p) {
    free(p);
}
/// Resizes a big buffer.
/// @param p it
/// @param n its new size
/// @return it, or NULL
static void *big_realloc(void *p, size_t n) {
    return realloc(p, n);
}
#endif

/// LodePNG's allocators (LODEPNG_NO_COMPILE_ALLOCATORS).
/// @param size the size
/// @return the memory, or NULL
void *lodepng_malloc(size_t size) {
    return big_alloc(size);
}
/// LodePNG's realloc.
/// @param ptr the memory
/// @param new_size its new size
/// @return it, or NULL
void *lodepng_realloc(void *ptr, size_t new_size) {
    return big_realloc(ptr, new_size);
}
/// LodePNG's free.
/// @param ptr the memory
void lodepng_free(void *ptr) {
    big_free(ptr);
}

void image_free(image_t *img) {
    big_free(img->rgb);
    img->rgb = NULL;
}

/// Makes an empty RGB picture.
/// @param img the picture
/// @param w its width
/// @param h its height
/// @param err set to why it failed
/// @return made
static bool new_image(image_t *img, int w, int h, const char **err) {
    if (w <= 0 || h <= 0 || (unsigned)w * (unsigned)h > MAX_PIXELS) {
        *err = "picture too big";
        return false;
    }
    img->w = w;
    img->h = h;
    img->rgb = big_alloc((size_t)w * h * 3);
    if (!img->rgb) {
        *err = "not enough memory";
        return false;
    }
    return true;
}

/// Reads a whole file.
/// @param path the file
/// @param data set to its bytes (free with big_free)
/// @param len set to their count
/// @param err set to why it failed
/// @return read
static bool read_file(const char *path, uint8_t **data, size_t *len, const char **err) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        *err = "file not found";
        return false;
    }
    *len = f_size(&f);
    *data = big_alloc(*len ? *len : 1);
    UINT got = 0;
    bool ok = *data && f_read(&f, *data, *len, &got) == FR_OK && got == *len;
    f_close(&f);
    if (!ok) {
        *err = *data ? "can't read the file" : "not enough memory";
        big_free(*data);
        *data = NULL;
    }
    return ok;
}

// ---- BMP ----------------------------------------------------------------------

/// A little-endian 16-bit number.
/// @param p the bytes
/// @return it
static uint32_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}
/// A little-endian 32-bit number.
/// @param p the bytes
/// @return it
static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/// The value of the bits of `v` under `mask`, scaled to 0-255.
/// @param v the pixel
/// @param mask the channel's mask
/// @return the channel
static uint8_t mask_channel(uint32_t v, uint32_t mask) {
    if (!mask)
        return 0;
    int shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++shift;
    }
    uint32_t x = (v >> shift) & mask;
    return (uint8_t)(x * 255 / mask);
}

/// Decodes a BMP (1, 4, 8, 16, 24 or 32 bits, uncompressed or bitfields).
/// @param d the file
/// @param len its size
/// @param img the picture
/// @param err set to why it failed
/// @return decoded
static bool decode_bmp(const uint8_t *d, size_t len, image_t *img, const char **err) {
    *err = "not a BMP file this can read";
    if (len < 26 || d[0] != 'B' || d[1] != 'M')
        return false;
    uint32_t off = le32(d + 10), dib = le32(d + 14);
    int32_t w, h;
    int bpp;
    uint32_t comp = 0, colours = 0;
    if (dib == 12) {  // OS/2 BITMAPCOREHEADER
        w = (int16_t)le16(d + 18);
        h = (int16_t)le16(d + 20);
        bpp = le16(d + 24);
    } else if (dib >= 40 && len >= 54) {
        w = (int32_t)le32(d + 18);
        h = (int32_t)le32(d + 22);
        bpp = le16(d + 28);
        comp = le32(d + 30);
        colours = le32(d + 46);
    } else {
        return false;
    }
    bool top_down = h < 0;
    if (top_down)
        h = -h;
    if (comp != 0 && comp != 3) {
        *err = "compressed BMPs aren't supported";
        return false;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return false;
    // Colour masks for 16 and 32 bits.
    uint32_t rm, gm, bm;
    if (bpp == 16) {
        rm = 0x7c00, gm = 0x03e0, bm = 0x001f;
    } else {
        rm = 0xff0000, gm = 0x00ff00, bm = 0x0000ff;
    }
    if (comp == 3) {
        const uint8_t *m = d + 14 + 40;  // after the info header (v1) or inside it (v4/v5)
        if (dib > 40)
            m = d + 14 + 40;
        if ((size_t)(m - d) + 12 > len)
            return false;
        rm = le32(m), gm = le32(m + 4), bm = le32(m + 8);
    }
    // The palette.
    uint8_t pal[256][3];
    int entry = dib == 12 ? 3 : 4;
    if (bpp <= 8) {
        int n = colours ? (int)colours : 1 << bpp;
        if (n > 256)
            n = 256;
        const uint8_t *p = d + 14 + dib;
        if ((size_t)(p - d) + (size_t)n * entry > len)
            return false;
        memset(pal, 0, sizeof pal);
        for (int i = 0; i < n; ++i, p += entry) {
            pal[i][0] = p[2];
            pal[i][1] = p[1];
            pal[i][2] = p[0];
        }
    }
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4;
    if (w <= 0 || h <= 0 || off + stride * h > len)
        return false;
    if (!new_image(img, w, h, err))
        return false;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = d + off + stride * (top_down ? y : h - 1 - y);
        uint8_t *out = img->rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x, out += 3) {
            if (bpp <= 8) {
                int bit = x * bpp;
                int i = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1);
                memcpy(out, pal[i], 3);
            } else if (bpp == 24) {
                out[0] = row[x * 3 + 2];
                out[1] = row[x * 3 + 1];
                out[2] = row[x * 3];
            } else {
                uint32_t v = bpp == 16 ? le16(row + x * 2) : le32(row + x * 4);
                out[0] = mask_channel(v, rm);
                out[1] = mask_channel(v, gm);
                out[2] = mask_channel(v, bm);
            }
        }
    }
    return true;
}

// ---- PNG ----------------------------------------------------------------------

/// Decodes a PNG with LodePNG.
/// @param d the file
/// @param len its size
/// @param img the picture
/// @param err set to why it failed
/// @return decoded
static bool decode_png(const uint8_t *d, size_t len, image_t *img, const char **err) {
    unsigned w, h;
    LodePNGState state;
    lodepng_state_init(&state);
    unsigned e = lodepng_inspect(&w, &h, &state, d, len);
    lodepng_state_cleanup(&state);
    if (e) {
        *err = lodepng_error_text(e);
        return false;
    }
    if ((unsigned long)w * h > MAX_PIXELS) {
        *err = "picture too big";
        return false;
    }
    unsigned char *out = NULL;
    e = lodepng_decode_memory(&out, &w, &h, d, len, LCT_RGB, 8);
    if (e) {
        *err = e == 83 ? "not enough memory" : lodepng_error_text(e);
        return false;
    }
    img->w = w;
    img->h = h;
    img->rgb = out;
    return true;
}

// ---- JPEG ---------------------------------------------------------------------

/// TJpgDec's device: the file and the picture.
typedef struct {
    FIL f;  ///< the file
    image_t *img;  ///< the picture
} jpeg_io_t;

/// TJpgDec's input: reads (or skips) bytes.
/// @param jd the decoder
/// @param buf where (NULL: skip)
/// @param n how many
/// @return how many were read
static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n) {
    jpeg_io_t *io = jd->device;
    UINT got = 0;
    if (buf) {
        f_read(&io->f, buf, n, &got);
        return got;
    }
    return f_lseek(&io->f, f_tell(&io->f) + n) == FR_OK ? n : 0;
}

/// TJpgDec's output: a block of pixels into the picture.
/// @param jd the decoder
/// @param bitmap the RGB pixels
/// @param r where they go
/// @return 1 to go on
static int jpeg_out(JDEC *jd, void *bitmap, JRECT *r) {
    jpeg_io_t *io = jd->device;
    image_t *img = io->img;
    const uint8_t *src = bitmap;
    int bw = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; ++y, src += bw * 3) {
        if (y >= img->h)
            break;
        int n = bw;
        if (r->left + n > img->w)
            n = img->w - r->left;
        uint8_t *out = img->rgb + ((size_t)y * img->w + r->left) * 3;
        for (int x = 0; x < n; ++x, out += 3) {  // this TJpgDec gives B, G, R
            out[0] = src[x * 3 + 2];
            out[1] = src[x * 3 + 1];
            out[2] = src[x * 3];
        }
    }
    return 1;
}

/// Decodes a JPEG, scaled down (1/2, 1/4, 1/8) towards the size wanted.
/// @param path the file
/// @param want_w the width wanted
/// @param want_h the height wanted
/// @param img the picture
/// @param err set to why it failed
/// @return decoded
static bool decode_jpeg(const char *path, int want_w, int want_h, image_t *img,
                        const char **err) {
    jpeg_io_t io = {.img = img};
    if (f_open(&io.f, path, FA_READ) != FR_OK) {
        *err = "file not found";
        return false;
    }
    const size_t work_size = 4096;
    void *work = malloc(work_size);
    JDEC jd;
    JRESULT r = work ? jd_prepare(&jd, jpeg_in, work, work_size, &io) : JDR_MEM1;
    bool ok = false;
    if (r == JDR_OK) {
        // Decode at the smallest scale (1/1 .. 1/8) that still covers the
        // wanted size and fits in memory.
        int scale = 0;
        unsigned ww = want_w ? (unsigned)want_w : 65535, wh = want_h ? (unsigned)want_h : 65535;
        while (scale < 3 && (unsigned)(jd.width >> (scale + 1)) >= ww &&
               (unsigned)(jd.height >> (scale + 1)) >= wh)
            ++scale;
        while (scale < 3 &&
               (unsigned long)(jd.width >> scale) * (jd.height >> scale) > MAX_PIXELS)
            ++scale;
        int w = (jd.width + (1 << scale) - 1) >> scale;
        int h = (jd.height + (1 << scale) - 1) >> scale;
        if (new_image(img, w, h, err)) {
            r = jd_decomp(&jd, jpeg_out, scale);
            ok = r == JDR_OK;
            if (!ok)
                image_free(img);
        } else {
            r = JDR_OK;  // *err is set
        }
    }
    if (r != JDR_OK)
        *err = r == JDR_FMT3 ? "progressive JPEGs aren't supported"
               : r == JDR_MEM1 || r == JDR_MEM2 ? "not enough memory"
                                                : "not a JPEG file this can read";
    free(work);
    f_close(&io.f);
    return ok;
}

// ---- Loading ------------------------------------------------------------------

bool image_load(const char *path, int want_w, int want_h, image_t *img, const char **err) {
    img->rgb = NULL;
    FIL f;
    uint8_t magic[8] = {0};
    UINT got = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        *err = "file not found";
        return false;
    }
    f_read(&f, magic, sizeof magic, &got);
    f_close(&f);
    if (got >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
        return decode_jpeg(path, want_w, want_h, img, err);
    bool png = got >= 8 && !memcmp(magic, "\x89PNG\r\n\x1a\n", 8);
    bool bmp = got >= 2 && magic[0] == 'B' && magic[1] == 'M';
    if (!png && !bmp) {
        *err = "not a BMP, PNG or JPEG file";
        return false;
    }
    uint8_t *data;
    size_t len;
    if (!read_file(path, &data, &len, err))
        return false;
    bool ok = png ? decode_png(data, len, img, err) : decode_bmp(data, len, img, err);
    big_free(data);
    return ok;
}

// ---- Drawing --------------------------------------------------------------------

/// The ordered dither's 4x4 matrix.
static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/// One colour channel's level (0..steps) for the screen. A value that is
/// exactly one of the levels (as screenshots and RGB332 pictures have) is
/// that level; others are dithered: a fraction of a step (d / 4080) is added
/// before rounding down. Without the first part, a level's value just under
/// k * 255 / steps would come out a dot pattern of k and k - 1.
/// @param v the value (0-255)
/// @param steps the levels less one
/// @param d the dither (0-15 times 255)
/// @return the level
static inline int level(int v, int steps, int d) {
    int q = (v * steps + 127) / 255;
    if (q * 255 / steps == v)
        return q;
    q = (v * steps * 16 + d) / 4080;
    return q > steps ? steps : q;
}

void image_draw(const image_t *img, uint8_t *fb, int x, int y, int w, int h,
                const image_clip_t *clip) {
    if (!img->rgb || w <= 0 || h <= 0)
        return;
    image_clip_t c = clip ? *clip : (image_clip_t){0, 0, SCREEN_W, SCREEN_H};
    if (c.x0 < 0)
        c.x0 = 0;
    if (c.y0 < 0)
        c.y0 = 0;
    if (c.x1 > SCREEN_W)
        c.x1 = SCREEN_W;
    if (c.y1 > SCREEN_H)
        c.y1 = SCREEN_H;
    int x0 = x < c.x0 ? c.x0 : x, x1 = x + w > c.x1 ? c.x1 : x + w;
    int y0 = y < c.y0 ? c.y0 : y, y1 = y + h > c.y1 ? c.y1 : y + h;
    // Which picture column each screen column shows: 16.16 steps (a
    // multiply a pixel, no table: SRAM is short).
    int64_t step = ((int64_t)img->w << 16) / w;
    for (int dy = y0; dy < y1; ++dy) {
        int sy = (int)((long long)(dy - y) * img->h / h);
        const uint8_t *row = img->rgb + (size_t)sy * img->w * 3;
        uint8_t *out = fb + dy * SCREEN_W;
        const uint8_t *b = bayer[dy & 3];
        for (int dx = x0; dx < x1; ++dx) {
            int sx = (int)(((int64_t)(dx - x) * step) >> 16);
            const uint8_t *p = row + (sx < img->w ? sx : img->w - 1) * 3;
            // Ordered dithering: add a fraction of a step before rounding down
            // (v * 7 / 255 + d / 16, scaled by 4080 to stay in integers).
            int d = b[dx & 3] * 255;
            int r = level(p[0], 7, d), g = level(p[1], 7, d), bl = level(p[2], 3, d);
            out[dx] = (uint8_t)(r << 5 | g << 2 | bl);
        }
    }
}

void image_fit(const image_t *img, int *x, int *y, int *w, int *h) {
    int fw = *w, fh = *h;
    if ((long long)img->w * fh > (long long)img->h * fw) {  // wider than the box
        fh = (int)((long long)img->h * fw / img->w);
    } else {
        fw = (int)((long long)img->w * fh / img->h);
    }
    *x += (*w - fw) / 2;
    *y += (*h - fh) / 2;
    *w = fw ? fw : 1;
    *h = fh ? fh : 1;
}

// ---- Saving ---------------------------------------------------------------------

bool image_save_bmp(const char *path, const uint8_t *fb, const char **err) {
    uint8_t hdr[54 + 256 * 4];
    uint32_t pixels = 54 + 256 * 4, size = pixels + SCREEN_W * SCREEN_H;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
#define PUT32(o, v) \
    (hdr[o] = (uint8_t)(v), hdr[o + 1] = (uint8_t)((v) >> 8), hdr[o + 2] = (uint8_t)((v) >> 16), \
     hdr[o + 3] = (uint8_t)((v) >> 24))
    PUT32(2, size);
    PUT32(10, pixels);
    PUT32(14, 40);
    PUT32(18, SCREEN_W);
    PUT32(22, SCREEN_H);
    hdr[26] = 1;
    hdr[28] = 8;
    PUT32(34, SCREEN_W * SCREEN_H);
    PUT32(46, 256);
#undef PUT32
    for (int i = 0; i < 256; ++i) {  // the RGB332 colours
        uint8_t *e = hdr + 54 + i * 4;
        e[2] = (i >> 5) * 255 / 7;
        e[1] = ((i >> 2) & 7) * 255 / 7;
        e[0] = (i & 3) * 85;
    }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        *err = "can't create the file";
        return false;
    }
    UINT n;
    bool ok = f_write(&f, hdr, sizeof hdr, &n) == FR_OK && n == sizeof hdr;
    for (int y = SCREEN_H - 1; ok && y >= 0; --y)  // bottom row first
        ok = f_write(&f, fb + y * SCREEN_W, SCREEN_W, &n) == FR_OK && n == SCREEN_W;
    ok = f_close(&f) == FR_OK && ok;
    if (!ok)
        *err = "can't write the file (drive full?)";
    return ok;
}
