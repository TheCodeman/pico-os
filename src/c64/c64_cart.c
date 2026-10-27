/**
 * @file c64_cart.c
 * @brief Implementation of c64_cart.h: cartridges from .crt files.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_cart.h"

#include <string.h>

#include "c64.h"

enum { NORMAL = 0, OCEAN = 5, SYSTEM3 = 15, MAGIC_DESK = 19, EASYFLASH = 32 };

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

const char *c64_cart_type_name(int type) {
    switch (type) {
    case NORMAL: return "normal";
    case OCEAN: return "Ocean";
    case SYSTEM3: return "System 3 (C64GS)";
    case MAGIC_DESK: return "Magic Desk";
    case EASYFLASH: return "EasyFlash";
    default: return "unknown";
    }
}

int c64_cart_parse(c64_cart *c, const uint8_t *data, size_t size, int *type) {
    memset(c, 0, sizeof *c);
    if (size < 0x40 || memcmp(data, "C64 CARTRIDGE   ", 16))
        return C64_CART_NOT_CRT;
    uint32_t at = be32(data + 0x10);
    c->type = be16(data + 0x16);
    *type = c->type;
    if (c->type != NORMAL && c->type != OCEAN && c->type != SYSTEM3 && c->type != MAGIC_DESK &&
        c->type != EASYFLASH)
        return C64_CART_UNSUPPORTED;
    c->exrom0 = data[0x18];
    c->game0 = data[0x19];
    memcpy(c->name, data + 0x20, 32);
    if (at < 0x40)
        at = 0x40;

    // CHIP packets: a ROM bank each, at $8000 (8 or 16 KB) or $A000/$E000.
    while (at + 16 <= size && !memcmp(data + at, "CHIP", 4)) {
        uint32_t len = be32(data + at + 4);
        uint16_t bank = be16(data + at + 10), load = be16(data + at + 12), n = be16(data + at + 14);
        const uint8_t *rom = data + at + 16;
        if (len < 16 || at + 16 + n > size)
            break;
        if (bank < C64_CART_BANKS) {
            if (load == 0x8000) {
                c->lo[bank] = rom;
                if (n > 0x2000)
                    c->hi[bank] = rom + 0x2000;
            } else if (load == 0xa000 || load == 0xe000) {
                c->hi[bank] = rom;
            }
            if (bank + 1 > c->banks)
                c->banks = bank + 1;
        }
        at += len;
    }
    if (!c->banks)
        return C64_CART_NOT_CRT;
    // Ocean's ROMH shows the same bank's second 8 KB, or mirrors ROML.
    if (c->type == OCEAN)
        for (int b = 0; b < c->banks; ++b)
            if (!c->hi[b])
                c->hi[b] = c->lo[b];
    c64_cart_reset(c);
    return 0;
}

void c64_cart_reset(c64_cart *c) {
    c->bank = 0;
    c->control = 0;
    memset(c->ram, 0, sizeof c->ram);
    if (c->type == EASYFLASH) {
        c->exrom = true;  // boots as Ultimax (the jumper in its boot position)
        c->game = false;
    } else {
        c->exrom = c->exrom0;
        c->game = c->game0;
    }
}

void c64_cart_attach(c64 *m, c64_cart *c) {
    m->cart = c;
    if (c)
        c64_cart_reset(c);
    c64_cart_changed(m);
}

uint8_t c64_cart_io_read(c64 *m, uint16_t addr) {
    c64_cart *c = m->cart;
    if (c && c->type == EASYFLASH && addr >= 0xdf00)
        return c->ram[addr & 0xff];
    return 0xff;  // open: nothing answers
}

void c64_cart_io_write(c64 *m, uint16_t addr, uint8_t v) {
    c64_cart *c = m->cart;
    if (!c)
        return;
    bool io1 = addr < 0xdf00;
    switch (c->type) {
    case OCEAN:
        if (io1)
            c->bank = v & 0x3f;
        break;
    case SYSTEM3:
        if (io1)
            c->bank = addr & 0x3f;
        break;
    case MAGIC_DESK:
        if (io1) {
            c->bank = v & 0x3f;
            c->exrom = v & 0x80;  // bit 7 switches the cartridge off
        }
        break;
    case EASYFLASH:
        if (!io1) {
            c->ram[addr & 0xff] = v;
            return;
        }
        if ((addr & 0x0f) == 0x00) {
            c->bank = v & 0x3f;
        } else if ((addr & 0x0f) == 0x02) {
            // Bit 1: EXROM low; bit 2: GAME from bit 0 (else the jumper,
            // which holds it low); bit 0: GAME low.
            c->control = v;
            c->exrom = !(v & 0x02);
            c->game = (v & 0x04) ? !(v & 0x01) : false;
        }
        break;
    default:
        return;
    }
    c64_cart_changed(m);
}
