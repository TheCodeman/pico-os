/**
 * @file c64_sid.h
 * @brief The C64's sound chip, a 6581 SID, made into samples a sample at a time (not a cycle at a time).
 *
 * three voices with triangle, sawtooth, pulse and noise (several at once
 * ANDed together, as near as cheaply possible), ring modulation and hard
 * sync, the ADSR envelopes with the chip's rate table and exponential decay,
 * the multimode filter (a state-variable filter with a 6581-like cutoff
 * curve), the master volume with the 6581's DC step that makes $D418 digis
 * audible, and voice 3's oscillator and envelope for reading back. Portable
 * C: the board runs it on core 1, and the C64 keeps a second one on core 0
 * just for $D41B/$D41C.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// One SID voice: oscillator, waveform, ADSR envelope and its registers.
typedef struct {
    uint32_t acc;        ///< 24-bit phase
    uint32_t lfsr;       ///< 23-bit noise shift register
    uint16_t freq, pw;  ///< frequency and pulse width
    uint8_t ctrl;        ///< $D404: noise pulse saw tri, test, ring, sync, gate
    uint8_t ad, sr;  ///< attack/decay and sustain/release
    uint8_t env;         ///< 0-255
    uint8_t state;       ///< attack, decay/sustain, release
    uint16_t rate_count;  ///< the envelope's rate counter
    uint8_t exp_count;  ///< the envelope's exponential counter
    bool msb_rose;       ///< the phase's top bit went up this step (for sync)
} c64_sid_voice;

/// The 6581 SID: three voices, the filter and the volume.
typedef struct {
    c64_sid_voice v[3];  ///< the three voices
    uint8_t regs[0x20];  ///< the registers, as last written
    uint32_t cycles_q16;      ///< clock cycles per sample, 16.16
    uint32_t frac;            ///< the fraction carried between samples
    uint32_t rate;  ///< the output rate
    /// Filter: state-variable, Q12 coefficients.
    int32_t low, band;
    int32_t f_q12, q_q12;  ///< the filter's cutoff and resonance (Q12)
    bool filter_dirty;        ///< the coefficients need working out again
    /// Output DC blocker (keeps the volume's DC step as a click, so digis play).
    int32_t dc_x, dc_y;
} c64_sid;

/// Silent, all registers 0, at clock_hz making samples at rate.
void c64_sid_init(c64_sid *s, uint32_t clock_hz, uint32_t rate);
/// Writes SID register `reg` (0-24).
void c64_sid_write(c64_sid *s, uint8_t reg, uint8_t value);
/// The next sample, about +-16000 at full volume.
int16_t c64_sid_sample(c64_sid *s);

/// Voice 3 alone, run on by cycles clock cycles (for the readback copy).
void c64_sid_run_voice3(c64_sid *s, uint32_t cycles);
/// $D41B: voice 3's waveform's top 8 bits; $D41C: its envelope.
uint8_t c64_sid_osc3(const c64_sid *s);
/// Voice 3's envelope level, as $D41C reads it.
uint8_t c64_sid_env3(const c64_sid *s);
