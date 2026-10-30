/**
 * @file font.h
 * @brief The `font` command (ttf too): TrueType, OpenType and PFN fonts shown and timed.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's font (ttf) command: `font FILE [size] ["text"]` or `font bench FILE...`.
/// @param argc the argument count
/// @param argv the arguments
void font_command(int argc, char **argv);
