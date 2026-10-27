/**
 * @file c64_cia.h
 * @brief A 6526 CIA: two 8-bit ports, timers A and B (one-shot or continuous; B can count A's underflows), the time-of-day clock with its alarm, and the interrupt control register.
 *
 * The serial shift register only stores what is written. What is wired to
 * the ports is the machine's business: it supplies the pin levels through
 * port_in.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// A 6526 CIA chip (see struct c64_cia).
typedef struct c64_cia c64_cia;

/// A 6526 CIA: ports, timers, time of day clock and interrupts.
struct c64_cia {
    /// ports A and B and their directions
    ///@{
    uint8_t pra, prb, ddra, ddrb;
    ///@}
    /// counters
    ///@{
    uint16_t ta, tb;
    ///@}
    /// reload values
    ///@{
    uint16_t ta_latch, tb_latch;
    ///@}
    /// control registers A and B
    ///@{
    uint8_t cra, crb;
    ///@}
    uint8_t icr_mask;             ///< bits 0-4: which events interrupt
    uint8_t icr_data;             ///< bits 0-4: events since the last read
    bool irq;                     ///< the IRQ output (active)
    uint8_t sdr;  ///< the serial data register
    /// Time of day, BCD: tenths, seconds, minutes, hours (bit 7 PM).
    ///@{
    uint8_t tod[4], alarm[4], tod_latch[4];
    ///@}
    bool tod_latched;  ///< reading the hours freezes what is read until the tenths
    bool tod_stopped;  ///< writing the hours stops the clock until the tenths
    /// The levels on a port's pins (port 0 is A, 1 is B), given what the CIA
    /// drives (c64_cia_out). NULL: the pins follow the outputs.
    uint8_t (*port_in)(void *ctx, c64_cia *cia, int port);
    void *ctx;  ///< passed to the port callbacks
};

/// Resets the CIA.
/// @param c the CIA
void c64_cia_reset(c64_cia *c);
/// Reads CIA register `reg` (0-15).
/// @param c the CIA
/// @param reg the register
/// @return its value
uint8_t c64_cia_read(c64_cia *c, uint8_t reg);
/// Writes CIA register `reg` (0-15).
/// @param c the CIA
/// @param reg the register
/// @param v the value
void c64_cia_write(c64_cia *c, uint8_t reg, uint8_t v);
/// Runs the timers for n cycles.
/// @param c the CIA
/// @param n cycles
void c64_cia_tick(c64_cia *c, unsigned n);
/// A tenth of a second of the time-of-day clock.
/// @param c the CIA
void c64_cia_tod_tick(c64_cia *c);
/// What the CIA drives on a port: its output bits, with inputs pulled high.
/// @param c the CIA
/// @param port 0 (A) or 1 (B)
/// @return the port's level
static inline uint8_t c64_cia_out(const c64_cia *c, int port) {
    return port ? (uint8_t)(c->prb | ~c->ddrb) : (uint8_t)(c->pra | ~c->ddra);
}
