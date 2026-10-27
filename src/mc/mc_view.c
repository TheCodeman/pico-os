/**
 * @file mc_view.c
 * @brief mc's viewer (F3): text with or without wrapping, or a hex dump; search.
 *
 * The file is read into PSRAM (up to VIEW_MAX).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mc.h"
#include "psram_heap.h"

/// The biggest file viewed.
#define VIEW_MAX (4u << 20)
/// Text rows.
#define ROWS (SCR_H - 2)

/// The file.
static uint8_t *data;
/// Its size.
static uint32_t size;
/// Wrap long lines; hex.
///@{
static bool wrap = true, hex;
///@}
/// The search's match: where, and its length.
///@{
static uint32_t match_at = UINT32_MAX, match_len;
///@}

/// The start of the screen row after the one starting at `at` (text mode).
/// @param at the row's start
/// @return the next row's
static uint32_t next_row(uint32_t at) {
    int col = 0;
    for (uint32_t i = at; i < size; ++i) {
        uint8_t c = data[i];
        if (c == '\n')
            return i + 1;
        col += c == '\t' ? 8 - col % 8 : (c == '\r' ? 0 : 1);
        if (wrap && col >= SCR_W)
            return i + 1;
    }
    return size;
}

/// The start of the screen row before the one at at.
/// @param at the row's start
/// @return the row before's
static uint32_t prev_row(uint32_t at) {
    if (!at)
        return 0;
    if (hex)
        return at >= 16 ? at - 16 : 0;
    // The start of the line before `at`, then its rows up to `at`.
    uint32_t line = at - 1;
    while (line > 0 && data[line - 1] != '\n')
        --line;
    uint32_t row = line, prev = line;
    while (row < at) {
        prev = row;
        row = next_row(row);
    }
    return prev;
}

/// A row down, unless at the end.
/// @param at the row's start
/// @return the next row's
static uint32_t down(uint32_t at) {
    if (hex)
        return at + 16 < size ? at + 16 : at;
    uint32_t n = next_row(at);
    return n < size ? n : at;
}

/// Draws the viewer.
/// @param path the file's path
/// @param top the first row
static void draw(const char *path, uint32_t top) {
    scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
    char title[SCR_W + 1];
    int pct = size ? (int)((uint64_t)top * 100 / size) : 100;
    snprintf(title, sizeof title, " %s", path);
    scr_text(0, 0, SCR_W - 30, C_MENU, title);
    scr_printf(0, SCR_W - 30, 30, C_MENU, "%10lu/%-10lu %3d%% ", (unsigned long)top,
               (unsigned long)size, pct);
    scr_fill(1, 0, ROWS, SCR_W, ' ', C_VIEW);
    uint32_t at = top;
    for (int r = 0; r < ROWS && at < size; ++r) {
        if (hex) {
            scr_printf(1 + r, 0, 10, A(YELLOW, BLUE), "%08lX", (unsigned long)at);
            for (int i = 0; i < 16 && at + i < size; ++i) {
                uint32_t o = at + i;
                bool hit = o >= match_at && o < match_at + match_len;
                uint8_t c = data[o];
                scr_printf(1 + r, 10 + i * 3 + (i >= 8), 2, hit ? C_SELECT : C_VIEW, "%02X", c);
                scr_put(1 + r, 60 + i, c < 0x20 ? '.' : c, hit ? C_SELECT : C_VIEW);
            }
            at += 16;
            continue;
        }
        int col = 0;
        uint32_t end = next_row(at);
        for (uint32_t i = at; i < end && col < SCR_W; ++i) {
            uint8_t c = data[i];
            bool hit = i >= match_at && i < match_at + match_len;
            if (c == '\n' || c == '\r')
                continue;
            if (c == '\t') {
                do
                    scr_put(1 + r, col++, ' ', C_VIEW);
                while (col % 8 && col < SCR_W);
                continue;
            }
            scr_put(1 + r, col++, c < 0x20 ? '.' : c, hit ? C_SELECT : C_VIEW);
        }
        at = end;
    }
    const char *const labels[10] = {"Help", wrap ? "UnWrap" : "Wrap", "Quit",
                                    hex ? "Ascii" : "Hex", "Goto", "", "Search", "", "", "Quit"};
    int c = 0;
    for (int i = 0; i < 10; ++i) {
        scr_printf(SCR_H - 1, c, 2, C_KEYNUM, "%2d", i + 1);
        scr_text(SCR_H - 1, c + 2, 6, C_KEYTEXT, labels[i]);
        c += 8;
    }
    scr_cursor(-1, 0);
    scr_flush();
}

/// Finds text, any case.
/// @param text the text
/// @param from where to start
/// @param found set to where
/// @return found
static bool search(const char *text, uint32_t from, uint32_t *found) {
    size_t n = strlen(text);
    for (uint32_t i = from; n && i + n <= size; ++i)
        if (!strncasecmp((const char *)data + i, text, n)) {
            *found = i;
            return true;
        }
    return false;
}

void view_file(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        dlg_message("View", "Can't open the file.", "OK", true);
        return;
    }
    uint64_t full = f_size(&f);
    size = full > VIEW_MAX ? VIEW_MAX : (uint32_t)full;
    data = psram_malloc(size + 1);
    if (!data) {
        f_close(&f);
        dlg_message("View", "Not enough memory for this file.", "OK", true);
        return;
    }
    dlg_progress_begin("View");
    uint32_t got_total = 0;
    UINT got;
    while (got_total < size && f_read(&f, data + got_total, size - got_total > 32768 ? 32768 : size - got_total, &got) == FR_OK && got) {
        got_total += got;
        dlg_progress("Reading", path, got_total, size, 0, 0);
        if (key_stop())
            break;
    }
    dlg_progress_end();
    f_close(&f);
    size = got_total;
    void *saved = scr_save();
    if (full > VIEW_MAX)
        dlg_message("View", "The file is big: only its first 4 MB are shown.", "OK", false);
    uint32_t top = 0;
    static char last[64];
    match_at = UINT32_MAX;
    for (;;) {
        draw(path, top);
        int k = key_get();
        if (k == KEY_ESC || k == KEY_F3 || k == KEY_F10 || k == 'q' || k == 'Q')
            break;
        switch (k) {
        case KEY_UP: top = prev_row(top); break;
        case KEY_DOWN: top = down(top); break;
        case KEY_PGUP:
        case 'b':
            for (int i = 0; i < ROWS - 1; ++i)
                top = prev_row(top);
            break;
        case KEY_PGDN:
        case ' ':
            for (int i = 0; i < ROWS - 1; ++i)
                top = down(top);
            break;
        case KEY_HOME: top = 0; break;
        case KEY_END:
            top = hex ? (size > 16 ? (size - 1) & ~15u : 0) : size;
            for (int i = 0; i < ROWS - 1 && !hex; ++i)
                top = prev_row(top);
            if (hex)
                for (int i = 0; i < ROWS - 1; ++i)
                    top = prev_row(top);
            break;
        case KEY_F2: wrap = !wrap; top = prev_row(down(top)); break;
        case KEY_F4:
            hex = !hex;
            top = hex ? top & ~15u : prev_row(down(top));
            break;
        case KEY_F5: {
            char where[24] = "";
            if (dlg_input("Go to", "Offset (decimal, 0x hex) or a percentage (50%):", where,
                          sizeof where) && where[0]) {
                uint32_t o = strchr(where, '%') ? (uint32_t)((uint64_t)strtoul(where, NULL, 10) * size / 100)
                                                : strtoul(where, NULL, 0);
                if (o > size)
                    o = size;
                top = hex ? o & ~15u : prev_row(o + 1 <= size ? o + 1 : o);
            }
            break;
        }
        case KEY_F7:
        case '/':
            if (!dlg_input("Search", "Text to find (any case):", last, sizeof last) || !last[0])
                break;
            /* fall through */
        case 'n': {
            uint32_t found;
            uint32_t from = match_at != UINT32_MAX ? match_at + 1 : top;
            if (last[0] && search(last, from, &found)) {
                match_at = found;
                match_len = strlen(last);
                top = hex ? found & ~15u : prev_row(found + 1);
            } else if (last[0]) {
                dlg_message("Search", "Not found.", "OK", false);
            }
            break;
        }
        case KEY_F1:
            dlg_message("Viewer keys",
                        "Arrows, PgUp/PgDn (Space, b), Home/End: move\n"
                        "F2 wrap or not      F4 hex or text\n"
                        "F5 go to offset or %  F7 or / search, n next\n"
                        "F3, F10, Esc or q: back to the panels",
                        "OK", false);
            break;
        }
    }
    scr_restore(saved);
    psram_free(data);
    data = NULL;
}
