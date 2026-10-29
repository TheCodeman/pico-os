/**
 * @file hexedit.h
 * @brief The `hex` command: a graphical hex editor for files of any size, read from the drive a page at a time.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's hex (hexedit) command: `hex [file]` opens the editor.
/// @param argc the argument count
/// @param argv the arguments
void hex_command(int argc, char **argv);
