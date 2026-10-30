/**
 * @file pngtest.c
 * @brief PC test of src/image/png_rows.c: each PNG given is decoded a row at
 *        a time and through LodePNG, at several sizes, and the two pictures
 *        must be the same; then damaged copies are decoded (ASan).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ff.h"
#include "image.h"
#include "image_rows.h"

// FatFs on stdio (image.c's file functions; not used by the checks).
FRESULT f_open(FIL *f, const char *path, int mode) { f->f = fopen(path, mode & FA_WRITE ? "wb" : "rb"); return f->f ? FR_OK : 1; }  ///< opens
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }  ///< reads
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }  ///< writes
FRESULT f_lseek(FIL *f, FSIZE_t at) { return fseek(f->f, (long)at, SEEK_SET) ? 1 : FR_OK; }  ///< moves
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }  ///< closes
FSIZE_t f_tell(FIL *f) { return (FSIZE_t)ftell(f->f); }  ///< where it is
FSIZE_t f_size(FIL *f) { long at = ftell(f->f); fseek(f->f, 0, SEEK_END); long n = ftell(f->f); fseek(f->f, at, SEEK_SET); return (FSIZE_t)n; }  ///< its size

/// Reads a file.
/// @param path it
/// @param n set to its size
/// @return its bytes (malloc)
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint8_t *d = malloc(*n);
    if (fread(d, 1, *n, f) != *n)
        *n = 0;
    fclose(f);
    return d;
}

/// @return seconds
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int fails = 0, rows = 0, fallback = 0;
    double t_rows = 0, t_lode = 0;
    for (int i = 1; i < argc; ++i) {
        size_t n;
        uint8_t *d = slurp(argv[i], &n);
        if (!d || !n) {
            printf("%s: can't read\n", argv[i]);
            return 1;
        }
        int iw = d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19], ih = d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23];
        const int sizes[][2] = {{iw, ih}, {iw / 2 + 1, ih / 2 + 1}, {iw * 3 / 2 + 1, ih * 5 / 4 + 1}, {7, 3}, {600, 450}};
        for (int k = 0; k < 5; ++k) {
            int w = sizes[k][0], h = sizes[k][1];
            uint8_t *a = malloc((size_t)w * h), *b = malloc((size_t)w * h);
            memset(a, 0x55, (size_t)w * h);
            const char *err = "";
            double t0 = now();
            int r = png_rows_decode(d, n, a, w, h, &err);
            t_rows += now() - t0;
            image_t img;
            t0 = now();
            bool ok = image_load_mem(d, n, 0, 0, &img, &err);
            if (ok) {
                image_draw_compact(&img, b, w, h);
                image_free(&img);
            }
            t_lode += now() - t0;
            if (r == 0) {
                fallback += k == 0;
            } else if (r < 0 || !ok || memcmp(a, b, (size_t)w * h)) {
                size_t at = 0;
                while (at < (size_t)w * h && a[at] == b[at])
                    ++at;
                printf("FAIL %s at %dx%d: rows %d (%s), lodepng %d, first difference at %zu\n", argv[i], w, h, r,
                       r < 0 ? err : "", ok, at);
                ++fails;
            } else {
                rows += k == 0;
            }
            free(a);
            free(b);
        }
        // Damaged copies.
        srand(i);
        for (int k = 0; k < 300; ++k) {
            size_t m = k % 4 == 0 ? (size_t)rand() % n : n;
            uint8_t *c = malloc(m ? m : 1);
            memcpy(c, d, m);
            for (int j = 0; j < 1 + k % 6 && m > 40; ++j)
                c[40 + (size_t)rand() % (m - 40)] = (uint8_t)rand();
            uint8_t px[64 * 48];
            const char *err;
            png_rows_decode(c, m, px, 64, 48, &err);
            free(c);
        }
        free(d);
    }
    printf("%d PNGs decoded a row at a time, %d left to LodePNG, %d failures; rows %.1f ms, LodePNG %.1f ms\n", rows,
           fallback, fails, t_rows * 1e3, t_lode * 1e3);
    return fails != 0;
}
