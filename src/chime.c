/**
 * @file chime.c
 * @brief The start-up chime (see chime.h).
 *
 * Each note is three sine partials (1, 2 and 3 times the pitch, the upper
 * ones quieter and fading sooner), with a 4 ms attack and an exponential
 * fade; the notes start 110 ms apart and are spread a little across the
 * stereo field.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "chime.h"

#include <math.h>
#include <stdlib.h>

/// The sample rate.
#define RATE    48000
/// Notes sounding at once.
#define VOICES  4
#define ATTACK  192  ///< frames (4 ms)

static int16_t sine[257];  ///< one cycle, and the first value again at the end

/// A note: three partials, with their envelope and pan.
static struct voice {
    /// The partials' phases and steps.
    ///@{
    uint32_t phase[3], step[3];
    ///@}
    int32_t env[3];      ///< amplitude << 15
    /// frame it starts; 0 left .. 256 right
    ///@{
    int start, pan;
    ///@}
} v[VOICES];  ///< the notes
/// Samples played so far.
static int t;
/// The chime is playing.
static bool running;

void chime_begin(void) {
    static const float notes[VOICES] = {523.25f, 659.26f, 783.99f, 1046.50f};
    static const int pans[VOICES] = {96, 160, 112, 144};
    static const int level[3] = {2600, 900, 350};  // each partial's peak
    if (!sine[64])
        for (int i = 0; i <= 256; ++i)
            sine[i] = (int16_t)lrintf(32767 * sinf(6.2831853f * i / 256));
    for (int k = 0; k < VOICES; ++k) {
        for (int p = 0; p < 3; ++p) {
            v[k].phase[p] = 0;
            v[k].step[p] = (uint32_t)(notes[k] * (p + 1) / RATE * 4294967296.0);
            v[k].env[p] = level[p] << 15;
        }
        v[k].start = k * RATE * 110 / 1000;
        v[k].pan = pans[k];
    }
    t = 0;
    running = true;
}

/// The sine table, interpolated.
/// @param phase the phase (32 bits a cycle)
/// @return the sample
static inline int32_t wave(uint32_t phase) {
    // Linear interpolation in the table: the top 8 bits pick the entry.
    int i = phase >> 24, frac = (phase >> 16) & 0xff;
    return sine[i] + (((sine[i + 1] - sine[i]) * frac) >> 8);
}

bool chime_sample(int32_t *left, int32_t *right) {
    if (!running)
        return false;
    // How fast each partial fades: amplitude loses 1/2^n a frame (the
    // fundamental's time constant is 0.34 s, the others' 0.085 and 0.043 s).
    static const int fade[3] = {14, 12, 11};
    int32_t l = 0, r = 0;
    bool sounding = false;
    for (int k = 0; k < VOICES; ++k) {
        struct voice *s = &v[k];
        int age = t - s->start;
        if (age < 0) {
            sounding = true;
            continue;
        }
        int32_t out = 0;
        for (int p = 0; p < 3; ++p) {
            out += (wave(s->phase[p]) * (s->env[p] >> 15)) >> 15;
            s->phase[p] += s->step[p];
            s->env[p] -= s->env[p] >> fade[p];
        }
        if (age < ATTACK)
            out = out * age / ATTACK;
        if (s->env[0] > (26 << 15))  // until 40 dB down
            sounding = true;
        l += out * (256 - s->pan) >> 7;
        r += out * s->pan >> 7;
    }
    ++t;
    *left += l;
    *right += r;
    if (!sounding)
        running = false;
    return running;
}
