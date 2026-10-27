/**
 * @file chime.h
 * @brief The start-up chime: a soft C major arpeggio (C5 E5 G5 C6) of bell-like tones, about 1.5 s, made as it plays (no samples stored).
 *
 * Plain C: the PC tests render it to a WAV file.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Starts the start-up chime's notes over (core 1 then mixes them in).
void chime_begin(void);
/// The next 48 kHz stereo frame, added to *left and *right; false once it
/// has faded out.
bool chime_sample(int32_t *left, int32_t *right);
