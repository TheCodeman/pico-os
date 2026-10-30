/**
 * @file md_ttf.h
 * @brief The md viewer's TrueType text: fonts from /FONTS, their widths for
 *        md_layout, and characters drawn anti-aliased from the glyph cache.
 *
 * The fonts are named in /FONTS/MD.CFG (lines `regular=`, `bold=`,
 * `italic=` and `bolditalic=`, each a path), else /FONTS/SS3.TTF,
 * SS3-B.TTF, SS3-I.TTF and SS3-BI.TTF. Only the regular one is needed: a
 * missing bold one is the regular one drawn twice, a missing italic one is
 * upright. Any font fnt.c reads will do (.ttf, .otf, .pfn).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "md_doc.h"

/// Loads the fonts and hands their widths to md_layout (md_set_widths).
/// @param msg set to what happened (fonts loaded, or why not)
/// @param size its room
/// @return false if there is no regular font or not enough memory (the 8x16 font is used)
bool md_ttf_open(char *msg, int size);
/// Frees the fonts and sets md_layout back to the 8x16 font's widths.
void md_ttf_close(void);
/// Lends the fonts' 16 KB SRAM arena out for a moment (a picture decoding), or takes it back.
/// @param lend true to lend it, false to take it back
void md_ttf_lend(bool lend);
/// @return the fonts are loaded
bool md_ttf_loaded(void);
/// Whether text is drawn with the fonts (and laid out with their widths).
/// @param on with them
void md_ttf_use(bool on);
/// @return text is drawn with them
bool md_ttf_on(void);
/// Whether an item's text is drawn with the fonts (not code, the source or bullets).
/// @param it the item
/// @return it is
bool md_ttf_item(const md_item_t *it);
/// Draws an item's character.
/// @param it the item (its font and style)
/// @param c the character (CP437)
/// @param dst the picture
/// @param stride its bytes per row
/// @param cw its width (clipped to it)
/// @param ch its height
/// @param x the character's left
/// @param y its line's top
/// @param fg its colour
void md_ttf_char(const md_item_t *it, uint8_t c, uint8_t *dst, int stride, int cw, int ch, int x, int y,
                 uint8_t fg);
