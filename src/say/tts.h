/**
 * @file tts.h
 * @brief A small text-to-speech engine: English spelling rules to phonemes, and a formant synthesizer that speaks them at 16 kHz.
 *
 * Text first becomes a line of phonemes (tts_phonemes): numbers are read out,
 * short words in capitals are spelled ("USB"), and other words go through
 * letter-to-sound rules in the style of the US Naval Research Laboratory's
 * 1976 rules (written afresh here). Text in [brackets] is taken as phonemes
 * as they are, e.g. "[HH EH1 L OW]".
 *
 * The phonemes are ARPAbet-like names separated by spaces; a vowel with a 1
 * after it is stressed. "/" ends a word, "," is a short pause, "." a long one
 * with a falling end, and "?" a long one with a rising end.
 *
 * The synthesizer (tts_start, tts_render) is a cascade formant synthesizer in
 * the manner of Klatt's: a glottal pulse and aspiration noise pass through
 * five resonators, and frication noise through a band-pass filter beside
 * them. Each phoneme is a target for the resonators and the sources, with
 * glides between them; stops are a closure, a burst and (unvoiced) some
 * aspiration; the pitch falls slowly through each phrase, with a lift on
 * each stressed vowel. It makes the sound piece by piece, so speech of any
 * length needs only the phoneme list and the filter state.
 *
 * Plain C, no memory but malloc and free, so the PC test runs it as it is.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/// The rate tts_render makes samples at.
#define TTS_RATE 16000

/// How the voice sounds.
typedef struct {
    int pitch;  ///< the voice's pitch in Hz (50-300; 0: 110)
    int speed;  ///< the speaking speed in percent (40-250; 0: 100)
} tts_voice_t;

/// The synthesizer's state (tts.c); opaque.
typedef struct tts tts_t;

/// Turns text into phonemes.
/// @param text the text (ASCII)
/// @param out the phonemes, a 0-ended string
/// @param size the room in `out`
/// @return the phonemes' length, or -1 if they didn't fit (`out` then holds
///         as many whole words as fit)
int tts_phonemes(const char *text, char *out, size_t size);

/// Gets the synthesizer ready to speak phonemes (as from tts_phonemes).
/// @param phonemes the phonemes
/// @param voice the voice (NULL: the default one)
/// @param bad set to the first name that isn't a phoneme (NULL if all are);
///        such names are left out. May be NULL.
/// @return the state, or NULL if there's no memory; free with tts_end
tts_t *tts_start(const char *phonemes, const tts_voice_t *voice, const char **bad);

/// Makes the next samples of the speech.
/// @param t the state
/// @param out where they go (mono, 16 bits, TTS_RATE)
/// @param n how many are wanted
/// @return how many were made: fewer than n only at the end of the speech
size_t tts_render(tts_t *t, int16_t *out, size_t n);

/// How long the whole speech is, in samples.
/// @param t the state
/// @return its length
size_t tts_length(const tts_t *t);

/// Frees the synthesizer's state.
/// @param t the state (NULL: nothing)
void tts_end(tts_t *t);
