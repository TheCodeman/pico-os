/**
 * @file ay38910.h
 * @brief An AY-3-8910/8912 sound chip: three square-wave tones, noise and the envelope, made into samples at a given rate.
 *
 * Portable C; several chips can run at once (the Mockingboard has two).
 * After the 6809 project's ay.c.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// An AY-3-8910 sound chip: its registers, tone and noise generators and envelope.
typedef struct {
    /// the chip's clock and the output rate
    ///@{
    uint32_t clock, rate;
    ///@}
    uint8_t regs[16];  ///< its registers
    /// the square wave is the top bit; inc 0: held high
    ///@{
    uint32_t tone_phase[3], tone_inc[3];
    ///@}
    /// 16.16 noise steps per sample
    ///@{
    uint32_t noise_acc, noise_inc, lfsr;
    ///@}
    /// 16.16 envelope steps per sample
    ///@{
    uint32_t env_acc, env_inc;
    ///@}
    /// the envelope's step and volume now
    ///@{
    uint8_t env_step, env_vol;
    ///@}
    /// the envelope going up, and holding
    ///@{
    bool env_up, env_holding;
    ///@}
    /// DC blocker (y in Q8)
    ///@{
    int32_t dc_x, dc_y;
    ///@}
} ay38910;

/// Silent, all registers 0 (as after the chip's reset), at clock_hz making
/// samples at rate.
/// @param a the chip
/// @param clock_hz its clock
/// @param rate the sample rate made
void ay_init(ay38910 *a, uint32_t clock_hz, uint32_t rate);
/// Writes register `reg` of the chip.
/// @param a the chip
/// @param reg the register (0-15)
/// @param value its new value
void ay_write(ay38910 *a, uint8_t reg, uint8_t value);
/// The next sample, the three channels mixed, with the DC taken out (about
/// +-16000 at full volume).
/// @param a the chip
/// @return the sample
int16_t ay_sample(ay38910 *a);
