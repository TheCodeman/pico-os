/**
 * @file image_rows.h
 * @brief Inside the picture code: a row dithered to RGB332 (image.c), and
 *        PNGs decoded a row at a time straight into an RGB332 picture (png_rows.c).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/// Dithers one row of a picture, scaled across, as image_draw does.
/// @param row the picture's row, RGB
/// @param iw its width in pixels
/// @param out the row drawn (out[x0] to out[x1 - 1] are written)
/// @param x where the picture's left is drawn
/// @param x0 the first column written
/// @param x1 after the last
/// @param dy the row's place on the screen (for the dither pattern)
/// @param step picture columns a screen column, 16.16
void image_dither_row(const uint8_t *row, int iw, uint8_t *out, int x, int x0, int x1, int dy, int64_t step);

/// Decodes a PNG straight into a w x h RGB332 picture, scaled and dithered as
/// image_draw_compact does, a row at a time: the whole picture is never kept.
/// Only for opaque, non-interlaced PNGs up to PNG_ROWS_MAX_W wide.
/// @param d the file
/// @param len its size
/// @param px w * h bytes, filled in
/// @param w the width wanted
/// @param h the height
/// @param err set to why, on failure
/// @return 1 done, 0 not a PNG this does (use LodePNG), -1 failed (err set)
int png_rows_decode(const uint8_t *d, size_t len, uint8_t *px, int w, int h, const char **err);

/// The widest PNG png_rows_decode takes.
#define PNG_ROWS_MAX_W 4096
