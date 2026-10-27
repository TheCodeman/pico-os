/**
 * @file screenshot.c
 * @brief Screenshots: see screenshot.h.
 *
 * The box is drawn in pixels over whatever is on the screen (text or
 * graphics) and the screen is put back from the copy afterwards. PNGs are
 * 8-bit with the RGB332 palette, deflated with runs of the same pixel and
 * repeats of the row above (fixed Huffman codes).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

/// The screen's width.
#define W DVI_WIDTH
/// Its height.
#define H DVI_HEIGHT
/// Where screenshots go.
#define FOLDER "/SCREENSHOTS"

/// A screenshot was asked for (Print Screen).
static volatile bool wanted;

void screenshot_request(void) {
    wanted = true;
}

// ---- Names ---------------------------------------------------------------------------------------

void screenshot_next_name(char *out, int size) {
    f_mkdir(FOLDER);
    for (int n = 1; n < 10000; ++n) {
        snprintf(out, size, FOLDER "/SHOT%04d.PNG", n);
        FILINFO fi;
        char bmp[40];
        snprintf(bmp, sizeof bmp, FOLDER "/SHOT%04d.BMP", n);
        if (f_stat(out, &fi) != FR_OK && f_stat(bmp, &fi) != FR_OK)
            return;
    }
}

// ---- PNG -----------------------------------------------------------------------------------------

/// Updates a PNG CRC-32.
/// @param c the CRC so far
/// @param p the bytes
/// @param n their count
/// @return the CRC
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n) {
    // Bit by bit (no 1 KB table in SRAM; PNGs are small, so it is quick enough).
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c;
}

/// The picture, a row at a time: 640 RGB332 bytes for row y.
typedef const uint8_t *(*rows_t)(const void *ctx, int y);

/// A buffer's rows.
/// @param ctx the buffer
/// @param y the row
/// @return its pixels
static const uint8_t *rows_of_buffer(const void *ctx, int y) {
    return (const uint8_t *)ctx + y * W;
}

/// The screen as shown: line-doubled modes show each framebuffer row twice,
/// dvi_double_top rows down, black around.
/// @param ctx unused
/// @param y the row
/// @return its pixels
static const uint8_t *rows_of_screen(const void *ctx, int y) {
    static const uint8_t black[W];
    (void)ctx;
    if (!dvi_line_double)
        return dvi_framebuf + y * W;
    int r = y / 2 - dvi_double_top;
    return r >= 0 && r < dvi_double_rows ? dvi_framebuf + r * W : black;
}

/// Deflate output, written to the file as IDAT chunks of a small buffer
/// (nothing of the picture passes through PSRAM).
typedef struct {
    FIL *f;  ///< the file
    uint8_t buf[2048];  ///< the chunk being filled
    size_t len;  ///< its length
    uint32_t bits;  ///< bits not yet a byte
    int nbits;  ///< their count
    bool ok;  ///< no write has failed
} bitout_t;

static bool write_chunk(FIL *f, const char *type, const uint8_t *data, uint32_t len);

/// Writes the buffer as an IDAT chunk.
/// @param o the output
static void flush_out(bitout_t *o) {
    if (o->len && o->ok)
        o->ok = write_chunk(o->f, "IDAT", o->buf, o->len);
    o->len = 0;
}

/// Adds n bits, low first.
/// @param o the output
/// @param v the bits
/// @param n their count
static void put_bits(bitout_t *o, uint32_t v, int n) {  // LSB first
    o->bits |= v << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        o->buf[o->len++] = (uint8_t)o->bits;
        if (o->len == sizeof o->buf)
            flush_out(o);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

/// Adds a Huffman code, high bit first.
/// @param o the output
/// @param code the code
/// @param n its length
static void put_huff(bitout_t *o, uint32_t code, int n) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < n; ++i)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(o, r, n);
}

/// Adds a literal or length symbol with the fixed codes.
/// @param o the output
/// @param s the symbol (0-285)
static void put_symbol(bitout_t *o, int s) {  // the fixed literal/length code
    if (s < 144)
        put_huff(o, 0x30 + s, 8);
    else if (s < 256)
        put_huff(o, 0x190 + s - 144, 9);
    else if (s < 280)
        put_huff(o, s - 256, 7);
    else
        put_huff(o, 0xc0 + s - 280, 8);
}

/// Adds a match: its length and distance codes.
/// @param o the output
/// @param len the length (3-258)
/// @param dist the distance
static void put_match(bitout_t *o, int len, int dist) {
    static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                       33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    int l = 28;
    while (lbase[l] > len)
        --l;
    put_symbol(o, 257 + l);
    put_bits(o, len - lbase[l], lextra[l]);
    int d = 29;
    while (dbase[d] > dist)
        --d;
    put_huff(o, d, 5);
    put_bits(o, dist - dbase[d], d < 4 ? 0 : (d - 2) / 2);
}

/// The byte at position i of the rows as PNG stores them: each row a filter
/// byte (0) then its pixels.
/// @param rows the rows
/// @param ctx their context
/// @param i the position
/// @return the byte
static inline uint8_t raw_at(rows_t rows, const void *ctx, size_t i) {
    size_t x = i % (W + 1);
    return x ? rows(ctx, (int)(i / (W + 1)))[x - 1] : 0;
}

/// zlib stream of the picture: runs of a pixel, the cell before, and the row
/// above, with the fixed Huffman codes.
/// @param rows the rows
/// @param ctx their context
/// @param o the output
static void deflate_picture(rows_t rows, const void *ctx, bitout_t *o) {
    const size_t total = (size_t)(W + 1) * H;
    uint32_t a = 1, b = 0;  // Adler-32
    put_bits(o, 0x78, 8);
    put_bits(o, 0x01, 8);
    put_bits(o, 1, 1);  // the last block
    put_bits(o, 1, 2);  // fixed codes
    for (size_t i = 0; i < total && o->ok;) {
        int best = 0, best_d = 0;
        static const int dists[3] = {1, 8, W + 1};
        for (int k = 0; k < 3; ++k) {
            size_t d = dists[k];
            if (i < d)
                continue;
            int n = 0;
            while (n < 258 && i + n < total && raw_at(rows, ctx, i + n) == raw_at(rows, ctx, i + n - d))
                ++n;
            if (n > best) {
                best = n;
                best_d = d;
            }
        }
        int take = best >= 3 ? best : 1;
        for (int k = 0; k < take; ++k) {
            a = (a + raw_at(rows, ctx, i + k)) % 65521;
            b = (b + a) % 65521;
        }
        if (best >= 3)
            put_match(o, best, best_d);
        else
            put_symbol(o, raw_at(rows, ctx, i));
        i += take;
    }
    put_symbol(o, 256);
    if (o->nbits)
        put_bits(o, 0, 8 - o->nbits);
    uint32_t adler = b << 16 | a;
    for (int k = 3; k >= 0; --k)
        put_bits(o, adler >> (8 * k) & 0xff, 8);
    flush_out(o);
}

/// Writes a PNG chunk: length, type, data and CRC.
/// @param f the file
/// @param type the type
/// @param data the data
/// @param len its length
/// @return written
static bool write_chunk(FIL *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t head[8] = {len >> 24, len >> 16, len >> 8, len, type[0], type[1], type[2], type[3]};
    uint32_t c = crc_update(0xffffffffu, head + 4, 4);
    c = crc_update(c, data, len) ^ 0xffffffffu;
    uint8_t tail[4] = {c >> 24, c >> 16, c >> 8, c};
    UINT n;
    bool ok = f_write(f, head, 8, &n) == FR_OK && n == 8;
    if (len)
        ok = ok && f_write(f, data, len, &n) == FR_OK && n == len;
    return ok && f_write(f, tail, 4, &n) == FR_OK && n == 4;
}

/// Writes a PNG: 8-bit, the RGB332 palette.
/// @param f the file
/// @param rows the rows
/// @param ctx their context
/// @return written
static bool save_png(FIL *f, rows_t rows, const void *ctx) {
    uint8_t ihdr[13] = {0, 0, W >> 8, W & 0xff, 0, 0, H >> 8, H & 0xff, 8, 3, 0, 0, 0};
    uint8_t plte[768];
    for (int i = 0; i < 256; ++i) {
        plte[i * 3] = (i >> 5) * 255 / 7;
        plte[i * 3 + 1] = ((i >> 2) & 7) * 255 / 7;
        plte[i * 3 + 2] = (i & 3) * 85;
    }
    UINT n;
    if (!(f_write(f, "\x89PNG\r\n\x1a\n", 8, &n) == FR_OK && n == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
          write_chunk(f, "PLTE", plte, 768)))
        return false;
    bitout_t *o = malloc(sizeof *o);  // 2 KB, for a moment
    if (!o)
        return false;
    memset(o, 0, sizeof *o);
    o->f = f;
    o->ok = true;
    deflate_picture(rows, ctx, o);
    bool ok = o->ok;
    free(o);
    return ok && write_chunk(f, "IEND", NULL, 0);
}

/// Writes a BMP: 8-bit, the RGB332 palette.
/// @param f the file
/// @param rows the rows
/// @param ctx their context
/// @return written
static bool save_bmp(FIL *f, rows_t rows, const void *ctx) {
    // 8-bit with the RGB332 palette, bottom row first.
    uint8_t hdr[54 + 256 * 4];
    uint32_t pixels = sizeof hdr, size = pixels + W * H;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    uint32_t v[] = {2, size, 10, pixels, 14, 40, 18, W, 22, H, 34, W * H, 46, 256};
    for (unsigned k = 0; k < sizeof v / sizeof *v; k += 2)
        for (int j = 0; j < 4; ++j)
            hdr[v[k] + j] = (uint8_t)(v[k + 1] >> (8 * j));
    hdr[26] = 1;
    hdr[28] = 8;
    for (int i = 0; i < 256; ++i) {
        uint8_t *e = hdr + 54 + i * 4;
        e[2] = (i >> 5) * 255 / 7;
        e[1] = ((i >> 2) & 7) * 255 / 7;
        e[0] = (i & 3) * 85;
    }
    UINT n;
    bool ok = f_write(f, hdr, sizeof hdr, &n) == FR_OK && n == sizeof hdr;
    for (int y = H - 1; ok && y >= 0; --y)
        ok = f_write(f, rows(ctx, y), W, &n) == FR_OK && n == W;
    return ok;
}

/// Saves a picture, PNG or BMP by the name's extension.
/// @param path the file
/// @param rows the rows
/// @param ctx their context
/// @param err set to why it failed
/// @return saved
static bool save_rows(const char *path, rows_t rows, const void *ctx, const char **err) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        *err = "can't create the file";
        return false;
    }
    const char *dot = strrchr(path, '.');
    bool ok = dot && !strcasecmp(dot, ".bmp") ? save_bmp(&f, rows, ctx) : save_png(&f, rows, ctx);
    ok = f_close(&f) == FR_OK && ok;
    if (!ok)
        *err = "can't write the file (drive full?)";
    return ok;
}

bool screenshot_save(const char *path, const uint8_t *pixels, const char **err) {
    return pixels ? save_rows(path, rows_of_buffer, pixels, err)
                  : save_rows(path, rows_of_screen, NULL, err);
}

void screenshot_capture(uint8_t *out) {
    for (int y = 0; y < H; ++y)
        memcpy(out + y * W, rows_of_screen(NULL, y), W);
}

// ---- The box -------------------------------------------------------------------------------------

/// The box's width in characters.
#define BOX_COLS 56
#define C_BOX   0xb6  ///< light grey
#define C_INK   0x00  ///< black
#define C_TITLE 0x03  ///< blue
#define C_EDIT  0xff  ///< white
#define C_GOOD  0x10  ///< green
#define C_BAD   0xa0  ///< red

/// The box's top left.
///@{
static int box_x, box_y;
///@}

/// Draws a character in the box.
/// @param col its column
/// @param line its line
/// @param ch the character
/// @param fg its colour
/// @param bg the background
static void glyph(int col, int line, uint8_t ch, uint8_t fg, uint8_t bg) {
    int x = box_x + col * 8, y = box_y + line * 16;
    if (x < 0 || y < 0 || x + 8 > W || y + 16 > H)
        return;
    const uint8_t *g = font8x16[ch];
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 8; ++c)
            dvi_framebuf[(y + r) * W + x + c] = g[r] & (0x80 >> c) ? fg : bg;
}

/// Draws text in the box, padded with spaces.
/// @param col its column
/// @param line its line
/// @param width the width
/// @param s the text
/// @param fg its colour
/// @param bg the background
static void text(int col, int line, int width, const char *s, uint8_t fg, uint8_t bg) {
    for (int i = 0; i < width; ++i)
        glyph(col + i, line, *s ? (uint8_t)*s++ : ' ', fg, bg);
}

/// Draws the box: the file name being typed and a status line.
/// @param name the name
/// @param cursor the cursor in it
/// @param status the status
/// @param status_fg its colour
static void draw_box(const char *name, int cursor, const char *status, uint8_t status_fg) {
    static const char *title = " Screenshot ";
    for (int l = 0; l < 6; ++l)
        for (int c = 0; c < BOX_COLS; ++c) {
            uint8_t ch = ' ';
            if (l == 0 || l == 5)
                ch = c == 0 ? (l ? 0xc8 : 0xc9) : c == BOX_COLS - 1 ? (l ? 0xbc : 0xbb) : 0xcd;
            else if (c == 0 || c == BOX_COLS - 1)
                ch = 0xba;
            glyph(c, l, ch, C_INK, C_BOX);
        }
    text((BOX_COLS - (int)strlen(title)) / 2, 0, strlen(title), title, C_TITLE, C_BOX);
    text(2, 1, BOX_COLS - 4, "Save as (.PNG or .BMP):", C_INK, C_BOX);
    // The name, scrolled to show the cursor.
    int w = BOX_COLS - 4, len = strlen(name), off = cursor > w - 1 ? cursor - (w - 1) : 0;
    for (int i = 0; i < w; ++i) {
        int k = off + i;
        uint8_t ch = k < len ? (uint8_t)name[k] : ' ';
        bool at = k == cursor && cursor >= 0;
        glyph(2 + i, 2, ch, at ? C_EDIT : C_INK, at ? C_INK : C_EDIT);
    }
    text(2, 4, BOX_COLS - 4, status, status_fg, C_BOX);
}

/// Waits for a key.
/// @param getc where keys come from
/// @param us how long
/// @return the key, or -1
static int key_wait(int (*getc)(void), uint32_t us) {
    absolute_time_t until = make_timeout_time_us(us);
    do {
        int c = getc();
        if (c >= 0)
            return c;
        sleep_ms(2);
    } while (!time_reached(until));
    return -1;
}

/// The pixels under the box, kept in PSRAM while it is up. Copies are
/// checked: this board's PSRAM has been seen to drop bits under load.
static uint8_t *under;

/// Saves the picture under the box (or puts it back), until the frame stops changing.
/// @param save save (else put back)
static void copy_box(bool save) {
    int rows = 6 * 16, w = BOX_COLS * 8;
    for (int tries = 0; tries < 4; ++tries) {
        bool same = true;
        for (int r = 0; r < rows; ++r) {
            uint8_t *fb = dvi_framebuf + (box_y + r) * W + box_x, *u = under + r * w;
            if (save)
                memcpy(u, fb, w);
            else
                memcpy(fb, u, w);
            same = same && !memcmp(u, fb, w);
        }
        if (same)
            return;
    }
}

void screenshot_poll(int (*getc)(void)) {
    if (!wanted)
        return;
    wanted = false;
    int rows = dvi_line_double ? dvi_double_rows : H;
    box_x = (W - BOX_COLS * 8) / 2;
    box_y = rows / 2 - 48 > 0 ? rows / 2 - 48 : 0;
    under = psram_malloc(BOX_COLS * 8 * 6 * 16);
    if (!under)
        return;  // no memory: no screenshot
    // What is on the screen, without the mouse's arrow; the console's cursor
    // and pointer are kept still while the box is up. The picture is saved
    // from the screen itself, with the box taken away first.
    console_freeze(true);
    mouse_pointer_hide();
    copy_box(true);

    char name[96];
    screenshot_next_name(name, sizeof name);
    int cur = strlen(name);
    const char *status = "Enter saves, Esc cancels.";
    uint8_t status_fg = C_INK;
    for (;;) {
        draw_box(name, cur, status, status_fg);
        int c = key_wait(getc, 60000000);
        if (c < 0 || c == 3)
            break;  // nothing for a minute, or Ctrl-C
        if (c == 0x1b) {
            int n = key_wait(getc, 30000);
            if (n < 0)
                break;  // Esc alone
            if (n == '[' || n == 'O') {
                int k = key_wait(getc, 30000);
                while (k >= '0' && k <= '9')
                    k = key_wait(getc, 30000);
                if (k == 'D' && cur > 0)
                    --cur;
                else if (k == 'C' && name[cur])
                    ++cur;
                else if (k == 'H')
                    cur = 0;
                else if (k == 'F')
                    cur = strlen(name);
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            const char *err = NULL;
            copy_box(false);  // the screen as it was, to save
            bool ok = name[0] && screenshot_save(name, NULL, &err);
            static char done[80];
            if (ok)
                snprintf(done, sizeof done, "Saved %s", name);
            else
                snprintf(done, sizeof done, "Not saved: %s", err ? err : "no name");
            draw_box(name, -1, done, ok ? C_GOOD : C_BAD);
            sleep_ms(ok ? 900 : 2000);
            if (ok)
                break;
            continue;
        }
        if ((c == 8 || c == 0x7f) && cur > 0) {
            memmove(name + cur - 1, name + cur, strlen(name) - cur + 1);
            --cur;
        } else if (c >= 0x20 && c < 0x7f && strlen(name) < sizeof name - 1) {
            memmove(name + cur + 1, name + cur, strlen(name) - cur + 1);
            name[cur++] = (char)c;
        } else if (c == 0x15) {  // Ctrl-U: clear
            name[0] = 0;
            cur = 0;
        }
    }
    copy_box(false);
    console_freeze(false);
    mouse_pointer_show();
    psram_free(under);
    under = NULL;
}
