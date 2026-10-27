/**
 * @file audio.h
 * @brief Sound over HDMI.
 *
 * Core 1 generates samples and encodes them into audio packets for the
 * scanout IRQ on core 0.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Starts the audio mixer and the HDMI sound.
void audio_init(void);

/// Core 1: tops up the HDMI audio packet queue. audio_start_refill (called
/// on core 1) runs it from a 1 ms timer interrupt there; false if it can't
/// (then call audio_task as often as possible instead).
void audio_task(void);
/// Starts core 1's 1 ms refill of the HDMI sound; false if it couldn't.
/// @return false if core 1 couldn't start the refill timer
bool audio_start_refill(void);

/// The start-up chime (chime.c): a soft arpeggio, about 1.5 s.
void audio_chime(void);
/// True while the start-up chime is playing.
/// @return true while the chime plays
bool audio_chime_playing(void);

/// Plays a sine tone; a new beep replaces one still playing.
/// @param freq_hz the tone's frequency
/// @param ms how long it plays
void audio_beep(unsigned freq_hz, unsigned ms);

/// A queue of notes played one after another in the background (BASIC's
/// PLAY). audio_play adds a tone (freq_hz 0: a rest); false if the queue is
/// full. audio_play_pending counts the notes not yet finished.
#define AUDIO_QUEUE_LEN 64
/// Queues a tone of `freq_hz` for `ms` milliseconds; false if the queue is full.
/// @param freq_hz the tone's frequency (0: a rest)
/// @param ms how long it plays
/// @return false if the queue is full
bool audio_play(unsigned freq_hz, unsigned ms);
/// How many queued tones haven't finished.
/// @return the tones queued or playing
int audio_play_pending(void);
/// Stops queued tones.
void audio_play_stop(void);

/// A one-bit speaker like the Apple II's: each click flips the cone, at
/// time_us_64() time `when_us` (now or a little ahead). The clicks are played
/// a fixed 40 ms later, so they come out exactly as spaced.
/// @param when_us when the click happens, in time_us_64() terms
void audio_speaker_click(uint64_t when_us);

/// Two AY-3-8910 sound chips (a Mockingboard's), chip 0 on the left and 1 on
/// the right, clocked at clock_hz. audio_ay_start resets them and starts
/// mixing them in; audio_ay_write changes a register at time_us_64() time
/// `when_us`, played (like the speaker) a fixed 40 ms later.
/// @param clock_hz the chips' clock (1.023 MHz on the Apple II)
void audio_ay_start(uint32_t clock_hz);
/// Stops mixing in the AY-3-8910 chips.
void audio_ay_stop(void);
/// Frees the speaker's and the AY chips' event queues once no program is
/// using them (they are made again on next use), so the shell keeps none of
/// that SRAM. Called after each command.
void audio_trim(void);
/// Queues a write to AY chip `chip`'s register, applied at `when_us`.
/// @param chip 0 (left) or 1 (right)
/// @param reg the register (0-15)
/// @param value its new value
/// @param when_us when, in time_us_64() terms
void audio_ay_write(unsigned chip, uint8_t reg, uint8_t value, uint64_t when_us);
/// The chips' level: 256 is as they come (the default is 512).
/// @param q8 the level, 256 = 1.0
void audio_ay_gain(unsigned q8);

/// The C64's SID (c64_sid.c), clocked at clock_hz, in the middle of both
/// channels. audio_sid_start resets it and starts mixing it in;
/// audio_sid_write changes a register at time_us_64() time when_us, played
/// (like the speaker) a fixed 40 ms later.
/// @param clock_hz the SID's clock (985248 Hz on a PAL C64)
void audio_sid_start(uint32_t clock_hz);
/// Stops the SID emulation.
void audio_sid_stop(void);
/// Queues a write to the SID's register, applied at `when_us`.
/// @param reg the register (0-24)
/// @param value its new value
/// @param when_us when, in time_us_64() terms
void audio_sid_write(uint8_t reg, uint8_t value, uint64_t when_us);
/// The SID's level: 256 is as it comes (the default).
/// @param q8 the level, 256 = 1.0
void audio_sid_gain(unsigned q8);
/// How much of core 1's time making the SID's samples takes, in tenths of a
/// percent, since audio_sid_start.
/// @return tenths of a percent
unsigned audio_sid_load(void);

/// A sound source mixed in on core 1 (the MOD player): fn gives the next
/// stereo frame at 48 kHz. NULL removes it.
/// @param fn the source, called on core 1 for each frame (NULL: none)
/// @note fn runs in core 1's audio refill: it must be quick and in SRAM
void audio_set_source(bool (*fn)(int32_t *left, int32_t *right));
/// The source now (NULL if none): radio stops when another one takes over.
bool (*audio_get_source(void))(int32_t *left, int32_t *right);
/// How much of core 1's time the source took since the last call, in tenths
/// of a percent.
/// @return tenths of a percent
unsigned audio_source_load(void);
