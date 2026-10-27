/**
 * @file si4703_pico.c
 * @brief The Si4703 on the board: BASIC's I2C port (which marks its pins as taken while open) and the reset pin.
 *
 * The chip picks its bus mode as RST rises: SDIO held low then means I2C.
 * The reset pin is a plain GPIO, left driven high while the radio plays so
 * the chip keeps going after the panel closes; `fm off` lets it go low.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "si4703_pico.h"

#include <string.h>

#include "basic_port.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

/// Where the chip is, for the callbacks.
static si4703_pins_t pins = {2, 3, 400, 22};

/// Reads the 16 registers: the chip sends 0x0A-0x0F, then 0x00-0x09.
/// @param ctx unused
/// @param regs filled in
/// @return the chip answered
static bool bus_read(void *ctx, uint16_t regs[16]) {
    (void)ctx;
    uint8_t b[32];
    if (port_i2c_read(SI4703_ADDR, b, 32) != 32)
        return false;
    for (int i = 0; i < 16; ++i)
        regs[(0x0a + i) & 15] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    return true;
}

/// Writes registers from 0x02 on (the chip takes no register number).
/// @param ctx unused
/// @param from_02 the values
/// @param n how many (up to 14)
/// @return the chip took them
static bool bus_write(void *ctx, const uint16_t *from_02, int n) {
    (void)ctx;
    uint8_t b[28];
    if (n > 14)
        return false;
    for (int i = 0; i < n; ++i) {
        b[2 * i] = (uint8_t)(from_02[i] >> 8);
        b[2 * i + 1] = (uint8_t)from_02[i];
    }
    return port_i2c_write(SI4703_ADDR, b, 2 * n, false) == 2 * n;
}

/// Resets the chip into I2C mode: SDIO low while RST rises (the I2C pins
/// are then given back to the I2C controller).
/// @param ctx unused
static void bus_reset(void *ctx) {
    (void)ctx;
    port_i2c_close();
    gpio_init(pins.sda);
    gpio_set_dir(pins.sda, GPIO_OUT);
    gpio_put(pins.sda, 0);
    gpio_init(pins.rst);
    gpio_set_dir(pins.rst, GPIO_OUT);
    gpio_put(pins.rst, 0);
    sleep_ms(2);
    gpio_put(pins.rst, 1);
    sleep_ms(2);
    port_i2c_open(pins.sda, pins.scl, pins.khz);
}

/// Waits.
/// @param ctx unused
/// @param ms milliseconds
static void bus_delay(void *ctx, unsigned ms) {
    (void)ctx;
    sleep_ms(ms);
}

const char *si4703_bus_open(const si4703_pins_t *p, si4703_bus_t *bus) {
    if (p->rst < 0 || p->rst >= 48 || p->rst == p->sda || p->rst == p->scl)
        return "the reset pin must be another GPIO (e.g. GP22)";
    const char *why = port_pin_reserved(p->rst);
    if (why)
        return why;
    why = port_i2c_open(p->sda, p->scl, p->khz);
    if (why)
        return why;
    pins = *p;
    // The reset pin high (the chip running), unless it is already: then
    // leave it alone, so a radio left playing goes on.
    if (gpio_get_function(pins.rst) != GPIO_FUNC_SIO || !gpio_is_dir_out(pins.rst)) {
        gpio_init(pins.rst);
        gpio_set_dir(pins.rst, GPIO_OUT);
        gpio_put(pins.rst, 1);
    }
    bus->read = bus_read;
    bus->write = bus_write;
    bus->reset = bus_reset;
    bus->delay_ms = bus_delay;
    bus->ctx = NULL;
    return NULL;
}

void si4703_bus_close(bool keep_running) {
    port_i2c_close();
    if (!keep_running) {  // the chip held in reset: off, and the pin let go
        gpio_put(pins.rst, 0);
        gpio_set_dir(pins.rst, GPIO_IN);
        gpio_pull_down(pins.rst);
    }
}
