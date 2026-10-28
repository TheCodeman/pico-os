/**
 * @file swarm_snd.h
 * @brief Swarm's sound: a small synthesizer on core 1 (eight voices of square, triangle, saw and noise, with pitch sweeps, vibrato and fades) and the game's own sound effects and tunes.
 *
 * Core 0 asks for a sound with snd_fx or snd_tune; core 1 makes the samples
 * in snd_source, the audio source (audio_set_source) while the game runs.
 * The state lives where snd_start is told (in the framebuffer's free part),
 * so the game keeps no SRAM after it ends. Portable C: the PC test runs it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The sound effects.
enum {
    FX_SHOT,        ///< the fighter fires
    FX_HIT,         ///< a drone or moth blows up
    FX_BOSS_HIT,    ///< a flagship's first hit
    FX_BOSS_BOOM,   ///< a flagship blows up
    FX_PLAYER_BOOM, ///< the fighter blows up
    FX_DIVE,        ///< an alien leaves the formation to dive
    FX_BEAM,        ///< the tractor beam hums (about 3 s)
    FX_CAPTIVE_HIT, ///< the captured fighter is shot
    FX_DOCK,        ///< the rescued fighter docks
    FX_COIN,        ///< a menu click
    FX_COUNT        ///< how many
};

/// The tunes.
enum {
    TUNE_START,     ///< a new game
    TUNE_CAPTURED,  ///< the fighter has been captured
    TUNE_RESCUE,    ///< the captured fighter is free
    TUNE_CHALLENGE, ///< a challenge stage starts
    TUNE_PERFECT,   ///< every alien of a challenge stage hit
    TUNE_EXTRA,     ///< an extra fighter
    TUNE_GAME_OVER, ///< the last fighter is gone
    TUNE_HIGH,      ///< a place in the high scores
    TUNE_COUNT      ///< how many
};

/// The bytes snd_start needs for its state.
size_t snd_state_size(void);
/// Sets the synthesizer up in `mem` (snd_state_size bytes, which must stay
/// while snd_source is the audio source), silent.
/// @param mem where the state goes
void snd_start(void *mem);
/// Plays a sound effect (each has its own voice, so one starts over itself).
/// @param fx FX_ value
void snd_fx(int fx);
/// Stops a sound effect still playing (the beam when it is cut short).
/// @param fx FX_ value
void snd_fx_stop(int fx);
/// Plays a tune on the three music voices, in place of one still playing.
/// @param tune TUNE_ value (-1: stop the tune)
void snd_tune(int tune);
/// True while a tune plays.
/// @return true while a tune plays
bool snd_tune_playing(void);
/// Sound on or off (the M key); off stops everything.
/// @param on true for sound
void snd_enable(bool on);
/// The next 48 kHz stereo frame (core 1, the audio source).
/// @param left set to the left sample
/// @param right set to the right sample
/// @return true while it is the source
bool snd_source(int32_t *left, int32_t *right);
