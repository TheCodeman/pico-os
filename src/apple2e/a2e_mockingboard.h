/**
 * @file a2e_mockingboard.h
 * @brief A Mockingboard (the A or C): two 6522 VIAs in the slot's $Cn00-$CnFF space ($Cn00 and $Cn80), each driving an AY-3-8912 through its ports and interrupting the 65C02 from its timers.
 *
 * The chips' register writes go out through a callback with their cycle
 * time, for the sound to be made elsewhere (on the board, by core 1 into the
 * HDMI audio). Portable C.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "a2e.h"

/// One 6522 VIA on the Mockingboard, driving an AY-3-8910.
typedef struct {
    uint8_t orb, ora, ddrb, ddra, sr, acr, pcr, ifr, ier;  ///< the VIA's registers: ports, direction, shift, control and interrupts
    uint16_t t1_latch;  ///< timer 1's latch
    uint8_t t2_latch_lo;  ///< timer 2's latch, low byte
    /// Each timer counts down from `value` at cycle `base`; `at` is when it
    /// next passes zero (UINT64_MAX: not counting towards an interrupt).
    uint16_t t1_value, t2_value;
    uint64_t t1_base, t2_base, t1_at, t2_at;  ///< when the timers were loaded, and when they run out
    /// The sound chip on the ports: its selected register, and its registers
    /// as written (for reads, and the PC's tests).
    uint8_t ay_reg, ay_in;
    uint8_t ay_regs[16];  ///< the AY chip's registers, as last written
} a2e_via;

/// A Mockingboard sound card: two VIAs and two AY-3-8910 sound chips.
typedef struct {
    a2e *m;  ///< the machine it is in
    int slot;  ///< its slot (4)
    a2e_via via[2];  ///< its two VIAs
    /// Each sound chip register write: chip 0 (left) or 1 (right).
    void (*ay_write)(void *user, int chip, uint8_t reg, uint8_t value, uint64_t cycle);
    void *user;  ///< the host program's pointer
} a2e_mockingboard;

/// Puts the card in a slot (4 as a rule). Set ay_write and user afterwards.
void a2e_mockingboard_install(a2e *m, a2e_mockingboard *mb, int slot);
