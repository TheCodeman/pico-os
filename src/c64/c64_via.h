/**
 * @file c64_via.h
 * @brief A 6522 VIA, as the 1541 uses it.
 *
 * two ports with their direction registers, timer 1 (one-shot or
 * free-running) and timer 2 (one-shot), CA1 as an edge-triggered interrupt
 * input, CA2 and CB2 as outputs set by hand through the PCR, and the
 * interrupt flags and enables. The shift register only stores what is
 * written. What is wired to the ports is the drive's business: it supplies
 * the input pins through port_in.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// A 6522 VIA (see struct c64_via).
typedef struct c64_via c64_via;

/// A 6522 VIA: ports, two timers, shift register and interrupts (the 1541's two).
struct c64_via {
    uint8_t ora, orb, ddra, ddrb;  ///< ports A and B and their directions
    uint8_t ira;            ///< port A as latched (the 1541's disk byte)
    uint16_t t1, t1_latch, t2;  ///< timer 1, its latch, and timer 2
    uint8_t t2_latch_lo;  ///< timer 2's latch, low byte
    bool t1_armed, t2_armed;  ///< a one-shot timer still to interrupt
    uint8_t acr, pcr, ifr, ier, sr;  ///< control registers, interrupt flags and enables, shift register
    bool ca1;               ///< the level on CA1
    bool irq;               ///< the IRQ output (active)
    /// The pins of port B (port 1) or A (port 0) as the outside world drives
    /// them; NULL: they follow the outputs.
    uint8_t (*port_in)(void *ctx, c64_via *via, int port);
    void *ctx;  ///< passed to the port callbacks
};

/// The VIA's interrupt flag bits (IFR and IER).
enum { VIA_CA2 = 0x01, VIA_CA1 = 0x02, VIA_SR = 0x04, VIA_CB2 = 0x08, VIA_CB1 = 0x10, VIA_T2 = 0x20, VIA_T1 = 0x40 };

/// Resets the VIA.
void c64_via_reset(c64_via *v);
/// Reads VIA register `reg` (0-15).
uint8_t c64_via_read(c64_via *v, uint8_t reg);
/// Writes VIA register `reg` (0-15).
void c64_via_write(c64_via *v, uint8_t reg, uint8_t value);
/// Runs the timers for n cycles.
void c64_via_tick(c64_via *v, unsigned n);
/// CA1's level; an edge of the kind the PCR asks for sets the CA1 flag.
void c64_via_set_ca1(c64_via *v, bool level);
/// Raises interrupt flags (the drive's own events).
void c64_via_flag(c64_via *v, uint8_t bits);
/// What a port drives: its output bits, with inputs pulled high.
static inline uint8_t c64_via_out(const c64_via *v, int port) {
    return port ? (uint8_t)(v->orb | ~v->ddrb) : (uint8_t)(v->ora | ~v->ddra);
}
/// CA2 and CB2 as outputs (the PCR's manual modes): their levels.
static inline bool c64_via_ca2(const c64_via *v) {
    return (v->pcr & 0x0e) == 0x0e || (v->pcr & 0x0e) < 0x0c;
}
/// True when CB2 is an output driven high (the 1541's SOE line).
static inline bool c64_via_cb2(const c64_via *v) {
    return (v->pcr & 0xe0) == 0xe0 || (v->pcr & 0xe0) < 0xc0;
}
