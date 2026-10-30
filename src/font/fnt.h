/**
 * @file fnt.h
 * @brief Anti-aliased text from font files: TrueType (.ttf), OpenType with
 *        CFF outlines (.otf), both drawn with stb_truetype, and PFN, our own
 *        pre-drawn 4-bit greyscale glyphs (tools/fonttest/mkpfn.py).
 *
 * The font file is kept in memory by the caller (PSRAM); outline glyphs are
 * drawn once per size and kept in a glyph cache in PSRAM, PFN glyphs are
 * read from the file as they are. Text is blended into an RGB332 picture
 * (the framebuffer, or one like it), with or without dithering.
 *
 * Memory: the state and the glyph cache in PSRAM, and while any outline
 * font is open one small arena in the SRAM heap (FNT_ARENA), shared by them,
 * for stb_truetype's working buffers, so drawing a glyph doesn't go through
 * the PSRAM heap.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The SRAM arena stb_truetype's working buffers come from (bigger ones go to PSRAM).
#define FNT_ARENA (16 * 1024)

/// What kind of font file it is.
typedef enum {
    FNT_TTF,  ///< TrueType outlines (glyf, quadratic curves)
    FNT_CFF,  ///< OpenType with CFF outlines (cubic curves)
    FNT_PFN,  ///< pre-drawn 4-bit glyphs at fixed sizes
} fnt_kind_t;

/// An open font.
typedef struct fnt fnt_t;

/// Figures kept while a font is open (for the benchmark).
typedef struct {
    uint32_t drawn;        ///< glyphs drawn from their outlines
    uint32_t hits;         ///< glyphs found in the cache
    uint32_t flushes;      ///< times the cache was full and emptied
    size_t cache_bytes;    ///< glyph pixels in the cache now
    size_t arena_peak;     ///< the most of the SRAM arena one glyph used
    uint32_t spills;       ///< working buffers that didn't fit it (from PSRAM)
} fnt_stats_t;

/// Opens a font from a file's bytes, which must stay put until fnt_close.
/// @param data the file
/// @param size its length
/// @param err set to why not (on failure)
/// @return the font, or NULL
fnt_t *fnt_open(const uint8_t *data, size_t size, const char **err);
/// Closes it and frees its cache and arena (not the file's bytes).
/// @param f the font (NULL is fine)
void fnt_close(fnt_t *f);

/// @param f the font
/// @return its kind
fnt_kind_t fnt_kind(const fnt_t *f);
/// @param f the font
/// @return how many glyphs it has (in a PFN file: in its first size)
int fnt_glyphs(const fnt_t *f);
/// The sizes a PFN file has (outline fonts: none, any size will do).
/// @param f the font
/// @param out filled in, smallest first
/// @param max room in it
/// @return how many
int fnt_sizes(const fnt_t *f, int *out, int max);
/// The size actually drawn for a size asked for: itself, or for a PFN file the nearest it has.
/// @param f the font
/// @param px the size, in pixels per em
/// @return the size used
int fnt_size_used(const fnt_t *f, int px);

/// The line's measures at a size, in pixels.
/// @param f the font
/// @param px the size, in pixels per em
/// @param ascent set to the height above the baseline
/// @param descent set to the depth below it (positive)
/// @param gap set to the space between lines
void fnt_vmetrics(fnt_t *f, int px, int *ascent, int *descent, int *gap);

/// Draws UTF-8 text into an RGB332 picture, blended over what is there.
/// @param f the font
/// @param px the size, in pixels per em
/// @param dst the picture
/// @param stride its bytes per row
/// @param cw its width (clipped to it)
/// @param ch its height
/// @param x the pen's start
/// @param y the baseline
/// @param s the text
/// @param fg the colour (RGB332)
/// @return the pen's x after it
int fnt_draw(fnt_t *f, int px, uint8_t *dst, int stride, int cw, int ch, int x, int y, const char *s,
             uint8_t fg);
/// A character's advance (no kerning).
/// @param f the font
/// @param px the size
/// @param cp the character
/// @return its advance, 1/64 px (0 if the font lacks it)
int32_t fnt_advance(fnt_t *f, int px, int cp);
/// Draws one character with its origin (the pen on the baseline) at (x, y):
/// for text laid out by the caller.
/// @param f the font
/// @param px the size
/// @param dst the picture
/// @param stride its bytes per row
/// @param cw its width (clipped to it)
/// @param ch its height
/// @param x the pen
/// @param y the baseline
/// @param cp the character
/// @param fg the colour (RGB332)
void fnt_draw_cp(fnt_t *f, int px, uint8_t *dst, int stride, int cw, int ch, int x, int y, int cp, uint8_t fg);
/// The width text would take.
/// @param f the font
/// @param px the size
/// @param s the text
/// @return its advance, in pixels
int fnt_width(fnt_t *f, int px, const char *s);

/// Whether drawn glyphs are kept (on by default). Off, every glyph is drawn
/// from its outline each time (for the benchmark); PFN fonts don't care.
/// @param f the font
/// @param on keep them
void fnt_set_cache(fnt_t *f, bool on);
/// Whether pairs of letters are kerned (on by default).
/// @param f the font
/// @param on kern them
void fnt_set_kerning(fnt_t *f, bool on);
/// Whether the blend is dithered (ordered, 4x4), which hides RGB332's few levels.
/// @param f the font
/// @param on dither
void fnt_set_dither(fnt_t *f, bool on);
/// Empties the glyph cache.
/// @param f the font
void fnt_cache_clear(fnt_t *f);
/// Sets the most bytes of glyph pixels the cache keeps before it is emptied.
/// @param f the font
/// @param bytes the budget
void fnt_cache_budget(fnt_t *f, size_t bytes);
/// @param f the font
/// @param out filled in
void fnt_get_stats(const fnt_t *f, fnt_stats_t *out);

/// Lends the SRAM arena out (frees it) while something else needs SRAM for a
/// moment, or takes it back. Glyphs drawn meanwhile use PSRAM for their
/// working buffers (slower, still right).
/// @param lend true to free it, false to have it again
void fnt_arena_lend(bool lend);

/// Draws one glyph from its outline into a buffer of the font's own and
/// throws it away: what drawing costs without the cache (outline fonts only).
/// @param f the font
/// @param px the size
/// @param cp the character
/// @return its pixels (w * h), or -1 for a PFN font
int fnt_raster_one(fnt_t *f, int px, int cp);
/// The same for a signed distance field (stbtt_GetCodepointSDF), padding 4.
/// @param f the font
/// @param px the size
/// @param cp the character
/// @return its pixels, or -1 for a PFN font
int fnt_sdf_one(fnt_t *f, int px, int cp);
/// Looks a glyph up the way drawing does (cmap, or a PFN's table) and nothing more.
/// @param f the font
/// @param cp the character
/// @return its glyph index (PFN: its entry), 0 or -1 if it has none
int fnt_find(fnt_t *f, int cp);
