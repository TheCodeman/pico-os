/**
 * @file radio_tone.c
 * @brief The radio's bass and treble: a low shelf at 150 Hz and a high shelf at 4 kHz on the 48 kHz sound.
 *
 * Speakers on a TV or monitor give little bass, so the bass starts a little
 * up (RADIO_BASS_DEFAULT). The shelves are RBJ's cookbook ones (slope 1) in
 * fixed point: Q28 coefficients, the samples with 8 fraction bits inside so
 * the 150 Hz shelf doesn't add noise, and what the rounding drops carried
 * into the next sample. A boost first lowers the level by half
 * of it, so loud music doesn't go into the mixer's limiter so much.
 *
 * Core 0 makes the coefficients (radio_set_tone) in the copy not in use and
 * then switches; core 1's audio refill runs the filters (radio_tone_frame).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "radio.h"

#include <math.h>
#include <string.h>

#include "hardware/sync.h"

#define TONE_Q    28        ///< the coefficients' fraction bits
#define TONE_FRAC 8         ///< the samples' fraction bits inside the filters
#define RATE      48000.0f  ///< the sound's rate (the mixer's)
#define BASS_HZ   150.0f    ///< the bass shelf's corner
#define TREBLE_HZ 4000.0f   ///< the treble shelf's

/// A shelf: b0 b1 b2 a1 a2, in Q28 (a0 is 1).
typedef struct {
    /// The coefficients.
    ///@{
    int32_t b0, b1, b2, a1, a2;
    ///@}
} shelf_t;
/// The bass and treble shelves, twice: core 0 makes a new pair in the one not in use.
static shelf_t tone[2][2];
/// The pair in use.
static volatile int tone_at;
/// Both shelves are flat: nothing to do.
static volatile bool tone_flat = true;
/// The bass and treble in dB.
///@{
static int bass_db = RADIO_BASS_DEFAULT, treble_db = 0;
///@}
/// The filters' past inputs and outputs and what the last output's
/// rounding left: [shelf][channel][x1 x2 y1 y2 e].
static int32_t tone_z[2][2][5];

/// A shelf's coefficients.
/// @param out filled in
/// @param high a high shelf (else low)
/// @param hz its corner
/// @param db its gain
/// @param pre a gain on the whole (the boost's headroom)
static void make_shelf(shelf_t *out, bool high, float hz, int db, float pre) {
    float A = powf(10.0f, db / 40.0f), w = 2.0f * 3.14159265f * hz / RATE;
    float c = cosf(w), sq = 2.0f * sqrtf(A) * sinf(w) * 0.70710678f;
    float s = high ? -1.0f : 1.0f;  // the high shelf: the low one's with cos's sign turned
    float b0 = A * ((A + 1) - s * (A - 1) * c + sq);
    float b1 = 2 * s * A * ((A - 1) - s * (A + 1) * c);
    float b2 = A * ((A + 1) - s * (A - 1) * c - sq);
    float a0 = (A + 1) + s * (A - 1) * c + sq;
    float a1 = -2 * s * ((A - 1) + s * (A + 1) * c);
    float a2 = (A + 1) + s * (A - 1) * c - sq;
    float k = (float)(1 << TONE_Q) / a0;
    out->b0 = (int32_t)lrintf(b0 * k * pre);
    out->b1 = (int32_t)lrintf(b1 * k * pre);
    out->b2 = (int32_t)lrintf(b2 * k * pre);
    out->a1 = (int32_t)lrintf(a1 * k);
    out->a2 = (int32_t)lrintf(a2 * k);
}

/// Makes the shelves for bass_db and treble_db and puts them in use (core 0).
static void tone_update(void) {
    int boost = bass_db > treble_db ? bass_db : treble_db;
    float pre = boost > 0 ? powf(10.0f, -boost / 40.0f) : 1.0f;  // half the boost, in dB
    int next = !tone_at;
    make_shelf(&tone[next][0], false, BASS_HZ, bass_db, pre);
    make_shelf(&tone[next][1], true, TREBLE_HZ, treble_db, 1.0f);
    __dmb();
    tone_at = next;
    tone_flat = !bass_db && !treble_db;
}

/// One sample through a shelf (Direct Form I). What rounding the output
/// drops goes into the next one: near 1, the bass shelf's poles would
/// otherwise make of it an offset and a hiss of several whole samples.
/// @param f the shelf
/// @param z its past values for this channel
/// @param x the input, with TONE_FRAC fraction bits
/// @return the output, the same
static inline int32_t shelf(const shelf_t *f, int32_t *z, int32_t x) {
    int64_t acc = (int64_t)f->b0 * x + (int64_t)f->b1 * z[0] + (int64_t)f->b2 * z[1] -
                  (int64_t)f->a1 * z[2] - (int64_t)f->a2 * z[3] + z[4];
    int32_t y = (int32_t)(acc >> TONE_Q);
    z[4] = (int32_t)(acc - ((int64_t)y << TONE_Q));
    z[1] = z[0];
    z[0] = x;
    z[3] = z[2];
    z[2] = y;
    return y;
}

void radio_tone_frame(int *l, int *r) {
    if (tone_flat)
        return;
    const shelf_t *t = tone[tone_at];
    int32_t vl = *l * (1 << TONE_FRAC), vr = *r * (1 << TONE_FRAC);
    vl = shelf(&t[1], tone_z[1][0], shelf(&t[0], tone_z[0][0], vl));
    vr = shelf(&t[1], tone_z[1][1], shelf(&t[0], tone_z[0][1], vr));
    *l = vl >> TONE_FRAC;
    *r = vr >> TONE_FRAC;
}

void radio_tone_reset(void) {
    memset(tone_z, 0, sizeof tone_z);
    tone_update();
}

void radio_set_tone(int bass, int treble) {
    bass_db = bass < -RADIO_TONE_MAX ? -RADIO_TONE_MAX : bass > RADIO_TONE_MAX ? RADIO_TONE_MAX : bass;
    treble_db = treble < -RADIO_TONE_MAX ? -RADIO_TONE_MAX : treble > RADIO_TONE_MAX ? RADIO_TONE_MAX : treble;
    tone_update();
}

int radio_bass(void) {
    return bass_db;
}

int radio_treble(void) {
    return treble_db;
}
