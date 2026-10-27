/**
 * @file console.h
 * @brief 80x30 text console drawn into the DVI framebuffer with an 8x16 font.
 *
 * It understands the common VT100/ANSI escape sequences: cursor movement and
 * positioning (ESC [ A B C D E F G H d f), erase (J K X), insert and delete
 * (@ P L M), scrolling and scroll regions (S T r, ESC D, ESC M), colours and
 * attributes (m: 0 1 7 22 27 30-37 39 40-47 49 90-97 100-107), save/restore
 * (s u, ESC 7, ESC 8), cursor show/hide (?25 h/l), autowrap (?7 h/l) and
 * reset (ESC c).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdio/driver.h"

/// Text columns on the console.
#define CONSOLE_COLS 80
/// Text rows on the console.
#define CONSOLE_ROWS 30

/// Clears the screen and sets up the console (cursor blink, mouse pointer timers).
void console_init(void);
/// Clears the screen.
void console_clear(void);
/// Sets the colours (RGB332) for text written from now on.
void console_set_color(uint8_t fg, uint8_t bg);
/// Writes one character, escape sequences included.
void console_putc(char c);
/// Writes `len` characters, escape sequences included.
void console_write(const char *buf, int len);
/// Draws character c (0..255, code page 437) at the cursor even if it is a
/// control code, e.g. CHR$(1), the smiley.
void console_put_glyph(uint8_t c);

/// The character in a cell and its colours as a PC attribute byte
/// (foreground + 16 * background, colours 0-15); and writing one there without
/// moving the cursor (BASIC's SCREEN function and text-memory PEEK/POKE).
void console_get_cell(int row, int col, uint8_t *ch, uint8_t *attr);
/// Puts a character with a PC attribute (colours) straight into a cell.
void console_set_cell(int row, int col, uint8_t ch, uint8_t attr);

/// Where the cursor is (0-based).
void console_get_cursor(int *row, int *col);

/// The scroll region's first and last rows (0-based, inclusive).
void console_get_scroll_region(int *top, int *bottom);

/// For remote sessions: where answers to status queries (ESC [ 6 n, ESC [ c)
/// go (NULL: nowhere); whether cursor keys are in application mode (send
/// ESC O A rather than ESC [ A); and putting the modes back afterwards
/// (main screen, character sets, scrolling, colours, cursor).
void console_set_reply(void (*fn)(const char *text, int len));
/// True if the far end asked for application cursor keys (ESC [ ? 1 h).
bool console_app_cursor(void);
/// Undoes modes a program set: scroll region, hidden cursor, colours, cursor keys.
void console_reset_modes(void);

/// The mouse pointer in text mode (mouse.h) drawn normally again now, as it
/// stops being the text pointer.
void console_pointer_off(void);

/// While frozen, the cursor's blink and the mouse pointer leave the screen
/// alone (something else is drawing over it for a moment).
void console_freeze(bool on);

/// Add with stdio_set_driver_enabled() to send printf output to the screen.
extern stdio_driver_t console_stdio_driver;
