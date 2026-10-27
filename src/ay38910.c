/**
 * @file ay38910.c
 * @brief The AY-3-8910's tone, noise and envelope generators, a sample at a time.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "ay38910.h"

#include <string.h>

/// The chip's output levels as measured on a real AY-3-8910 (the steps are
/// smaller near the top than a plain 3 dB each, so middle volumes are louder).
static const int16_t vol_table[16] = {
    0, 53, 75, 111, 160, 233, 333, 508, 619, 1017, 1485, 1769, 2484, 3032, 3922, 5000,
};
static const uint8_t reg_mask[16] = {
    0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff,
};

static void update_tone(ay38910 *a, unsigned ch) {
    uint32_t tp = a->regs[2 * ch] | (uint32_t)a->regs[2 * ch + 1] << 8;
    if (tp == 0)
        tp = 1;
    uint64_t inc = ((uint64_t)a->clock << 32) / (16ull * tp * a->rate);
    // At or above half the sample rate a square wave only aliases: hold it
    // high, so it acts as a plain level.
    a->tone_inc[ch] = inc >= 0x80000000ull ? 0 : (uint32_t)inc;
    if (!a->tone_inc[ch])
        a->tone_phase[ch] = 0;
}

/// 16.16 steps per sample for a generator stepping every 16 * period clocks.
static uint32_t steps_inc(const ay38910 *a, uint32_t period) {
    if (period == 0)
        period = 1;
    return (uint32_t)(((uint64_t)a->clock << 16) / (16ull * period * a->rate));
}

static void env_restart(ay38910 *a) {
    a->env_step = 0;
    a->env_acc = 0;
    a->env_up = a->regs[13] & 4;  // ATTACK
    a->env_holding = false;
    a->env_vol = a->env_up ? 0 : 15;
}

void ay_write(ay38910 *a, uint8_t reg, uint8_t value) {
    reg &= 15;
    a->regs[reg] = value & reg_mask[reg];
    switch (reg) {
    case 0: case 1: update_tone(a, 0); break;
    case 2: case 3: update_tone(a, 1); break;
    case 4: case 5: update_tone(a, 2); break;
    case 6: a->noise_inc = steps_inc(a, a->regs[6]); break;
    case 11: case 12: a->env_inc = steps_inc(a, a->regs[11] | (uint32_t)a->regs[12] << 8); break;
    case 13: env_restart(a); break;
    }
}

void ay_init(ay38910 *a, uint32_t clock_hz, uint32_t rate) {
    memset(a, 0, sizeof *a);
    a->clock = clock_hz;
    a->rate = rate;
    a->lfsr = 1;
    for (uint8_t r = 0; r < 16; ++r)
        ay_write(a, r, 0);
}

/// The envelope's next level. Shape bits: CONTINUE ATTACK ALTERNATE HOLD.
static unsigned envelope(ay38910 *a) {
    if (a->env_holding)
        return a->env_vol;
    a->env_acc += a->env_inc;
    while (a->env_acc >= 0x10000u && !a->env_holding) {
        a->env_acc -= 0x10000u;
        if (++a->env_step < 16) {
            a->env_vol = a->env_up ? a->env_step : 15 - a->env_step;
            continue;
        }
        // The end of a 16-step ramp.
        uint8_t s = a->regs[13];
        if (!(s & 8)) {
            a->env_holding = true;
            a->env_vol = 0;
        } else if (s & 1) {
            a->env_holding = true;
            a->env_vol = (((s >> 2) ^ (s >> 1)) & 1) ? 15 : 0;
        } else {
            if (s & 2)
                a->env_up = !a->env_up;
            a->env_step = 0;
            a->env_vol = a->env_up ? 0 : 15;
        }
    }
    return a->env_vol;
}

int16_t ay_sample(ay38910 *a) {
    // Noise: a 17-bit LFSR stepped at clock / (16 * period).
    a->noise_acc += a->noise_inc;
    while (a->noise_acc >= 0x10000u) {
        a->noise_acc -= 0x10000u;
        uint32_t bit = (a->lfsr ^ (a->lfsr >> 3)) & 1;
        a->lfsr = (a->lfsr >> 1) | (bit << 16);
    }
    bool noise = a->lfsr & 1;
    unsigned env = envelope(a);
    uint8_t mixer = a->regs[7];
    int32_t out = 0;
    for (unsigned ch = 0; ch < 3; ++ch) {
        bool tone = a->tone_inc[ch] ? (a->tone_phase[ch] >> 31) : true;
        a->tone_phase[ch] += a->tone_inc[ch];
        bool on = (tone || (mixer >> ch & 1)) && (noise || (mixer >> (ch + 3) & 1));
        uint8_t v = a->regs[8 + ch];
        if (on)
            out += vol_table[v & 0x10 ? env : v & 15];
    }
    // y = x - x[-1] + 0.996 y[-1], as the speaker does.
    int32_t x = out;
    a->dc_y = ((x - a->dc_x) << 8) + (a->dc_y * 255) / 256;
    a->dc_x = x;
    int32_t y = a->dc_y >> 8;
    return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}
