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
/// @param dir the folder shown (updated as the user moves into others)
/// @param dir_len its buffer's size
/// @param last the game to put the cursor on, or NULL
/// @param message a line to show (an error), or NULL
/// @param path set to the game picked
/// @param path_len its buffer's size
/// @return true if a game was picked, false for Esc
bool genesis_launcher(char *dir, size_t dir_len, const char *last, const char *message, char *path,
                      size_t path_len);

/// While a game loads: what and how far, on the bottom line.
/// @param what what is happening
/// @param percent how far, 0-100
void genesis_launcher_progress(const char *what, int percent);
