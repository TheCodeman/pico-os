/**
 * @file sprite.h
 * @brief The `sprite` command: a graphical editor for RGB332 sprites with animation frames, saved as .SPR files (spr.h) for BASIC's SPRITE LOAD.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's sprite (spredit) command: `sprite [file]` opens the editor;
/// `sprite info file` and `sprite png file [out]` are for scripts.
/// @param argc the argument count
/// @param argv the arguments
void sprite_command(int argc, char **argv);
