/**
 * @file say.h
 * @brief The `say` command: speaks text over HDMI with the text-to-speech engine (tts.h), or saves the speech as a WAV file.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's say command: `say text` speaks the text; `say` alone asks for
/// lines and speaks each one; `say -f file` reads a text file aloud. Options:
/// -p pitch (Hz), -s speed (%), -w file.wav (save, don't play), -x (show the
/// phonemes).
/// @param argc the argument count
/// @param argv the arguments
void say_command(int argc, char **argv);
