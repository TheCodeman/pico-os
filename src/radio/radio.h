/**
 * @file radio.h
 * @brief Internet radio.
 *
 * MP3 and AAC (ADTS; HE-AAC as its AAC core) streams over HTTP or HTTPS
 * (Icecast/Shoutcast, with their song titles), decoded on core 1 into the
 * HDMI sound; and the `radio` command with its graphical station browser
 * (radio_ui.c).
 *
 * The stream is pumped into a few seconds' buffer in PSRAM by a worker in
 * lwIP's own context, and decoded by the audio source on core 1, so it goes
 * on playing after the browser closes, until `radio stop` (or another
 * program takes the sound).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The radio's states.
enum { RADIO_OFF, RADIO_CONNECTING, RADIO_BUFFERING, RADIO_PLAYING, RADIO_ENDED, RADIO_ERROR };

/// A station: name, address, bitrate, country and codec.
typedef struct {
    char name[64];  ///< the station's name
    char url[200];  ///< its stream address
    uint16_t bitrate;   ///< kbit/s, 0 unknown
    char country[4];  ///< its country code
    char codec[8];  ///< its codec: "MP3", "AAC" or "AAC+"
} radio_station_t;

/// What the radio is doing, for the screen and `radio status`.
typedef struct {
    int state;  ///< RADIO_ state
    char station[64];     ///< the station's name (from the list, or its icy-name)
    char title[128];      ///< the song, from the stream's StreamTitle
    char url[200];  ///< the stream's address
    char error[96];       ///< why it stopped (RADIO_ERROR, RADIO_ENDED)
    char codec[8];        ///< "MP3", "AAC" or "HE-AAC" (24 kHz or less: its core)
    int bitrate;          ///< kbit/s from the stream
    int samplerate, channels;  ///< the stream's sample rate and channels
    int buffer_ms;        ///< compressed audio waiting
    int buffer_pct;       ///< of the buffer
    uint32_t underruns;   ///< times the buffer ran dry
    uint8_t level[2];     ///< 0..64, left and right, for meters
    unsigned load;        ///< core 1's time decoding, tenths of a percent
    bool decoder_in_sram;  ///< the decoder is in SRAM (the apps' shared area)
    int decoder_sram_kb;  ///< of its 24 KB
} radio_status_t;

/// Connects to a stream (following redirects and .pls/.m3u playlists) and
/// starts it playing. Blocks while connecting; Esc gives up. False (with
/// st.error set) if it can't.
bool radio_play(const char *url, const char *name);
/// Stops the radio and frees its memory.
void radio_stop(void);
bool radio_active(void);  ///< connecting, buffering or playing
/// What the radio is doing now.
void radio_get_status(radio_status_t *st);
/// Sets the volume, 0-100%.
void radio_set_volume(int percent);
/// The volume, 0-100%.
int radio_volume(void);
void radio_set_mute(bool on);  ///< silent, but still playing (the volume is kept)
/// True if the radio is muted.
bool radio_muted(void);
/// Ctrl+Alt+M from the keyboard (core 1): toggles mute if the radio is
/// playing. radio_note_poll (core 0, from the keyboard's input) shows a note
/// at the top right for a moment, putting back what was under it.
void radio_hotkey_mute(void);
/// Shows (and later takes away) the mute note at the top right; call from core 0.
void radio_note_poll(void);

/// Fetches a small HTTP(S) document (a playlist, a station search) into out
/// (up to max - 1 bytes, 0-terminated). The body's length, or -1 with why in
/// *err.
int radio_http_get(const char *url, char *out, int max, const char **err);

/// radio_browser.c: searches radio-browser.info (MP3 and AAC stations, most listened
/// first). `what` is a name, or "tag:jazz", "country:DE", "lang:french".
/// Returns the number found (at most max), or -1 with why in *err.
int radio_search(const char *what, radio_station_t *out, int max, const char **err);

/// radio_ui.c: the shell command.
void radio_command(int argc, char **argv);
