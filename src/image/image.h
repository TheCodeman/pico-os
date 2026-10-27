/**
 * @file image.h
 * @brief Pictures on the 640x480 RGB332 screen: loading BMP, PNG and JPEG files from the drive, drawing them scaled and dithered, and saving the screen as a BMP.
 *
 * Decoded pictures live in PSRAM (ordinary heap if there is none).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// A decoded picture in PSRAM, as RGB888 or RGB332.
typedef struct {
    int w, h;  ///< width and height in pixels
    uint8_t *rgb;  ///< w * h * 3 bytes, top row first
} image_t;

/// Loads `path`: BMP (1, 4, 8, 16, 24 or 32 bits, uncompressed), PNG, or
/// baseline JPEG. A big JPEG is decoded at 1/2, 1/4 or 1/8 size if that still
/// gives at least want_w x want_h pixels (0 = full size). On failure returns
/// false with *err set to why.
bool image_load(const char *path, int want_w, int want_h, image_t *img, const char **err);
/// Frees the picture's pixels.
void image_free(image_t *img);

/// A clip rectangle for drawing pictures.
typedef struct {
    int x0, y0, x1, y1;  ///< x0 <= x < x1, y0 <= y < y1
} image_clip_t;

/// Draws `img` into the screen rectangle (x, y, w, h) of `fb` (640x480,
/// RGB332), scaled to fill it, with ordered dithering, clipped to `clip` (NULL:
/// the whole screen).
void image_draw(const image_t *img, uint8_t *fb, int x, int y, int w, int h,
                const image_clip_t *clip);

/// The largest rectangle with img's shape that fits in w x h, centred in it.
void image_fit(const image_t *img, int *x, int *y, int *w, int *h);

/// Saves the 640x480 RGB332 screen `fb` as an 8-bit BMP.
bool image_save_bmp(const char *path, const uint8_t *fb, const char **err);
