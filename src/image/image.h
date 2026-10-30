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
    /// width and height in pixels
    ///@{
    int w, h;
    ///@}
    uint8_t *rgb;  ///< w * h * 3 bytes, top row first
} image_t;

/// Loads `path`: BMP (1, 4, 8, 16, 24 or 32 bits, uncompressed), PNG, or
/// baseline JPEG. A big JPEG is decoded at 1/2, 1/4 or 1/8 size if that still
/// gives at least want_w x want_h pixels (0 = full size). On failure returns
/// false with *err set to why.
/// @param path the file
/// @param want_w the width needed (0: full size); a big JPEG is decoded smaller if that still gives it
/// @param want_h the height needed (0: full size)
/// @param img filled in
/// @param err set to why, on failure
/// @return true if it loaded
bool image_load(const char *path, int want_w, int want_h, image_t *img, const char **err);
/// The same as image_load, from bytes in memory (e.g. a download).
/// @param d the bytes
/// @param len their count
/// @param want_w the width needed (0: full size)
/// @param want_h the height needed (0: full size)
/// @param img filled in
/// @param err set to why, on failure
/// @return true if it loaded
bool image_load_mem(const uint8_t *d, size_t len, int want_w, int want_h, image_t *img,
                    const char **err);
/// How long the last image_load or image_load_mem took, by part, in
/// microseconds (0 on the PC): for finding what is slow.
typedef struct {
    uint32_t read_us;     ///< reading the file
    uint32_t decode_us;   ///< decoding it (PNG: inflating and unfiltering)
    uint32_t convert_us;  ///< making it RGB (PNG: blending its alpha)
    uint32_t sram_bytes;  ///< SRAM the row-at-a-time PNG decoder had for its buffers
} image_times_t;
/// The last load's times.
extern image_times_t image_times;

/// Frees the picture's pixels.
/// @param img the picture
void image_free(image_t *img);

/// A clip rectangle for drawing pictures.
typedef struct {
    /// x0 <= x < x1, y0 <= y < y1
    ///@{
    int x0, y0, x1, y1;
    ///@}
} image_clip_t;

/// Draws `img` into the screen rectangle (x, y, w, h) of `fb` (640x480,
/// RGB332), scaled to fill it, with ordered dithering, clipped to `clip` (NULL:
/// the whole screen).
/// @param img the picture
/// @param fb the screen
/// @param x the rectangle's left
/// @param y its top
/// @param w its width
/// @param h its height
/// @param clip what part of the screen may be drawn on, or NULL
void image_draw(const image_t *img, uint8_t *fb, int x, int y, int w, int h,
                const image_clip_t *clip);

/// Draws `img` scaled to w x h into `px`, a picture of its own (w bytes a
/// row, RGB332), with ordered dithering: for keeping a picture ready to copy.
/// @param img the picture
/// @param px w * h bytes
/// @param w the width
/// @param h the height
void image_draw_compact(const image_t *img, uint8_t *px, int w, int h);

/// Loads a picture straight into a w x h RGB332 picture of its own, scaled
/// and dithered as image_draw_compact does. Opaque, non-interlaced PNGs are
/// decoded a row at a time and never kept whole (much faster from PSRAM);
/// other files go through image_load.
/// @param path the file
/// @param px w * h bytes, filled in
/// @param w the width
/// @param h the height
/// @param err set to why, on failure
/// @return true if it loaded
bool image_load_compact(const char *path, uint8_t *px, int w, int h, const char **err);
/// The same, from bytes in memory.
/// @param d the bytes
/// @param len their count
/// @param px w * h bytes, filled in
/// @param w the width
/// @param h the height
/// @param err set to why, on failure
/// @return true if it loaded
bool image_load_compact_mem(const uint8_t *d, size_t len, uint8_t *px, int w, int h, const char **err);

/// The largest rectangle with img's shape that fits in w x h, centred in it.
/// @param img the picture
/// @param x set to the left
/// @param y set to the top
/// @param w the room's width, then the picture's
/// @param h the room's height, then the picture's
void image_fit(const image_t *img, int *x, int *y, int *w, int *h);

/// Saves the 640x480 RGB332 screen `fb` as an 8-bit BMP.
/// @param path the file
/// @param fb the screen
/// @param err set to why, on failure
/// @return true if it was saved
bool image_save_bmp(const char *path, const uint8_t *fb, const char **err);
