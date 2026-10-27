/**
 * @file genesis_launcher.h
 * @brief The genesis command's game launcher: a full-screen list of the games in a folder, drawn like the tracker's dialogs (src/mod/trk_gfx.c), with the highlighted game's cartridge header beside it.
 *
 * The caller has taken the screen from the console; the launcher draws on
 * the 640x480 framebuffer.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/// Shows dir (dir_len bytes; moving into folders changes it) with the cursor
/// on the file called last, if it is there, and message on the bottom line
/// if not NULL. Returns true with path set to the game picked, false to
/// leave (Esc or F10).
bool genesis_launcher(char *dir, size_t dir_len, const char *last, const char *message, char *path,
                      size_t path_len);

/// While a game loads: what and how far, on the bottom line.
void genesis_launcher_progress(const char *what, int percent);
