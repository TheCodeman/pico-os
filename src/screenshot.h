/**
 * @file screenshot.h
 * @brief Screenshots: the Print Screen key (or Ctrl-Alt-S) anywhere, and the screenshot command.
 *
 * The picture is taken the next time the running program reads a key; a box
 * then asks for the file name (the next free /SCREENSHOTS/SHOTnnnn.PNG), and
 * .PNG or .BMP picks the format.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Core 1 (the keyboard): the key was pressed.
void screenshot_request(void);
/// Core 0, where keys are read: takes it and asks, if one was requested.
/// getc gives the next key byte or -1 (straight from the keyboard, so the
/// program doesn't see them).
void screenshot_poll(int (*getc)(void));

/// The next free /SCREENSHOTS/SHOTnnnn.PNG (the folder is made if needed).
void screenshot_next_name(char *out, int size);
/// Saves a 640x480 RGB332 picture as PNG or BMP (by the name's extension);
/// pixels NULL: the screen as it shows now.
bool screenshot_save(const char *path, const uint8_t *pixels, const char **err);
/// The picture as shown now (line-doubled modes made full height) into out.
void screenshot_capture(uint8_t *out);
