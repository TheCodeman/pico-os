/**
 * @file fonttest.c
 * @brief PC test of src/font/fnt.c: draws a sample page from each font given
 *        (shot_N.raw, 640x480 RGB332), times rasterizing and drawing, and
 *        feeds PFN files damaged at random to fnt_open and fnt_draw (ASan).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fnt.h"

bool pc_psram_full;  ///< the stub heap's switch (unused here)

static uint8_t fb[640 * 480];  ///< the picture drawn

/// @return microseconds
static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/// Reads a file.
/// @param path it
/// @param size set to its length
/// @return its bytes (malloc), or NULL
static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*size);
    if (fread(d, 1, *size, f) != *size) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

/// Draws the sample page.
/// @param f the font
static void page(fnt_t *f) {
    static const int std[] = {12, 16, 24, 32, 48};
    int sizes[16], n = fnt_sizes(f, sizes, 16);
    if (!n) {
        memcpy(sizes, std, sizeof std);
        n = 5;
    }
    memset(fb, 0, sizeof fb);
    int y = 8;
    for (int i = 0; i < n; ++i) {
        int a, d, g;
        fnt_vmetrics(f, sizes[i], &a, &d, &g);
        y += a;
        fnt_draw(f, sizes[i], fb, 640, 640, 480, 8, y, "The quick brown fox jumps over the lazy dog. 0123456789",
                 0xff);
        y += d + g + 2;
    }
    for (int x = 0; x < 640; ++x)
        for (int r = 0; r < 70; ++r)
            fb[(y + r) * 640 + x] = x < 320 ? 0x06 : (uint8_t)((x - 320) / 40 * 0x24 | (x - 320) / 40 >> 1);
    int a, d, g;
    fnt_vmetrics(f, 48, &a, &d, &g);
    fnt_draw(f, 48, fb, 640, 640, 480, 8, y + a + 4, "Pico 2 fonts: AV Wa To \xc3\xa9\xc3\xa8", 0x1f);
}

int main(int argc, char **argv) {
    int fails = 0;
    for (int i = 1; i < argc; ++i) {
        size_t size;
        uint8_t *d = slurp(argv[i], &size);
        const char *err = "?";
        fnt_t *f = d ? fnt_open(d, size, &err) : NULL;
        if (!f) {
            printf("%s: %s\n", argv[i], d ? err : "can't read");
            ++fails;
            continue;
        }
        page(f);
        char name[32];
        snprintf(name, sizeof name, "shot_%d.raw", i);
        FILE *o = fopen(name, "wb");
        fwrite(fb, 1, sizeof fb, o);
        fclose(o);
        // Times, as on the board.
        double t0 = now_us();
        for (int rep = 0; rep < 20; ++rep)
            for (int c = 33; c < 127; ++c)
                fnt_raster_one(f, 32, c);
        double r32 = (now_us() - t0) / (20 * 94);
        fnt_set_cache(f, false);
        t0 = now_us();
        for (int rep = 0; rep < 200; ++rep)
            fnt_draw(f, 16, fb, 640, 640, 480, 0, 100, "The quick brown fox jumps over the lazy dog.", 0xff);
        double nc = 200 * 44 / ((now_us() - t0) / 1e6);
        fnt_set_cache(f, true);
        t0 = now_us();
        for (int rep = 0; rep < 2000; ++rep)
            fnt_draw(f, 16, fb, 640, 640, 480, 0, 100, "The quick brown fox jumps over the lazy dog.", 0xff);
        double ca = 2000 * 44 / ((now_us() - t0) / 1e6);
        double s0 = now_us();
        for (int c = 33; c < 127; ++c)
            fnt_sdf_one(f, 32, c);
        double sdf = (now_us() - s0) / 94;
        fnt_stats_t st;
        fnt_get_stats(f, &st);
        printf("%s: kind %d, %d glyphs; PC: raster 32px %.1f us, text %.0f g/s, cached %.0f g/s, SDF %.0f us;"
               " arena peak %zu, spills %u, cache %zu B\n",
               argv[i], fnt_kind(f), fnt_glyphs(f), r32, nc, ca, fnt_kind(f) == FNT_PFN ? 0.0 : sdf, st.arena_peak,
               st.spills, st.cache_bytes);
        // A small cache budget: emptied as it goes, still draws.
        fnt_cache_budget(f, 2048);
        for (int px = 8; px < 120; px += 7)
            fnt_draw(f, px, fb, 640, 640, 480, -20, px, "Budget test: ABCDEFGHIJ", 0xe0);
        fnt_close(f);
        // Damaged PFN files.
        if (size > 8 && !memcmp(d, "PFN1", 4)) {
            srand(1);
            int opened = 0;
            for (int k = 0; k < 3000; ++k) {
                size_t n = k % 3 == 0 ? (size_t)rand() % size : size;
                uint8_t *b = malloc(n ? n : 1);
                memcpy(b, d, n);
                for (int j = 0; j < 1 + k % 8 && n; ++j)
                    b[(size_t)rand() % (k % 2 ? (n < 2048 ? n : 2048) : n)] = (uint8_t)rand();
                fnt_t *bf = fnt_open(b, n, &err);
                if (bf) {
                    ++opened;
                    fnt_draw(bf, 16, fb, 640, 640, 480, 0, 50, "Fuzz \xc3\xa9 AVWAY", 0xff);
                    fnt_draw(bf, 48, fb, 640, 640, 480, 0, 300, "Fuzz", 0xff);
                    fnt_close(bf);
                }
                free(b);
            }
            printf("  damaged PFN files: 3000 tried, %d opened and drawn\n", opened);
        }
        free(d);
    }
    return fails;
}
