/**
 * @file xm.h
 * @brief XM music (FastTracker 2): the song in memory, loading the .XM file format, and a player that behaves as FT2 does (as libopenmpt plays XMs), mixing 48 kHz stereo in blocks.
 *
 * Plain C like mod.c: the PC tests run it.
 *
 * Notes in patterns: 0 none, 1..96 C-0..B-7, 97 key off. Instruments
 * 1..128. The volume column as the file has it (0x10-0x50 volume, 0x60-0xff
 * commands).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The most channels an XM can have.
#define XM_MAX_CH      32
/// The most patterns an XM can have.
#define XM_MAX_PATTERNS 256
/// The most instruments an XM can have.
#define XM_MAX_INSTR   128
/// The player's output rate.
#define XM_RATE        48000

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

/// An XM sample: data (8 or 16-bit), length, loop, volume, panning, finetune and relative note.
typedef struct {
    char name[23];  ///< the sample's name
    int16_t *data;        ///< 16-bit, whatever the file had (8-bit is scaled up)
    uint32_t length;      ///< in samples
    /// the loop's start and length, in samples
    ///@{
    uint32_t loop_start, loop_len;
    ///@}
    uint8_t loop;         ///< 0 none, 1 forward, 2 ping-pong
    bool bits16;          ///< as stored in the file (for saving it back)
    uint8_t volume;       ///< 0..64
    int8_t finetune;      ///< -128..127
    uint8_t pan;          ///< 0..255
    int8_t relnote;  ///< notes above (or below) the instrument's note
    uint8_t reserved;     ///< as the file had it
} xm_sample_t;

/// A point of an envelope: tick and value.
typedef struct {
    /// tick, value 0..64
    ///@{
    uint16_t x, y;
    ///@}
} xm_point_t;

/// An envelope: its points, sustain and loop.
typedef struct {
    xm_point_t points[12];  ///< the envelope's points
    /// points used, the sustain point and the loop's ends
    ///@{
    uint8_t count, sustain, loop_start, loop_end;
    ///@}
    uint8_t flags;        ///< 1 on, 2 sustain, 4 loop
} xm_env_t;

/// An XM instrument: its samples, note map, envelopes and vibrato.
typedef struct {
    char name[23];  ///< the instrument's name
    uint8_t keymap[96];   ///< the sample for each note (0-based)
    int nsamples;  ///< its samples
    xm_sample_t *samples; ///< nsamples of them (from xm_alloc)
    /// its volume and panning envelopes
    ///@{
    xm_env_t venv, penv;
    ///@}
    /// its vibrato: type, sweep, depth and rate
    ///@{
    uint8_t vib_type, vib_sweep, vib_depth, vib_rate;
    ///@}
    uint16_t fadeout;  ///< how fast notes fade after release
    uint8_t type;         ///< as the file had them, to write back the same
    /// the sizes its file gave (kept for saving)
    ///@{
    uint32_t header_size, sample_header_size;
    ///@}
} xm_instrument_t;

/// One cell of an XM pattern: note, instrument, volume column, effect and parameter.
typedef struct {
    /// note, instrument, volume column, effect and parameter
    ///@{
    uint8_t note, instrument, volume, effect, param;
    ///@}
} xm_cell_t;

/// An XM pattern: its rows of cells.
typedef struct {
    uint16_t rows;  ///< its rows
    xm_cell_t *cells;     ///< rows * channels (from xm_alloc)
} xm_pattern_t;

/// An XM song: its settings, order list, patterns and instruments.
typedef struct {
    char title[21];  ///< the song's title
    char tracker[21];  ///< the tracker that wrote it
    uint16_t version;  ///< the file's version
    /// the order list's length, restart position, channels, patterns and instruments
    ///@{
    int length, restart, channels, npatterns, ninstruments;
    ///@}
    bool linear;          ///< linear frequencies (else Amiga periods)
    /// the starting speed and BPM
    ///@{
    int speed, bpm;
    ///@}
    uint8_t orders[256];  ///< the order list
    xm_pattern_t patterns[XM_MAX_PATTERNS];  ///< the patterns
    xm_instrument_t *instruments;  ///< ninstruments of them (from xm_alloc)
} xm_song_t;

/// From the port: memory for songs and samples (PSRAM on the board).
void *xm_alloc(size_t size);
/// Frees memory from xm_alloc.
void xm_free(void *p);

/// Loads a .XM file's bytes. NULL if fine, else what is wrong.
const char *xm_load(xm_song_t *s, const uint8_t *data, size_t len);
/// Frees the song's patterns and instruments.
void xm_clear(xm_song_t *s);
/// The file's size, and the file into out (that many bytes): patterns are
/// packed as FT2 packs them, samples delta-coded in their own bit depth.
size_t xm_save_size(const xm_song_t *s);
/// Writes the song as a .XM file into `out` (xm_save_size bytes).
void xm_save(const xm_song_t *s, uint8_t *out);
/// True if the bytes are an XM file.
bool xm_is_xm(const uint8_t *data, size_t len);
/// Instrument files (.XI), as FT2 writes them: one instrument and its samples.
bool xm_is_xi(const uint8_t *data, size_t len);
const char *xm_xi_load(xm_instrument_t *in, const uint8_t *data, size_t len);  ///< free: its samples
/// The size of an instrument as a .XI file.
size_t xm_xi_size(const xm_instrument_t *in);
/// Writes an instrument as a .XI file into `out` (xm_xi_size bytes).
void xm_xi_save(const xm_instrument_t *in, uint8_t *out);

// ---- The player ---------------------------------------------------------------------------
/// What the XM player is doing, for the screen.
typedef struct {
    bool playing;  ///< playing now
    /// where it is, and the speed and BPM
    ///@{
    int order, pattern, row, speed, bpm;
    ///@}
    int global_volume;     ///< 0..64
    int active;            ///< voices sounding
    uint8_t level[XM_MAX_CH];  ///< each channel's level now, 0..64
} xm_status_t;

/// Core 0: start at an order position (and row), stop, settings.
/// The player's voices (8 KB) are made on the first play and kept until
/// xm_release (after the audio source is taken away): false if no memory.
bool xm_play(xm_song_t *s, int order, int row);
void xm_play_pattern(xm_song_t *s, int pattern, int row);  ///< one pattern, over and over
void xm_jam(xm_song_t *s, int ch, int instrument, int note);  ///< a note now (the keyboard)
/// Stops the player.
void xm_stop(void);
void xm_release(void);            ///< frees the voices (nothing may be rendering)
void xm_set_loop(bool loop);     ///< at the end: start over (default) or stop
/// Mutes or unmutes a channel.
void xm_mute(int ch, bool muted);
/// True if the channel is muted.
bool xm_muted(int ch);
/// Pauses or resumes the player.
void xm_pause(bool paused);
void xm_set_volume(int percent); ///< 0..100
/// What the player is doing.
void xm_get_status(xm_status_t *st);

/// Core 1 (or a PC): n stereo frames added into out (left, right, ...),
/// at a level like mod_render's. False once stopped and silent.
bool xm_render(int32_t *out, int n);
/// The same a frame at a time (for audio_set_source), from blocks of 32.
bool xm_render_frame(int32_t *left, int32_t *right);
