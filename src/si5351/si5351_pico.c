/**
 * @file si5351_pico.c
 * @brief The Si5351's I2C bus on the board: BASIC's I2C port (basic_port_pico.c), which also marks the pins as taken while it is open.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "si5351_pico.h"

#include <string.h>

#include "basic_port.h"

/// The device's address, for the callbacks.
static uint8_t dev_addr = SI5351_ADDR;

/// Writes registers: the first register's number, then the data.
/// @param ctx unused
/// @param reg the first register
/// @param data the values
/// @param n their count (up to 32)
/// @return the chip took them all
static bool bus_write(void *ctx, uint8_t reg, const uint8_t *data, int n) {
    (void)ctx;
    uint8_t buf[33];
    if (n > 32)
        return false;
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return port_i2c_write(dev_addr, buf, n + 1, false) == n + 1;
}

/// Reads registers: the first register's number (no stop), then the data.
/// @param ctx unused
/// @param reg the first register
/// @param data where they go
/// @param n their count
/// @return the chip answered
static bool bus_read(void *ctx, uint8_t reg, uint8_t *data, int n) {
    (void)ctx;
    return port_i2c_write(dev_addr, &reg, 1, true) == 1 && port_i2c_read(dev_addr, data, n) == n;
}

const char *si5351_bus_open(const si5351_i2c_t *i2c, si5351_bus_t *bus) {
    const char *why = port_i2c_open(i2c->sda, i2c->scl, i2c->khz);
    if (why)
        return why;
    dev_addr = i2c->addr;
    bus->write = bus_write;
    bus->read = bus_read;
    bus->ctx = NULL;
    return NULL;
}

void si5351_bus_close(void) {
    port_i2c_close();
}
