/**
 * @file wav.h
 * @brief The `wav` command: a graphical WAV editor (load, look, select, cut, paste, effects, play over HDMI, save).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's wav (wavedit) command: `wav [file]` opens the editor;
/// `wav info file` prints a WAV file's format and length.
/// @param argc the argument count
/// @param argv the arguments
void wav_command(int argc, char **argv);
