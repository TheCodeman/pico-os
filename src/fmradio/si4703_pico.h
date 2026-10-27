/**
 * @file si4703_pico.h
 * @brief The Si4703 FM radio on the board: its I2C bus and reset pin, and the `fm` command.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "si4703.h"

/// Where the chip is: the I2C pins and speed, and the reset pin.
typedef struct {
    int sda;   ///< SDA (SDIO) pin (even; I2C0 or I2C1 follows from it)
    int scl;   ///< SCL (SCLK) pin (sda + 1)
    int khz;   ///< bus speed
    int rst;   ///< the reset (RST) pin
} si4703_pins_t;

/// Opens the I2C bus (taking its pins) and drives the reset pin high (if it
/// isn't already: a radio left playing goes on).
/// @param pins where the chip is
/// @param bus set to the callbacks
/// @return NULL, or why the bus can't be opened
const char *si4703_bus_open(const si4703_pins_t *pins, si4703_bus_t *bus);

/// Closes the bus. keep_running leaves the reset pin high (the radio plays
/// on); otherwise it goes low, holding the chip off, and is let go.
/// @param keep_running the radio plays on
void si4703_bus_close(bool keep_running);

/// The `fm` (or `fmradio`) command: the graphical panel with no arguments,
/// else `tune`, `seek`, `scan`, `vol`, `off`, `status` and the like.
/// @param argc the argument count
/// @param argv the arguments
void fm_command(int argc, char **argv);
