/**
 * @file c64_sid.c
 * @brief Implementation of c64_sid.h: the C64's sound chip, a 6581 SID, made into samples a sample at a time (not a cycle at a time).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_sid.h"

#include <math.h>
#include <string.h>

// On the board the sample loop runs from SRAM: core 1 makes 48000 samples a
// second, and from flash it would compete with core 0 for the XIP cache.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#define TABLE   __attribute__((section("app_ov_c64.tables")))
#else
/// PC builds: the hot functions stay where they are.
#define FAST(f) f
/// PC builds: the tables stay in flash.
#define TABLE   const
#endif

/// The envelope's phases.
enum { ATTACK, DECAY_SUSTAIN, RELEASE };

/// Clock cycles per envelope step for each rate setting (attack; decay and
/// release step this often times the exponential divider).
static TABLE uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251,
};

/// Decay and release slow down as the envelope falls: the 6581's divider
/// thresholds.
/// @param env the envelope's level
/// @return the exponential divider's period
static inline uint8_t exp_period(uint8_t env) {
    return env > 0x5d ? 1 : env > 0x36 ? 2 : env > 0x1a ? 4 : env > 0x0e ? 8 : env > 0x06 ? 16 : env ? 30 : 1;
}

void c64_sid_init(c64_sid *s, uint32_t clock_hz, uint32_t rate) {
    memset(s, 0, sizeof *s);
    s->rate = rate;
    s->cycles_q16 = (uint32_t)(((uint64_t)clock_hz << 16) / rate);
    for (int i = 0; i < 3; ++i) {
        s->v[i].lfsr = 0x7ffff8;
        s->v[i].state = RELEASE;
    }
    s->filter_dirty = true;
}

void c64_sid_write(c64_sid *s, uint8_t reg, uint8_t value) {
    reg &= 0x1f;
    s->regs[reg] = value;
    if (reg < 21) {
        c64_sid_voice *v = &s->v[reg / 7];
        switch (reg % 7) {
        case 0: v->freq = (v->freq & 0xff00) | value; break;
        case 1: v->freq = (uint16_t)((v->freq & 0x00ff) | value << 8); break;
        case 2: v->pw = (v->pw & 0x0f00) | value; break;
        case 3: v->pw = (uint16_t)((v->pw & 0x00ff) | (value & 0x0f) << 8); break;
        case 4:
            if ((value & 1) && !(v->ctrl & 1))
                v->state = ATTACK;
            else if (!(value & 1) && (v->ctrl & 1))
                v->state = RELEASE;
            if (value & 0x08) {  // test: the oscillator held at 0, the noise reset
                v->acc = 0;
                v->lfsr = 0x7ffff8;
            }
            v->ctrl = value;
            break;
        case 5: v->ad = value; break;
        case 6: v->sr = value; break;
        }
    } else if (reg >= 0x15 && reg <= 0x17) {
        s->filter_dirty = true;
    }
}

/// Runs a voice's oscillator and envelope on by n cycles.
/// @param v the voice
/// @param n cycles
static void FAST(run_voice)(c64_sid_voice *v, uint32_t n) {
    if (!(v->ctrl & 0x08)) {
        uint32_t old = v->acc, add = (uint32_t)v->freq * n;
        // Noise is clocked each time bit 19 rises.
        uint32_t clocks = ((old + add + 0x80000) >> 20) - ((old + 0x80000) >> 20);
        while (clocks--) {
            uint32_t bit = ((v->lfsr >> 22) ^ (v->lfsr >> 17)) & 1;
            v->lfsr = ((v->lfsr << 1) | bit) & 0x7fffff;
        }
        v->acc = (old + add) & 0xffffff;
        v->msb_rose = !(old & 0x800000) && ((old + add) & 0x800000 || (old + add) > 0xffffff);
    } else {
        v->msb_rose = false;
    }

    // The envelope.
    unsigned rate = v->state == ATTACK ? v->ad >> 4 : v->state == DECAY_SUSTAIN ? v->ad & 15 : v->sr & 15;
    uint32_t period = rate_period[rate];
    uint32_t count = v->rate_count + n;
    while (count >= period) {
        count -= period;
        if (v->state == ATTACK) {
            if (v->env == 0xff || ++v->env == 0xff)
                v->state = DECAY_SUSTAIN;
            continue;
        }
        if (++v->exp_count < exp_period(v->env))
            continue;
        v->exp_count = 0;
        uint8_t sustain = (uint8_t)((v->sr >> 4) * 0x11);
        if (v->state == DECAY_SUSTAIN && v->env <= sustain)
            continue;
        if (v->env)
            --v->env;
    }
    v->rate_count = (uint16_t)count;
}

/// A voice's 12-bit waveform; ring is the modulating voice's top bit.
/// @param v the voice
/// @param ring_msb the ring modulating voice's top bit
/// @return the waveform's 12 bits
static uint32_t FAST(waveform)(const c64_sid_voice *v, bool ring_msb) {
    uint8_t w = v->ctrl >> 4;
    if (!w)
        return 0;
    uint32_t out = 0xfff, acc = v->acc;
    if (w & 1) {  // triangle
        uint32_t msb = acc & 0x800000;
        if ((v->ctrl & 0x04) && ring_msb)
            msb ^= 0x800000;
        out &= ((msb ? ~acc : acc) >> 11) & 0xfff;
    }
    if (w & 2)  // sawtooth
        out &= acc >> 12;
    if (w & 4)  // pulse
        out &= ((v->ctrl & 0x08) || (acc >> 12) >= v->pw) ? 0xfff : 0;
    if (w & 8) {  // noise
        uint32_t r = v->lfsr;
        out &= ((r >> 11) & 0x800) | ((r >> 10) & 0x400) | ((r >> 7) & 0x200) | ((r >> 5) & 0x100) |
               ((r >> 4) & 0x080) | ((r >> 1) & 0x040) | ((r << 1) & 0x020) | ((r << 2) & 0x010);
    }
    return out;
}

/// Works out the filter's coefficients from its registers.
/// @param s the SID
static void set_filter(c64_sid *s) {
    s->filter_dirty = false;
    unsigned fc = (s->regs[0x15] & 7) | s->regs[0x16] << 3;
    // The 6581's cutoff rises slowly at first, then steeply: about 220 Hz at
    // 0, 3.6 kHz half way. The filter is kept below 7 kHz, where this kind
    // of filter stays stable at 48 kHz.
    float x = fc / 2047.0f;
    float hz = 220.0f + 17780.0f * powf(x, 2.4f);
    if (hz > 7000.0f)
        hz = 7000.0f;
    s->f_q12 = (int32_t)(2.0f * sinf(3.14159265f * hz / (float)s->rate) * 4096.0f);
    unsigned res = s->regs[0x17] >> 4;
    s->q_q12 = (int32_t)((1.414f - res * 0.0776f) * 4096.0f);
}

int16_t FAST(c64_sid_sample)(c64_sid *s) {
    s->frac += s->cycles_q16;
    uint32_t n = s->frac >> 16;
    s->frac &= 0xffff;

    for (int i = 0; i < 3; ++i)
        run_voice(&s->v[i], n);
    // Hard sync: a voice restarts when the one before it (3 for 1) wraps.
    for (int i = 0; i < 3; ++i) {
        c64_sid_voice *v = &s->v[i], *src = &s->v[(i + 2) % 3];
        if ((v->ctrl & 0x02) && src->msb_rose)
            v->acc = 0;
    }

    uint8_t route = s->regs[0x17], mode = s->regs[0x18];
    int32_t filtered = 0, direct = 0;
    for (int i = 0; i < 3; ++i) {
        const c64_sid_voice *v = &s->v[i];
        bool ring = s->v[(i + 2) % 3].acc & 0x800000;
        int32_t out = ((int32_t)waveform(v, ring) - 0x800) * v->env >> 8;  // about +-2048
        if (route & (1 << i))
            filtered += out;
        else if (!(i == 2 && (mode & 0x80)))  // 3 OFF mutes voice 3 unless filtered
            direct += out;
    }

    if (s->filter_dirty)
        set_filter(s);
    int32_t high = filtered - s->low - (s->q_q12 * s->band >> 12);
    s->band += s->f_q12 * high >> 12;
    s->low += s->f_q12 * s->band >> 12;
    int32_t f = 0;
    if (mode & 0x10)
        f += s->low;
    if (mode & 0x20)
        f += s->band;
    if (mode & 0x40)
        f += high;

    // The volume, and the 6581's DC step with it (what $D418 digis play).
    unsigned vol = mode & 15;
    int32_t x = (direct + f) * (int32_t)vol / 6 + (int32_t)vol * 350;
    // DC blocker: y = x - x[-1] + 0.996 y[-1] (Q8).
    s->dc_y = (x - s->dc_x) * 256 + (s->dc_y * 255 >> 8);
    s->dc_x = x;
    int32_t y = s->dc_y >> 8;
    return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}

void c64_sid_run_voice3(c64_sid *s, uint32_t cycles) {
    // In pieces, so the noise and the multiplication stay in range.
    while (cycles) {
        uint32_t n = cycles > 4096 ? 4096 : cycles;
        run_voice(&s->v[2], n);
        cycles -= n;
    }
}

uint8_t c64_sid_osc3(const c64_sid *s) {
    return (uint8_t)(waveform(&s->v[2], s->v[1].acc & 0x800000) >> 4);
}

uint8_t c64_sid_env3(const c64_sid *s) {
    return s->v[2].env;
}
