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
    /// the sample number, effect and its parameter
    ///@{
    uint8_t sample, effect, param;
    ///@}
} mod_cell_t;

/// From the port: memory for songs and samples (PSRAM on the board).
/// @param size bytes wanted
/// @return the memory, or NULL
void *mod_alloc(size_t size);
/// Frees memory from mod_alloc.
/// @param p the memory
void mod_free(void *p);

/// A new empty song (4 channels, one pattern). False if out of memory.
/// @param s the song
/// @param channels 4, 6 or 8
/// @return false if out of memory
bool mod_new(mod_song_t *s, int channels);
/// Frees the song's samples and patterns.
/// @param s the song
void mod_clear(mod_song_t *s);  ///< frees everything
/// Loads a .MOD file's bytes. NULL if fine, else what is wrong.
/// @param s the song
/// @param data the file's bytes
/// @param len its length
/// @return NULL, or what is wrong
const char *mod_load(mod_song_t *s, const uint8_t *data, size_t len);
/// The file's size, and the file into out (that many bytes).
/// @param s the song
/// @return the file's size
size_t mod_save_size(const mod_song_t *s);
/// Writes the song as a .MOD file into `out` (mod_save_size bytes).
/// @param s the song
/// @param out where the file goes
void mod_save(const mod_song_t *s, uint8_t *out);

/// A pointer to a cell's 4 bytes in the pattern data.
/// @param s the song
/// @param pattern the pattern
/// @param row the row
/// @param ch the channel
/// @return the cell's bytes
uint8_t *mod_cell_ptr(const mod_song_t *s, int pattern, int row, int ch);
/// A cell of a pattern, unpacked.
/// @param s the song
/// @param pattern the pattern
/// @param row the row
/// @param ch the channel
/// @return the cell
mod_cell_t mod_get(const mod_song_t *s, int pattern, int row, int ch);
/// Sets a cell of a pattern.
/// @param s the song
/// @param pattern the pattern
/// @param row the row
/// @param ch the channel
/// @param c the cell
void mod_put(mod_song_t *s, int pattern, int row, int ch, mod_cell_t c);

/// Notes: 0 = C-1 ... 35 = B-3 (ProTracker's range), as periods and names.
/// @param note 0-35
/// @param finetune -8..7
/// @return the period
uint16_t mod_note_period(int note, int finetune);
/// The nearest note to a period.
/// @param period the period
/// @return the note, or -1
int mod_period_note(uint16_t period);  ///< the nearest note, -1 for none
/// A note's name.
/// @param note 0-35
/// @return "C-1", "C#1" ...
const char *mod_note_name(int note);   ///< "C-1", "C#1" ...

// ---- The player --------------------------------------------------------------------------
/// The player's state.
enum { MOD_STOPPED, MOD_SONG, MOD_PATTERN };

/// What the player is doing, for the screen.
typedef struct {
    int state;                 ///< MOD_STOPPED / MOD_SONG / MOD_PATTERN
    /// where it is
    ///@{
    int order, pattern, row;
    ///@}
    /// ticks per row, and BPM
    ///@{
    int speed, tempo;
    ///@}
    uint8_t level[MOD_MAX_CH]; ///< for VU meters, 0..64
    bool muted[MOD_MAX_CH];  ///< muted channels
    uint8_t sample[MOD_MAX_CH];  ///< what each channel plays (0: nothing), and where
    uint32_t pos[MOD_MAX_CH];  ///< where each channel is in its sample
} mod_status_t;

/// Core 0: start the song at an order position and row, or loop one pattern
/// from a row; stop; play a note now on a channel (the editor's keyboard).
/// @param s the song
/// @param order the order position
/// @param row the row
/// @param pattern_only loop this pattern only
void mod_play(mod_song_t *s, int order, int row, bool pattern_only);
/// Plays any pattern, looped.
/// @param s the song
/// @param pattern the pattern
/// @param row the row to start at
void mod_play_pattern(mod_song_t *s, int pattern, int row);  ///< any pattern, looped
/// Speed (ticks a row, 1..31) and tempo (BPM, 32..255): now if playing, and
/// what the next play starts with (normally 6 and 125). Out of range: unchanged.
/// @param speed ticks a row
/// @param tempo BPM
void mod_set_tempo(int speed, int tempo);
/// Stops the player.
void mod_stop(void);
/// Plays a note now on a channel (the editor's keyboard).
/// @param s the song
/// @param ch the channel
/// @param sample the sample
/// @param note the note
void mod_jam(mod_song_t *s, int ch, int sample, int note);
/// Mutes or unmutes a channel.
/// @param ch the channel
/// @param muted mute or unmute
void mod_mute(int ch, bool muted);
/// Sets the stereo separation.
/// @param percent 0 mono .. 100 hard Amiga panning
void mod_set_separation(int percent);  ///< stereo: 0 mono .. 100 hard Amiga panning
/// Sets what happens at the end.
/// @param loop start over (true) or stop
void mod_set_loop(bool loop);   ///< at the end: start over (the default), or stop
/// Pauses (silent and held where it is) or resumes.
/// @param paused pause or resume
void mod_pause(bool paused);    ///< silent and held where it is
/// Sets the volume.
/// @param percent 0-100
void mod_set_volume(int percent);  ///< 0..100 (the default)
/// What the player is doing.
/// @param st filled in
void mod_get_status(mod_status_t *st);
/// The last `n` (<= 256) mixed values of a channel, for its scope.
/// @param ch the channel
/// @param out where the values go
/// @param n how many (<= 256)
void mod_scope(int ch, int8_t *out, int n);
// Changing samples or the pattern count: stop the player first.

/// Core 1 (or a PC): the next stereo frame. False when silent and stopped.
/// @param left added to: the left sample
/// @param right added to: the right sample
/// @return false when silent and stopped
/// @note runs on core 1 as the audio source
bool mod_render(int32_t *left, int32_t *right);
