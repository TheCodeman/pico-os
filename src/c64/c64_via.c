/**
 * @file c64_via.c
 * @brief Implementation of c64_via.h: a 6522 VIA, as the 1541 uses it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_via.h"

#include <string.h>

static void update_irq(c64_via *v) {
    v->ifr &= 0x7f;
    v->irq = (v->ifr & v->ier & 0x7f) != 0;
    if (v->irq)
        v->ifr |= 0x80;
}

void c64_via_flag(c64_via *v, uint8_t bits) {
    v->ifr |= bits;
    update_irq(v);
}

static void clear(c64_via *v, uint8_t bits) {
    v->ifr &= ~bits;
    update_irq(v);
}

void c64_via_reset(c64_via *v) {
    uint8_t (*port_in)(void *, c64_via *, int) = v->port_in;
    void *ctx = v->ctx;
    memset(v, 0, sizeof *v);
    v->port_in = port_in;
    v->ctx = ctx;
    v->t1 = v->t1_latch = v->t2 = 0xffff;
}

void c64_via_tick(c64_via *v, unsigned n) {
    // Timer 1: counts down to 0 and then past it; free-running it reloads
    // from the latch and interrupts each time, one-shot only the first.
    if (n > v->t1) {
        unsigned left = n - v->t1 - 1;  // counts after the underflow
        if (v->acr & 0x40) {
            unsigned period = v->t1_latch + 2u;
            v->t1 = (uint16_t)(v->t1_latch - left % period);
            c64_via_flag(v, VIA_T1);
        } else {
            v->t1 = (uint16_t)(0xffff - left);
            if (v->t1_armed) {
                v->t1_armed = false;
                c64_via_flag(v, VIA_T1);
            }
        }
    } else {
        v->t1 -= n;
    }
    // Timer 2: one-shot, counting clock cycles.
    if (!(v->acr & 0x20)) {
        if (n > v->t2) {
            v->t2 = (uint16_t)(v->t2 - n);
            if (v->t2_armed) {
                v->t2_armed = false;
                c64_via_flag(v, VIA_T2);
            }
        } else {
            v->t2 -= n;
        }
    }
}

void c64_via_set_ca1(c64_via *v, bool level) {
    if (level == v->ca1)
        return;
    v->ca1 = level;
    if (level == ((v->pcr & 0x01) != 0))  // PCR bit 0: 1 for a rising edge
        c64_via_flag(v, VIA_CA1);
}

static uint8_t port(c64_via *v, int p) {
    return v->port_in ? v->port_in(v->ctx, v, p) : c64_via_out(v, p);
}

uint8_t c64_via_read(c64_via *v, uint8_t reg) {
    switch (reg & 0x0f) {
    case 0x0: {  // port B: outputs as written, inputs from the pins
        clear(v, VIA_CB1 | VIA_CB2);
        return (uint8_t)((v->orb & v->ddrb) | (port(v, 1) & ~v->ddrb));
    }
    case 0x1:
        clear(v, VIA_CA1 | VIA_CA2);
        /* fall through */
    case 0xf:
        if (v->acr & 0x01)  // latched on CA1
            return v->ira;
        return (uint8_t)((v->ora & v->ddra) | (port(v, 0) & ~v->ddra));
    case 0x2: return v->ddrb;
    case 0x3: return v->ddra;
    case 0x4:
        clear(v, VIA_T1);
        return v->t1 & 0xff;
    case 0x5: return v->t1 >> 8;
    case 0x6: return v->t1_latch & 0xff;
    case 0x7: return v->t1_latch >> 8;
    case 0x8:
        clear(v, VIA_T2);
        return v->t2 & 0xff;
    case 0x9: return v->t2 >> 8;
    case 0xa:
        clear(v, VIA_SR);
        return v->sr;
    case 0xb: return v->acr;
    case 0xc: return v->pcr;
    case 0xd: return v->ifr;
    default: return v->ier | 0x80;  // 0xe
    }
}

void c64_via_write(c64_via *v, uint8_t reg, uint8_t value) {
    switch (reg & 0x0f) {
    case 0x0:
        v->orb = value;
        clear(v, VIA_CB1 | VIA_CB2);
        break;
    case 0x1:
        clear(v, VIA_CA1 | VIA_CA2);
        /* fall through */
    case 0xf:
        v->ora = value;
        break;
    case 0x2: v->ddrb = value; break;
    case 0x3: v->ddra = value; break;
    case 0x4:
    case 0x6:
        v->t1_latch = (v->t1_latch & 0xff00) | value;
        break;
    case 0x5:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00ff) | value << 8);
        v->t1 = v->t1_latch;
        v->t1_armed = true;
        clear(v, VIA_T1);
        break;
    case 0x7:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00ff) | value << 8);
        clear(v, VIA_T1);
        break;
    case 0x8: v->t2_latch_lo = value; break;
    case 0x9:
        v->t2 = (uint16_t)(v->t2_latch_lo | value << 8);
        v->t2_armed = true;
        clear(v, VIA_T2);
        break;
    case 0xa:
        v->sr = value;
        clear(v, VIA_SR);
        break;
    case 0xb: v->acr = value; break;
    case 0xc: v->pcr = value; break;
    case 0xd:
        clear(v, value & 0x7f);
        break;
    default:  // 0xe
        if (value & 0x80)
            v->ier |= value & 0x7f;
        else
            v->ier &= ~value;
        update_irq(v);
        break;
    }
}
