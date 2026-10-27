/**
 * @file mod.h
 * @brief MOD music: the song in memory, loading and saving the .MOD file format, and a ProTracker-compatible player that mixes at 48 kHz.
 *
 * The player runs where the audio is made (core 1, from audio_task); the
 * editor and commands on core 0 talk to it with mod_play/mod_stop/mod_jam
 * and read mod_status. Everything here is plain C (tools/modtest runs it on
 * a PC against libopenmpt).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The most channels a MOD can have.
#define MOD_MAX_CH       8
/// Samples in a MOD.
#define MOD_MAX_SAMPLES  31
/// The most patterns a MOD can have.
#define MOD_MAX_PATTERNS 100
/// Rows in a pattern.
#define MOD_ROWS         64
/// The player's output rate.
#define MOD_RATE         48000

// The mixing that runs for every sound frame goes in SRAM on the board, so
// core 1 never waits for the flash cache while core 0 is busy (BASIC).
#ifndef MOD_HOT
#if defined(__arm__)
/// Puts the player's hot functions in SRAM.
#define MOD_HOT __attribute__((section(".time_critical.mod"), noinline))
#else
/// PC builds: no section.
#define MOD_HOT
#endif
#endif

/// A MOD sample: its data (8-bit), length, loop, volume and finetune.
typedef struct {
    char name[23];  ///< the sample's name
    uint32_t length;      ///< bytes (even)
    int8_t finetune;      ///< -8..7
    uint8_t volume;       ///< 0..64
    uint32_t loop_start;  ///< bytes
    uint32_t loop_len;    ///< bytes; a loop if > 2
    int8_t *data;         ///< `length` bytes, from mod_alloc
} mod_sample_t;

/// A MOD song: title, samples, order list and patterns.
typedef struct {
    char title[21];  ///< the song's title
    char sig[5];       ///< "M.K.", "6CHN", "8CHN", "FLT8" ...; "" for 15 samples
    int nsamples;      ///< 31, or 15 (old Soundtracker)
    int channels;      ///< 4, 6 or 8
    mod_sample_t samples[MOD_MAX_SAMPLES];  ///< its 31 samples
    int length;        ///< song positions in use
    int restart;  ///< the order position to start over from
    uint8_t orders[128];  ///< the order list
    int npatterns;     ///< patterns that exist (the file stores this many)
    uint8_t *patterns; ///< MOD_MAX_PATTERNS * 64 * channels * 4 bytes, raw cells
} mod_song_t;

/// A cell, decoded: period 0 = no note; sample 0 = none.
typedef struct {
    uint16_t period;  ///< the note, as an Amiga period
    uint8_t sample, effect, param;  ///< the sample number, effect and its parameter
} mod_cell_t;

/// From the port: memory for songs and samples (PSRAM on the board).
void *mod_alloc(size_t size);
/// Frees memory from mod_alloc.
void mod_free(void *p);

/// A new empty song (4 channels, one pattern). False if out of memory.
bool mod_new(mod_song_t *s, int channels);
void mod_clear(mod_song_t *s);  ///< frees everything
/// Loads a .MOD file's bytes. NULL if fine, else what is wrong.
const char *mod_load(mod_song_t *s, const uint8_t *data, size_t len);
/// The file's size, and the file into out (that many bytes).
size_t mod_save_size(const mod_song_t *s);
/// Writes the song as a .MOD file into `out` (mod_save_size bytes).
void mod_save(const mod_song_t *s, uint8_t *out);

/// A pointer to a cell's 4 bytes in the pattern data.
uint8_t *mod_cell_ptr(const mod_song_t *s, int pattern, int row, int ch);
/// A cell of a pattern, unpacked.
mod_cell_t mod_get(const mod_song_t *s, int pattern, int row, int ch);
/// Sets a cell of a pattern.
void mod_put(mod_song_t *s, int pattern, int row, int ch, mod_cell_t c);

/// Notes: 0 = C-1 ... 35 = B-3 (ProTracker's range), as periods and names.
uint16_t mod_note_period(int note, int finetune);
int mod_period_note(uint16_t period);  ///< the nearest note, -1 for none
const char *mod_note_name(int note);   ///< "C-1", "C#1" ...

// ---- The player --------------------------------------------------------------------------
/// The player's state.
enum { MOD_STOPPED, MOD_SONG, MOD_PATTERN };

/// What the player is doing, for the screen.
typedef struct {
    int state;                 ///< MOD_STOPPED / MOD_SONG / MOD_PATTERN
    int order, pattern, row;   ///< where it is
    int speed, tempo;  ///< ticks per row, and BPM
    uint8_t level[MOD_MAX_CH]; ///< for VU meters, 0..64
    bool muted[MOD_MAX_CH];  ///< muted channels
    uint8_t sample[MOD_MAX_CH];  ///< what each channel plays (0: nothing), and where
    uint32_t pos[MOD_MAX_CH];  ///< where each channel is in its sample
} mod_status_t;

/// Core 0: start the song at an order position and row, or loop one pattern
/// from a row; stop; play a note now on a channel (the editor's keyboard).
void mod_play(mod_song_t *s, int order, int row, bool pattern_only);
void mod_play_pattern(mod_song_t *s, int pattern, int row);  ///< any pattern, looped
/// Speed (ticks a row, 1..31) and tempo (BPM, 32..255): now if playing, and
/// what the next play starts with (normally 6 and 125). Out of range: unchanged.
void mod_set_tempo(int speed, int tempo);
/// Stops the player.
void mod_stop(void);
/// Plays a note now on a channel (the editor's keyboard).
void mod_jam(mod_song_t *s, int ch, int sample, int note);
/// Mutes or unmutes a channel.
void mod_mute(int ch, bool muted);
void mod_set_separation(int percent);  ///< stereo: 0 mono .. 100 hard Amiga panning
void mod_set_loop(bool loop);   ///< at the end: start over (the default), or stop
void mod_pause(bool paused);    ///< silent and held where it is
void mod_set_volume(int percent);  ///< 0..100 (the default)
/// What the player is doing.
void mod_get_status(mod_status_t *st);
/// The last `n` (<= 256) mixed values of a channel, for its scope.
void mod_scope(int ch, int8_t *out, int n);
// Changing samples or the pattern count: stop the player first.

/// Core 1 (or a PC): the next stereo frame. False when silent and stopped.
bool mod_render(int32_t *left, int32_t *right);
