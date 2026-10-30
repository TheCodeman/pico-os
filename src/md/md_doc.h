/**
 * @file md_doc.h
 * @brief Markdown laid out for a screen of pixels: the md viewer's document.
 *
 * md_layout reads Markdown (CommonMark with GitHub's tables, task lists,
 * strikethrough and bare links) and lays it out for a column of a given
 * width: a list of things to draw (runs of text in one style, rectangles,
 * pictures), each at a place in the document, top to bottom. The viewer only
 * has to draw the ones that fall on the screen. Everything is in PSRAM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// What an item is.
enum { MI_TEXT, MI_RECT, MI_IMG };

/// A text item's style (flags).
enum {
    MF_BOLD = 1,     ///< thicker strokes
    MF_ITALIC = 2,   ///< slanted
    MF_STRIKE = 4,   ///< a line through it
    MF_ULINE = 8,    ///< a line under it (links)
    MF_BG = 16,      ///< its background is filled (bg), else left as it is
    MF_CODE = 32,    ///< code
    MF_MONO = 64,    ///< in the 8x16 font whatever the widths (code blocks, the source, bullets)
};

/// The fonts: the 8x16 one, and it drawn at 12x24 and 16x32.
enum { MD_FONT_8x16, MD_FONT_12x24, MD_FONT_16x32 };
/// A font's character width.
#define MD_CW(f) (8 + 4 * (f))
/// A font's character height.
#define MD_CH(f) (16 + 8 * (f))

/// Proportional text (TrueType fonts in the viewer): each CP437 character's
/// width in pixels, by font (MD_FONT_) and style (1 bold, 2 italic).
typedef struct {
    uint8_t adv[3][4][256];  ///< the widths
} md_widths_t;

/// No item is taller than this, so the ones on a screen are found from the
/// first one starting this far above it.
#define MD_ITEM_MAX_H 512
/// The tallest a picture is drawn.
#define MD_PIC_MAX_H 400

/// The colours (RGB332).
///@{
#define MD_C_BG      0x00  ///< the page
#define MD_C_TEXT    0xb6  ///< text
#define MD_C_BOLD    0xff  ///< bold text
#define MD_C_H1      0xfc  ///< headings, by level
#define MD_C_H2      0xf8
#define MD_C_H3      0x7d
#define MD_C_H4      0x5f
#define MD_C_LINK    0x57  ///< links
#define MD_C_CODE    0xf5  ///< code
#define MD_C_CODE_BG 0x25  ///< behind code
#define MD_C_QUOTE   0x92  ///< text in a quote
#define MD_C_BAR     0x6d  ///< a quote's bar
#define MD_C_RULE    0x49  ///< rules, table lines
#define MD_C_HEAD_BG 0x29  ///< a table's header row
#define MD_C_ROW_BG  0x05  ///< every other table row
#define MD_C_ALT     0xd3  ///< a picture that can't be shown (its alt text)
#define MD_C_BULLET  0xdb  ///< list bullets and numbers
#define MD_C_TICK    0x3c  ///< a ticked task
///@}

/// A thing to draw.
typedef struct {
    int32_t y;      ///< its top, in the document
    int16_t x;      ///< its left
    uint16_t w;     ///< its width
    uint16_t h;     ///< its height
    uint8_t kind;   ///< MI_
    uint8_t font;   ///< MI_TEXT: MD_FONT_
    uint8_t flags;  ///< MI_TEXT: MF_
    uint8_t fg;     ///< MI_TEXT: the text's colour; MI_RECT: the rectangle's
    uint8_t bg;     ///< MI_TEXT with MF_BG: the background
    uint16_t link;  ///< the link it is part of (1 on: links[link - 1]), or 0
    uint16_t len;   ///< MI_TEXT: its characters
    uint32_t ref;   ///< MI_TEXT: its characters (CP437) in the pool; MI_IMG: imgs index
} md_item_t;

/// A picture: where it comes from and its own size.
typedef struct {
    uint32_t src;  ///< what the document says (in the pool)
    uint32_t alt;  ///< its alt text (in the pool)
    uint16_t w;    ///< its size as the document wants it (before it is fitted to the column)
    uint16_t h;    ///< and its height
} md_img_t;

/// A heading, for the table of contents and #links.
typedef struct {
    int32_t y;      ///< its top
    uint32_t text;  ///< its text, CP437 (in the pool)
    uint32_t slug;  ///< its #name, as GitHub makes them (in the pool)
    uint8_t level;  ///< 1-6
} md_head_t;

/// A document laid out.
typedef struct {
    md_item_t *items;  ///< the things to draw, top to bottom (by y)
    int nitems;        ///< how many
    char *pool;        ///< text
    uint32_t npool;    ///< its bytes used
    uint32_t *links;   ///< each link's target (in the pool)
    int nlinks;        ///< how many
    md_img_t *imgs;    ///< the pictures
    int nimgs;         ///< how many
    md_head_t *heads;  ///< the headings
    int nheads;        ///< how many
    int32_t height;    ///< the document's height in pixels
    int width;         ///< the width it was laid out for
    bool short_of_memory;  ///< it ran out of memory: the end is missing
    int caps[5];       ///< room in items, pool, links, imgs, heads (for md_layout)
} md_doc_t;

/// Tells md_layout a picture's size, so it can leave room for it.
/// @param arg what md_layout was given
/// @param src the picture, as the document names it
/// @param w set to its width in pixels
/// @param h set to its height
/// @return false if it can't be shown (its alt text is shown instead)
typedef bool (*md_size_fn)(void *arg, const char *src, int *w, int *h);

/// Lays Markdown out.
/// @param src the text (UTF-8)
/// @param len its length
/// @param width the column's width in pixels
/// @param source show the text as it is (each line wrapped), not laid out
/// @param size_of tells a picture's size (NULL: pictures show as their alt text)
/// @param arg passed to it
/// @return the document (free with md_free), or NULL if there was no memory at all
md_doc_t *md_layout(const char *src, uint32_t len, int width, bool source, md_size_fn size_of, void *arg);
/// Sets the widths md_layout lays text out with: proportional ones, or NULL
/// for the 8x16 font's (MD_CW). Code, the source view and bullets keep the
/// 8x16 font's. They must stay put while documents laid out with them are shown.
/// @param w the widths, or NULL
void md_set_widths(const md_widths_t *w);
/// A character's width in a text item, as it was laid out.
/// @param it the item
/// @param c the character (CP437)
/// @return its width in pixels
int md_char_w(const md_item_t *it, uint8_t c);
/// Frees a document.
/// @param d it (NULL: nothing)
void md_free(md_doc_t *d);
/// The first item that may show at or below y.
/// @param d the document
/// @param y a place in it
/// @return the item's index (nitems if none)
int md_first_item(const md_doc_t *d, int32_t y);
