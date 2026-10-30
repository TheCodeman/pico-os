/**
 * @file md.h
 * @brief The `md` command: a Markdown viewer in graphics, with pictures.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's md (markdown) command: `md [file.md]` opens the viewer.
/// @param argc the argument count
/// @param argv the arguments
void md_command(int argc, char **argv);
