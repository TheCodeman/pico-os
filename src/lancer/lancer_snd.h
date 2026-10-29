/**
 * @file lancer_snd.h
 * @brief Lancer's sound: a small synthesizer on core 1 (eight voices of square, pulse, triangle, saw and noise, with pitch sweeps, vibrato and fades), the game's sound effects, and its music (stage themes that loop, and short tunes).
 *
 * Core 0 asks for a sound with lsnd_fx or lsnd_tune; core 1 makes the samples
 * in lsnd_source, the audio source (audio_set_source) while the game runs.
 * The state lives where lsnd_start is told (in the framebuffer's free part),
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
    FX_SHOT,         ///< the ship fires
    FX_LASER,        ///< the pod's weapon fires
    FX_CHARGE,       ///< the beam charging (held until lsnd_fx_stop)
    FX_LANCE,        ///< the charged beam goes
    FX_HIT,          ///< a small enemy blows up
    FX_ARMOUR,       ///< a shot bounces off armour
    FX_BOOM,         ///< a big enemy blows up
    FX_PLAYER_BOOM,  ///< the ship blows up
    FX_ORB,          ///< the pod launched or called back
    FX_ATTACH,       ///< the pod locks on
    FX_POWER,        ///< a capsule picked up
    FX_MISSILE,      ///< missiles away
    FX_WARNING,      ///< the boss is coming
    FX_BEAM,         ///< a boss's laser
    FX_EXTRA,        ///< an extra ship
    FX_COIN,         ///< a menu click
    FX_COUNT         ///< how many
};

/// The tunes (the stage themes and the boss theme loop until stopped).
enum {
    TUNE_START,      ///< a new game
    TUNE_STAGE1,     ///< the first stage's theme
    TUNE_STAGE2,     ///< the second's
    TUNE_STAGE3,     ///< the third's
    TUNE_BOSS,       ///< a boss fight
    TUNE_CLEAR,      ///< a stage cleared
    TUNE_GAME_OVER,  ///< the last ship is gone
    TUNE_HIGH,       ///< a place in the high scores
    TUNE_COUNT       ///< how many
};

/// The bytes lsnd_start needs for its state.
/// @return the size
size_t lsnd_state_size(void);
/// Sets the synthesizer up in `mem` (lsnd_state_size bytes, which must stay
/// while lsnd_source is the audio source), silent.
/// @param mem where the state goes
void lsnd_start(void *mem);
/// Plays a sound effect (each has its own voice, so one starts over itself).
/// @param fx FX_ value
void lsnd_fx(int fx);
/// Stops a sound effect still playing (the charge when fire is let go).
/// @param fx FX_ value
void lsnd_fx_stop(int fx);
/// Plays a tune on the three music voices, in place of one still playing.
/// @param tune TUNE_ value (-1: stop the tune)
void lsnd_tune(int tune);
/// True while a tune plays (always, for one that loops).
/// @return true while a tune plays
bool lsnd_tune_playing(void);
/// Sound on or off (the M key); off stops everything.
/// @param on true for sound
void lsnd_enable(bool on);
/// Music on or off (the N key); off stops the tune and ignores new ones.
/// @param on true for music
void lsnd_music(bool on);
/// The steps a part of a tune lasts, for the test (-1 for a part it hasn't).
/// @param tune TUNE_ value
/// @param part 0-2
/// @return the steps
int lsnd_part_steps(int tune, int part);
/// True if a tune loops.
/// @param tune TUNE_ value
/// @return true if it loops
bool lsnd_tune_loops(int tune);
/// The next 48 kHz stereo frame (core 1, the audio source).
/// @param left set to the left sample
/// @param right set to the right sample
/// @return true while it is the source
bool lsnd_source(int32_t *left, int32_t *right);
