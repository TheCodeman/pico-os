/**
 * @file si5351_pico.h
 * @brief The Si5351 clock generator on the board: its I2C bus, and the `si5351` command.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "si5351.h"

/// Where the chip is: the I2C pins, speed and address.
typedef struct {
    int sda;   ///< SDA pin (even; I2C0 or I2C1 follows from it)
    int scl;   ///< SCL pin (sda + 1)
    int khz;   ///< bus speed
    int addr;  ///< the chip's address (0x60)
} si5351_i2c_t;

/// Opens the I2C bus (taking its pins until si5351_bus_close) and sets up
/// the callbacks for the chip.
/// @param i2c where the chip is
/// @param bus set to the callbacks
/// @return NULL, or why the bus can't be opened
const char *si5351_bus_open(const si5351_i2c_t *i2c, si5351_bus_t *bus);

/// Closes the bus and lets its pins go (the chip keeps running as set).
void si5351_bus_close(void);

/// The `si5351` (or `clockgen`) command: the graphical panel with no
/// arguments, else `set`, `on`, `off`, `status` and the like for scripts.
/// @param argc the argument count
/// @param argv the arguments
void si5351_command(int argc, char **argv);
