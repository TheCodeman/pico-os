/**
 * @file spr.h
 * @brief Sprite files (.SPR): RGB332 pictures with see-through pixels, one or more frames of the same size, written by the sprite editor and read by it and by BASIC's SPRITE LOAD.
 *
 * The file:
 *
 *     0  "SPR1"
 *     4  width (1-32), height (1-32), frames (1-64), 0
 *     8  each frame: width * height colour bytes (RGB332, rows from the
 *        top), then (width * height + 7) / 8 bytes of which pixels are
 *        drawn (1) or see-through (0), the first pixel in bit 7.
 *
 * A see-through pixel's colour byte is 0.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPR_MAX_SIZE   32  ///< the widest and tallest a sprite can be
#define SPR_MAX_FRAMES 64  ///< the most frames in a file

/// A sprite file's size and frame count.
typedef struct {
    /// width and height in pixels (1 to SPR_MAX_SIZE)
    ///@{
    int w, h;
    ///@}
    int frames;  ///< 1 to SPR_MAX_FRAMES
} spr_info_t;

/// Given a frame read from a file: its colours and, a byte a pixel, 1 where
/// it is drawn. Returns false to stop reading.
typedef bool (*spr_put_fn)(void *ctx, int frame, const uint8_t *px, const uint8_t *on);
/// Asked for a frame to write: fills in its colours and, a byte a pixel, 1
/// where it is drawn.
typedef void (*spr_get_fn)(void *ctx, int frame, uint8_t *px, uint8_t *on);

/// Reads a .SPR file: fills in info, then gives each frame to put.
/// @param path the file
/// @param info set to the size and frame count (before the first frame)
/// @param put called for each frame, in order
/// @param ctx passed to put
/// @param err set to why, on failure
/// @return true if every frame was read
bool spr_load(const char *path, spr_info_t *info, spr_put_fn put, void *ctx, const char **err);
/// Writes a .SPR file, asking get for each frame.
/// @param path the file
/// @param info the size and frame count
/// @param get called for each frame, in order
/// @param ctx passed to get
/// @param err set to why, on failure
/// @return true if it was written
bool spr_save(const char *path, const spr_info_t *info, spr_get_fn get, void *ctx, const char **err);
/// Writes the frames as one PNG, side by side (RGBA, see-through pixels
/// transparent), for other programs.
/// @param path the file
/// @param info the size and frame count
/// @param get called for each frame, in order
/// @param ctx passed to get
/// @param err set to why, on failure
/// @return true if it was written
bool spr_save_png(const char *path, const spr_info_t *info, spr_get_fn get, void *ctx, const char **err);
