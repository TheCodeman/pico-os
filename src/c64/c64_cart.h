/**
 * @file c64_cart.h
 * @brief Cartridges from .crt files.
 *
 * the normal 8 KB, 16 KB and Ultimax ones, and the bank-switching Ocean,
 * System 3 (C64GS), Magic Desk and EasyFlash (its flash read only, its 256
 * bytes of RAM at $DF00) types. Portable C; the image stays where the caller
 * loaded it and the banks are read from there.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The C64 machine (see c64.h).
typedef struct c64 c64;

/// The most 8 KB banks a cartridge can have.
#define C64_CART_BANKS 64

/// A plugged-in cartridge: its type, banks and banking state.
typedef struct c64_cart {
    uint16_t type;        ///< the .crt hardware type: 0, 5, 15, 19 or 32
    char name[33];  ///< the cartridge's name, from its .crt header
    bool exrom0, game0;   ///< the lines' levels after reset (true: high, off)
    const uint8_t *lo[C64_CART_BANKS];  ///< ROML ($8000) for each bank, or NULL
    const uint8_t *hi[C64_CART_BANKS];  ///< ROMH ($A000 or $E000)
    int banks;            ///< one more than the highest bank number

    /// Now: the bank, and the lines (true: high, the cartridge's ROM off).
    uint8_t bank, control;
    bool exrom, game;  ///< the EXROM and GAME lines it drives
    uint8_t ram[256];     ///< EasyFlash
} c64_cart;

/// Errors from c64_cart_parse.
#define C64_CART_NOT_CRT     (-1)
#define C64_CART_UNSUPPORTED (-2)  ///< a type this doesn't do (the type in *type)

/// Reads a .crt image (kept where it is: the banks point into it).
int c64_cart_parse(c64_cart *c, const uint8_t *data, size_t size, int *type);
/// A name for a .crt hardware type ("Ocean", ...).
const char *c64_cart_type_name(int type);

/// Plugs the cartridge in (NULL takes it out); takes effect at once, so
/// switch the machine off and on (c64_power_on) after.
void c64_cart_attach(c64 *m, c64_cart *c);

/// For the machine (c64.c): the cartridge's I/O at $DE00-$DFFF, and its reset.
uint8_t c64_cart_io_read(c64 *m, uint16_t addr);
/// A write to the cartridge's I/O area (I/O 1 and 2, $DE00-$DFFF).
void c64_cart_io_write(c64 *m, uint16_t addr, uint8_t v);
/// Puts the cartridge back in its power-on state.
void c64_cart_reset(c64_cart *c);
