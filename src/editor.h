/**
 * @file editor.h
 * @brief Full-screen text editor (nano-style keys), drawn with VT100 sequences through stdio so it works on the HDMI console and the serial terminal.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// Edits `path` on the USB drive (a new file is created on first save; NULL
/// or "" asks for a name when saving). Returns when the user quits, with the
/// screen cleared for the shell.
void editor_run(const char *path);
