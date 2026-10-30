/**
 * @file md_ttf.c
 * @brief The md viewer's TrueType text (see md_ttf.h).
 *
 * The 8x16 font's cells are kept: a line of body text is still 16 pixels,
 * a heading's 24 or 32, and the fonts are drawn at those sizes per em with
 * the baseline three quarters of the way down. Only the widths change: each
 * character's advance, rounded to a pixel, so md_layout's lines and the
 * drawing agree exactly (no kerning).
 *
 * Memory, while the viewer runs: the font files, their glyph caches and the
 * width table in PSRAM, and fnt.c's shared 16 KB arena in the SRAM heap.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "md_ttf.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "fnt.h"
#include "psram_heap.h"
#include "www_html.h"

#define FONT_MAX (2u << 20)  ///< the biggest font file loaded

/// The sizes drawn (pixels per em) for MD_FONT_8x16, 12x24 and 16x32.
static const uint8_t em[3] = {16, 24, 32};

/// The styles: regular, bold, italic, bold italic.
enum { ST_REG, ST_BOLD, ST_ITAL, ST_BOLDITAL, NSTYLES };

/// The fonts loaded (PSRAM).
typedef struct {
    uint8_t *data[NSTYLES];  ///< the files (NULL: not there)
    fnt_t *f[NSTYLES];       ///< the fonts
    fnt_t *use[NSTYLES];     ///< the font drawn for each style (one there)
    bool fake_bold[NSTYLES]; ///< bold made by drawing twice
    md_widths_t widths;      ///< for md_layout
    bool on;                 ///< in use
} ttf_t;

static ttf_t *T;  ///< the fonts, while the viewer runs

/// Reads a font file into PSRAM and opens it.
/// @param path the file
/// @param data set to its bytes
/// @return the font, or NULL
static fnt_t *load(const char *path, uint8_t **data) {
    FIL fl;
    *data = NULL;
    if (f_open(&fl, path, FA_READ) != FR_OK)
        return NULL;
    FSIZE_t size = f_size(&fl);
    UINT got = 0;
    uint8_t *d = size >= 12 && size <= FONT_MAX ? psram_malloc((size_t)size) : NULL;
    if (!d || f_read(&fl, d, (UINT)size, &got) != FR_OK || got != size) {
        f_close(&fl);
        psram_free(d);
        return NULL;
    }
    f_close(&fl);
    const char *err;
    fnt_t *f = fnt_open(d, (size_t)size, &err);
    if (!f) {
        psram_free(d);
        return NULL;
    }
    fnt_cache_budget(f, 512u << 10);
    *data = d;
    return f;
}

/// The font files' names: from /FONTS/MD.CFG, else the defaults.
/// @param path filled in, by style
static void names(char path[NSTYLES][96]) {
    static const char *const def[NSTYLES] = {"/FONTS/SS3.TTF", "/FONTS/SS3-B.TTF", "/FONTS/SS3-I.TTF",
                                             "/FONTS/SS3-BI.TTF"};
    static const char *const key[NSTYLES] = {"regular=", "bold=", "italic=", "bolditalic="};
    for (int i = 0; i < NSTYLES; ++i)
        snprintf(path[i], 96, "%s", def[i]);
    FIL fl;
    if (f_open(&fl, "/FONTS/MD.CFG", FA_READ) != FR_OK)
        return;
    char line[128];
    while (f_gets(line, sizeof line, &fl)) {
        line[strcspn(line, "\r\n")] = 0;
        for (int i = 0; i < NSTYLES; ++i) {
            size_t n = strlen(key[i]);
            if (!strncmp(line, key[i], n)) {
                const char *v = line + n;
                if (*v == '/')
                    snprintf(path[i], 96, "%s", v);
                else
                    snprintf(path[i], 96, "/FONTS/%s", v);
            }
        }
    }
    f_close(&fl);
}

bool md_ttf_open(char *msg, int size) {
    md_ttf_close();
    T = psram_malloc(sizeof *T);
    if (!T) {
        snprintf(msg, size, "Not enough memory for TrueType text");
        return false;
    }
    memset(T, 0, sizeof *T);
    char path[NSTYLES][96];
    names(path);
    int loaded = 0;
    for (int i = 0; i < NSTYLES; ++i)
        if ((T->f[i] = load(path[i], &T->data[i])) != NULL)
            ++loaded;
    if (!T->f[ST_REG]) {
        snprintf(msg, size, "No font %.60s: the 8x16 font is used", path[ST_REG]);
        md_ttf_close();
        return false;
    }
    // Each style's font: its own, else the nearest there is.
    T->use[ST_REG] = T->f[ST_REG];
    T->use[ST_BOLD] = T->f[ST_BOLD] ? T->f[ST_BOLD] : T->f[ST_REG];
    T->use[ST_ITAL] = T->f[ST_ITAL] ? T->f[ST_ITAL] : T->f[ST_REG];
    T->use[ST_BOLDITAL] = T->f[ST_BOLDITAL] ? T->f[ST_BOLDITAL] : T->f[ST_BOLD] ? T->f[ST_BOLD] : T->use[ST_ITAL];
    T->fake_bold[ST_BOLD] = !T->f[ST_BOLD];
    T->fake_bold[ST_BOLDITAL] = !T->f[ST_BOLDITAL] && !T->f[ST_BOLD];
    for (int fo = 0; fo < 3; ++fo)
        for (int s = 0; s < NSTYLES; ++s)
            for (int c = 0; c < 256; ++c) {
                int32_t a = fnt_advance(T->use[s], em[fo], (int)www_unicode((uint8_t)c));
                if (!a)  // drawn as nothing: as wide as a space
                    a = fnt_advance(T->use[s], em[fo], ' ');
                int w = (int)((a + 32) >> 6) + (T->fake_bold[s] ? 1 : 0);
                T->widths.adv[fo][s][c] = (uint8_t)(w < 1 ? 1 : w > 255 ? 255 : w);
            }
    T->on = true;
    md_set_widths(&T->widths);
    snprintf(msg, size, "TrueType text: %d font%s from /FONTS", loaded, loaded == 1 ? "" : "s");
    return true;
}

void md_ttf_close(void) {
    md_set_widths(NULL);
    if (!T)
        return;
    for (int i = 0; i < NSTYLES; ++i) {
        fnt_close(T->f[i]);
        psram_free(T->data[i]);
    }
    psram_free(T);
    T = NULL;
}

void md_ttf_lend(bool lend) {
    if (T)
        fnt_arena_lend(lend);
}

bool md_ttf_loaded(void) {
    return T != NULL;
}

void md_ttf_use(bool on) {
    if (!T)
        return;
    T->on = on;
    md_set_widths(on ? &T->widths : NULL);
}

bool md_ttf_on(void) {
    return T && T->on;
}

bool md_ttf_item(const md_item_t *it) {
    return T && T->on && it->kind == MI_TEXT && !(it->flags & (MF_CODE | MF_MONO));
}

void md_ttf_char(const md_item_t *it, uint8_t c, uint8_t *dst, int stride, int cw, int ch, int x, int y,
                 uint8_t fg) {
    if (c == ' ')
        return;
    int s = (it->flags & MF_BOLD ? 1 : 0) | (it->flags & MF_ITALIC ? 2 : 0);
    int fo = it->font < 3 ? it->font : 0;
    int base = y + MD_CH(fo) * 3 / 4;
    int cp = (int)www_unicode(c);
    fnt_draw_cp(T->use[s], em[fo], dst, stride, cw, ch, x, base, cp, fg);
    if (T->fake_bold[s])
        fnt_draw_cp(T->use[s], em[fo], dst, stride, cw, ch, x + 1, base, cp, fg);
}
