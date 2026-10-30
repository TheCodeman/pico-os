/**
 * @file font_app.c
 * @brief The `font` command (ttf too): shows a TrueType, OpenType or PFN font
 *        from the drive, anti-aliased, at several sizes, and times it.
 *
 * `font FILE [size] ["text"]` draws sample lines at 12 to 48 pixels (a PFN
 * file: at its sizes) and the text given at the size given; `font bench
 * FILE...` times loading, opening, finding and drawing glyphs, with and
 * without the glyph cache, and prints the figures (on the screen and the
 * debug UART).
 *
 * Memory, only while it runs: the font file, the state and the glyph cache
 * in PSRAM, the screen kept to be put back (PSRAM), and fnt.c's 16 KB arena
 * in the SRAM heap. The font's bytes live for a few seconds or minutes and
 * can be read again from the drive, so they get no refresh or check while in
 * use; a CRC taken when loaded is checked again when done, as a test.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "font.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "fnt.h"
#include "font8x16.h"
#include "mc.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height

#define FILE_MAX (6u << 20)  ///< the biggest font file loaded
#define MAX_FILES 4          ///< fonts one bench compares
#define BENCH_MS  500        ///< each drawing test runs this long

#define C_BG    0x00  ///< the page
#define C_STRIP 0x0b  ///< the title and status lines
#define C_LABEL 0xb6  ///< labels
#define C_TEXT  0xff  ///< sample text
#define C_YEL   0xfc  ///< the text given
#define C_BAND  0x06  ///< the coloured band
#define C_CYAN  0x1f  ///< text on it

/// The sample line.
static const char PANGRAM[] = "The quick brown fox jumps over the lazy dog. 0123456789";
/// And the letters and signs.
static const char LETTERS[] = "ABCDEFGHIJKLM abcdefghijklm !\"#$%&'()*+,-./:;<=>?@[\\]^_{|}~";

/// A loaded font.
typedef struct {
    char name[64];       ///< the file's name
    uint8_t *data;       ///< its bytes (PSRAM)
    size_t size;         ///< how many
    uint32_t crc;        ///< their CRC when loaded
    uint32_t load_us;    ///< reading them took
    uint32_t open_us;    ///< fnt_open (stbtt_InitFont) took
    fnt_t *f;            ///< the font
} font_file_t;

extern char __StackLimit;  ///< top of the heap, from the SDK's linker script

/// @return the SRAM heap's free bytes, as the mem command counts them
static long sram_free(void) {
    struct mallinfo mi = mallinfo();
    return (long)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks;
}

/// @return microseconds since boot
static uint32_t now_us(void) {
    return (uint32_t)time_us_64();
}

/// CRC-32 (the zip one), bit by bit: no table to keep.
/// @param p the bytes
/// @param n how many
/// @return the CRC
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k)
            c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

/// @param k the kind
/// @return its name
static const char *kind_name(fnt_kind_t k) {
    return k == FNT_TTF ? "TrueType (glyf, quadratic)" : k == FNT_CFF ? "OpenType CFF (cubic)" : "PFN (4-bit, pre-drawn)";
}

/// @param k the kind
/// @return a short name for it
static const char *kind_short(fnt_kind_t k) {
    return k == FNT_TTF ? "TTF" : k == FNT_CFF ? "OTF/CFF" : "PFN";
}

/// Reads a font file into PSRAM and opens it.
/// @param path the file
/// @param ff filled in
/// @return false (and says why) if it can't
static bool load(const char *path, font_file_t *ff) {
    memset(ff, 0, sizeof *ff);
    const char *base = strrchr(path, '/');
    snprintf(ff->name, sizeof ff->name, "%s", base ? base + 1 : path);
    FIL fl;
    if (f_open(&fl, path, FA_READ) != FR_OK) {
        printf("font: can't open %s\n", path);
        return false;
    }
    FSIZE_t size = f_size(&fl);
    if (size < 12 || size > FILE_MAX) {
        f_close(&fl);
        printf("font: %s is %s\n", path, size < 12 ? "too short" : "too big (6 MB at most)");
        return false;
    }
    ff->data = psram_malloc((size_t)size);
    if (!ff->data) {
        f_close(&fl);
        printf("font: not enough PSRAM for %s\n", path);
        return false;
    }
    uint32_t t0 = now_us();
    UINT got = 0;
    FRESULT r = f_read(&fl, ff->data, (UINT)size, &got);
    ff->load_us = now_us() - t0;
    f_close(&fl);
    if (r != FR_OK || got != size) {
        printf("font: reading %s failed\n", path);
        psram_free(ff->data);
        ff->data = NULL;
        return false;
    }
    ff->size = (size_t)size;
    ff->crc = crc32(ff->data, ff->size);
    const char *err = "?";
    t0 = now_us();
    ff->f = fnt_open(ff->data, ff->size, &err);
    ff->open_us = now_us() - t0;
    if (!ff->f) {
        printf("font: %s: %s\n", path, err);
        psram_free(ff->data);
        ff->data = NULL;
        return false;
    }
    return true;
}

/// Closes a font and frees its file.
/// @param ff the font
static void unload(font_file_t *ff) {
    fnt_close(ff->f);
    psram_free(ff->data);
    memset(ff, 0, sizeof *ff);
}

// ---- The screen --------------------------------------------------------------------------------------

/// Fills a rectangle.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    for (int r = y; r < y + h && r < H; ++r)
        if (r >= 0)
            memset(dvi_framebuf + r * W + x, c, (size_t)w);
}

/// Text in the 8x16 font.
/// @param x left
/// @param y top
/// @param s the text
/// @param fg its colour
/// @param bg the background
static void text8(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s && x + 8 <= W; ++s, x += 8)
        for (int r = 0; r < 16; ++r) {
            uint8_t bits = font8x16[(uint8_t)*s][r], *d = dvi_framebuf + (y + r) * W + x;
            for (int i = 0; i < 8; ++i)
                d[i] = bits & 0x80 >> i ? fg : bg;
        }
}

/// The viewer's state.
typedef struct {
    font_file_t *ff;      ///< the font
    int size;             ///< the size given (0: none)
    const char *text;     ///< the text given
    bool dither, kern;    ///< settings
    uint32_t draw_us;     ///< the last page took
    bool cold;            ///< with the cache emptied first
} view_t;

/// Draws the page: the sample lines and the text given.
/// @param v the state
static void page(view_t *v) {
    fnt_t *f = v->ff->f;
    static const int std_sizes[] = {12, 16, 24, 32, 48};
    int sizes[16], n;
    n = fnt_sizes(f, sizes, 16);
    if (!n) {
        memcpy(sizes, std_sizes, sizeof std_sizes);
        n = 5;
    }
    memset(dvi_framebuf, C_BG, W * H);
    fnt_set_dither(f, v->dither);
    fnt_set_kerning(f, v->kern);
    if (v->cold)
        fnt_cache_clear(f);
    uint32_t t0 = now_us();
    int y = 22;
    for (int i = 0; i < n && y < H - 40; ++i) {
        int a, d, g;
        fnt_vmetrics(f, sizes[i], &a, &d, &g);
        char lab[8];
        snprintf(lab, sizeof lab, "%d", sizes[i]);
        y += a;
        if (y + d > H - 20)
            break;
        text8(0, y - 12, lab, C_LABEL, C_BG);
        int x = fnt_draw(f, sizes[i], dvi_framebuf, W, W, H - 20, 28, y, PANGRAM, C_TEXT);
        (void)x;
        y += d + g + 2;
        if (sizes[i] <= 24 && y + a + d < H - 20) {  // the letters too, at the small sizes
            y += a;
            fnt_draw(f, sizes[i], dvi_framebuf, W, W, H - 20, 28, y, LETTERS, C_TEXT);
            y += d + g + 2;
        }
    }
    // The text given (or a greeting), over a coloured band and on black.
    int px = v->size ? fnt_size_used(f, v->size) : (n > 1 ? sizes[n - 1] : 48);
    const char *s = v->text ? v->text : "Pico 2 fonts: ffi & AV Wa To";
    int a, d, g;
    fnt_vmetrics(f, px, &a, &d, &g);
    if (y + a + d + 8 < H - 20) {
        fill(0, y, W / 2, a + d + 8, C_BAND);
        for (int r = 0; r < a + d + 8; ++r)  // a grey ramp beside it
            for (int c = W / 2; c < W; ++c) {
                int l = (c - W / 2) * 8 / (W / 2);
                dvi_framebuf[(y + r) * W + c] = (uint8_t)(l << 5 | l << 2 | l >> 1);
            }
        fnt_draw(f, px, dvi_framebuf, W, W, H - 20, 8, y + 4 + a, s, v->text ? C_YEL : C_CYAN);
    }
    v->draw_us = now_us() - t0;
    // The title and the status line.
    char buf[96];
    fill(0, 0, W, 18, C_STRIP);
    snprintf(buf, sizeof buf, "%.28s  %s  %u KB  %d glyphs", v->ff->name, kind_name(fnt_kind(f)),
             (unsigned)(v->ff->size >> 10), fnt_glyphs(f));
    text8(4, 1, buf, C_TEXT, C_STRIP);
    fnt_stats_t st;
    fnt_get_stats(f, &st);
    fill(0, H - 18, W, 18, C_STRIP);
    snprintf(buf, sizeof buf, "%s %lu.%lu ms  cache %lu KB  %s %s  R C D K  Esc", v->cold ? "drawn" : "cached",
             (unsigned long)(v->draw_us / 1000), (unsigned long)(v->draw_us / 100 % 10),
             (unsigned long)(st.cache_bytes >> 10), v->dither ? "dither" : "plain", v->kern ? "kern" : "nokern");
    text8(4, H - 17, buf, C_LABEL, C_STRIP);
}

/// Shows a font until a key other than R, C, D or K.
/// @param ff the font
/// @param size the size given (0: none)
/// @param s the text given (NULL: none)
static void view(font_file_t *ff, int size, const char *s) {
    uint8_t *saved = psram_malloc(W * H);
    if (!saved) {
        printf("font: not enough PSRAM\n");
        return;
    }
    memcpy(saved, dvi_framebuf, W * H);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    view_t v = {ff, size, s, true, true, 0, true};
    page(&v);
    for (;;) {
        int k = key_poll();
        if (k == KEY_NONE) {
            sleep_ms(10);
            continue;
        }
        if (k >= 'A' && k <= 'Z')
            k += 32;
        if (k == 'r')
            v.cold = false;  // again, from the cache
        else if (k == 'c')
            v.cold = true;  // again, every glyph drawn afresh
        else if (k == 'd')
            v.dither = !v.dither, v.cold = false;
        else if (k == 'k')
            v.kern = !v.kern, v.cold = false;
        else
            break;
        page(&v);
    }
    memcpy(dvi_framebuf, saved, W * H);
    psram_free(saved);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
}

// ---- The benchmark -------------------------------------------------------------------------------------

/// One font's figures.
typedef struct {
    int sz[2];             ///< the two sizes timed
    float find_us;         ///< finding a glyph (cmap)
    float first_us[2];     ///< drawing a glyph from its outline, the first time through
    float raster_us[2];    ///< and on average over 5 passes
    float sdf_us;          ///< a signed distance field glyph at 32 px (0: not done)
    float nocache_gps[2];  ///< glyphs a second drawing text without the cache
    float cache_gps[2];    ///< and from it
    size_t cache_bytes;    ///< the cache's pixels after the tests
    size_t arena_peak;     ///< the arena's most
    uint32_t spills;       ///< working buffers from PSRAM
    bool crc_ok;           ///< the file's bytes unchanged at the end
} bench_t;

/// Draws lines of text for BENCH_MS and counts the glyphs.
/// @param f the font
/// @param px the size
/// @param y0 the band of the screen used
/// @return glyphs a second
static float draw_rate(fnt_t *f, int px, int y0) {
    int a, d, g;
    fnt_vmetrics(f, px, &a, &d, &g);
    int n = (int)strlen(PANGRAM), glyphs = 0;
    uint32_t t0 = now_us(), t;
    do {
        fill(0, y0, W, 120, C_BG);
        for (int y = y0 + a; y + d <= y0 + 120; y += a + d) {
            fnt_draw(f, px, dvi_framebuf, W, W, H, 4, y, PANGRAM, C_TEXT);
            glyphs += n;
        }
        t = now_us() - t0;
    } while (t < BENCH_MS * 1000u);
    return glyphs * 1e6f / (float)t;
}

/// Times one font, drawing on the screen as it goes.
/// @param ff the font
/// @param b filled in
/// @param y0 the band of the screen to draw in
static void bench_one(font_file_t *ff, bench_t *b, int y0) {
    fnt_t *f = ff->f;
    bool outline = fnt_kind(f) != FNT_PFN;
    memset(b, 0, sizeof *b);
    char buf[96];
    snprintf(buf, sizeof buf, "%.40s: timing...", ff->name);
    fill(0, y0 - 18, W, 18, C_STRIP);
    text8(4, y0 - 17, buf, C_TEXT, C_STRIP);
    uint32_t t0 = now_us();
    for (int rep = 0; rep < 10; ++rep)
        for (int c = 32; c < 127; ++c)
            fnt_find(f, c);
    b->find_us = (now_us() - t0) / 950.0f;
    for (int s = 0; s < 2; ++s) {
        int px = b->sz[s] = fnt_size_used(f, s ? 32 : 16);
        if (outline) {
            for (int pass = 0; pass < 6; ++pass) {
                t0 = now_us();
                for (int c = 33; c < 127; ++c)
                    fnt_raster_one(f, px, c);
                float us = (now_us() - t0) / 94.0f;
                if (pass == 0)
                    b->first_us[s] = us;
                else
                    b->raster_us[s] += us / 5;
            }
            fnt_set_cache(f, false);
            b->nocache_gps[s] = draw_rate(f, px, y0);
            fnt_set_cache(f, true);
        }
        fnt_cache_clear(f);
        fnt_width(f, px, PANGRAM);  // warm the cache
        b->cache_gps[s] = draw_rate(f, px, y0);
        if (!outline)
            b->nocache_gps[s] = b->cache_gps[s];  // PFN glyphs are always ready
    }
    if (outline) {
        t0 = now_us();
        for (int c = 33; c < 127; ++c)
            fnt_sdf_one(f, 32, c);
        b->sdf_us = (now_us() - t0) / 94.0f;
    }
    fnt_stats_t st;
    fnt_get_stats(f, &st);
    b->cache_bytes = st.cache_bytes;
    b->arena_peak = st.arena_peak;
    b->spills = st.spills;
    b->crc_ok = crc32(ff->data, ff->size) == ff->crc;
}

/// Prints one font's figures.
/// @param ff the font
/// @param b its figures
static void bench_print(const font_file_t *ff, const bench_t *b) {
    bool outline = fnt_kind(ff->f) != FNT_PFN;
    printf("\n%s: %s, %u bytes, %d glyphs\n", ff->name, kind_name(fnt_kind(ff->f)), (unsigned)ff->size,
           fnt_glyphs(ff->f));
    printf("  load %lu ms (%lu KB/s)   open %lu us   find a glyph %.1f us\n", (unsigned long)(ff->load_us / 1000),
           (unsigned long)(ff->load_us ? (uint64_t)ff->size * 1000000 / ff->load_us / 1024 : 0),
           (unsigned long)ff->open_us, (double)b->find_us);
    for (int s = 0; s < 2; ++s) {
        if (outline)
            printf("  %2d px: raster %.0f us/glyph (first pass %.0f)   text %.0f glyphs/s, cached %.0f glyphs/s\n",
                   b->sz[s], (double)b->raster_us[s], (double)b->first_us[s], (double)b->nocache_gps[s],
                   (double)b->cache_gps[s]);
        else
            printf("  %2d px: no rasterizing   text %.0f glyphs/s\n", b->sz[s], (double)b->cache_gps[s]);
    }
    if (outline)
        printf("  SDF 32 px %.0f us/glyph   cache %u KB   SRAM arena %u KB, used up to %u bytes, %lu spills\n",
               (double)b->sdf_us, (unsigned)(b->cache_bytes >> 10), FNT_ARENA >> 10, (unsigned)b->arena_peak,
               (unsigned long)b->spills);
    printf("  PSRAM: file %u KB + cache %u KB   font data %s\n", (unsigned)(ff->size >> 10),
           (unsigned)(b->cache_bytes >> 10), b->crc_ok ? "intact (CRC same)" : "CHANGED (CRC differs)");
}

/// font bench FILE...
/// @param argc files + 2
/// @param argv the files from argv[2]
static void bench(int argc, char **argv) {
    font_file_t ff[MAX_FILES];
    bench_t b[MAX_FILES];
    int n = 0;
    long sram0 = sram_free();
    size_t ps0 = psram_free_total();
    for (int i = 2; i < argc && n < MAX_FILES; ++i)
        if (load(argv[i], &ff[n]))
            ++n;
    if (!n)
        return;
    long sram1 = sram_free();
    size_t ps1 = psram_free_total();
    uint8_t *saved = psram_malloc(W * H);
    if (!saved) {
        printf("font: not enough PSRAM\n");
        for (int i = 0; i < n; ++i)
            unload(&ff[i]);
        return;
    }
    memcpy(saved, dvi_framebuf, W * H);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    memset(dvi_framebuf, C_BG, W * H);
    for (int i = 0; i < n; ++i)
        bench_one(&ff[i], &b[i], 40 + (i % 3) * 145);
    size_t ps2 = psram_free_total();
    memcpy(dvi_framebuf, saved, W * H);
    psram_free(saved);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    printf("font bench: %lu MHz, glyphs ASCII 33-126, text \"%.20s...\"\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000), PANGRAM);
    for (int i = 0; i < n; ++i)
        bench_print(&ff[i], &b[i]);
    printf("\n%-8s %8s %7s %9s %9s %10s %10s %8s\n", "format", "load ms", "open us", "16px us", "32px us",
           "16px g/s", "32px g/s", "cached");
    for (int i = 0; i < n; ++i) {
        bool outline = fnt_kind(ff[i].f) != FNT_PFN;
        printf("%-8s %8lu %7lu %9.0f %9.0f %10.0f %10.0f %8.0f\n", kind_short(fnt_kind(ff[i].f)),
               (unsigned long)(ff[i].load_us / 1000), (unsigned long)ff[i].open_us,
               outline ? (double)b[i].raster_us[0] : 0.0, outline ? (double)b[i].raster_us[1] : 0.0,
               (double)b[i].nocache_gps[0], (double)b[i].nocache_gps[1], (double)b[i].cache_gps[1]);
    }
    printf("\nmemory while open: SRAM heap %ld KB (the arenas), PSRAM %lu KB (files, state, caches)\n",
           (sram0 - sram1) / 1024, (unsigned long)((ps0 - ps2) >> 10));
    (void)ps1;
    for (int i = 0; i < n; ++i)
        unload(&ff[i]);
}

void font_command(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        printf("usage: font FILE [size] [\"text\"]   show a .ttf, .otf or .pfn font (Esc: back)\n"
               "       font bench FILE...         time loading and drawing them (up to 4)\n");
        return;
    }
    if (!strcmp(argv[1], "bench")) {
        if (argc < 3)
            printf("usage: font bench FILE...\n");
        else
            bench(argc, argv);
        return;
    }
    int size = argc > 2 ? atoi(argv[2]) : 0;
    if (size < 0 || size > 200) {
        printf("font: sizes go from 1 to 200 pixels\n");
        return;
    }
    font_file_t ff;
    if (!load(argv[1], &ff))
        return;
    view(&ff, size, argc > 3 ? argv[3] : NULL);
    printf("%s: %s, loaded in %lu ms, opened in %lu us\n", ff.name, kind_name(fnt_kind(ff.f)),
           (unsigned long)(ff.load_us / 1000), (unsigned long)ff.open_us);
    unload(&ff);
}
