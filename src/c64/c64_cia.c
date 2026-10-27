/**
 * @file c64_cia.c
 * @brief Implementation of c64_cia.h: a 6526 CIA: two 8-bit ports, timers A and B (one-shot or continuous; B can count A's underflows), the time-of-day clock with its alarm, and the interrupt control register.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_cia.h"

#include <string.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#else
/// PC builds: the hot functions stay where they are.
#define FAST(f) f
#endif

/// The interrupt control register's bits.
enum { ICR_TA = 0x01, ICR_TB = 0x02, ICR_ALARM = 0x04, ICR_SP = 0x08 };
/// The timers' control register bits.
enum { CR_START = 0x01, CR_ONESHOT = 0x08, CR_LOAD = 0x10 };

/// Sets the IRQ line from the flags and the mask.
/// @param c the CIA
static void update_irq(c64_cia *c) {
    c->irq = (c->icr_data & c->icr_mask & 0x1f) != 0;
}

/// Raises interrupt flags.
/// @param c the CIA
/// @param bits the ICR bits
static void event(c64_cia *c, uint8_t bits) {
    c->icr_data |= bits;
    update_irq(c);
}

void c64_cia_reset(c64_cia *c) {
    uint8_t (*port_in)(void *, c64_cia *, int) = c->port_in;
    void *ctx = c->ctx;
    memset(c, 0, sizeof *c);
    c->port_in = port_in;
    c->ctx = ctx;
    c->ta = c->tb = c->ta_latch = c->tb_latch = 0xffff;
    c->tod[3] = 0x01;  // 1:00:00.0 AM, as the chip powers up
}

/// Counts a timer down n times; returns how many times it underflowed. It
/// reloads from the latch on each underflow, so it repeats every latch + 1
/// counts; one-shot, it stops after the first.
/// @param t the counter
/// @param latch its reload value
/// @param n counts
/// @param cr its control register (one-shot stops it)
/// @return the underflows
static unsigned FAST(count)(uint16_t *t, uint16_t latch, unsigned n, uint8_t *cr) {
    if (n <= *t) {
        *t -= n;
        return 0;
    }
    n -= *t + 1u;
    *t = latch;
    if (*cr & CR_ONESHOT) {
        *cr &= ~CR_START;
        return 1;
    }
    unsigned period = latch + 1u;
    *t = (uint16_t)(latch - n % period);
    return 1 + n / period;
}

void FAST(c64_cia_tick)(c64_cia *c, unsigned n) {
    unsigned under_a = 0;
    if ((c->cra & (CR_START | 0x20)) == CR_START) {  // counting clock cycles
        under_a = count(&c->ta, c->ta_latch, n, &c->cra);
        if (under_a)
            event(c, ICR_TA);
    }
    if (c->crb & CR_START) {
        unsigned src = (c->crb >> 5) & 3, under_b = 0;
        if (src == 0)
            under_b = count(&c->tb, c->tb_latch, n, &c->crb);
        else if (src >= 2 && under_a)  // counting timer A (with CNT, for 3: always high)
            under_b = count(&c->tb, c->tb_latch, under_a, &c->crb);
        if (under_b)
            event(c, ICR_TB);
    }
}

/// Adds one to a BCD byte.
/// @param v the byte
/// @return v + 1, in BCD
static uint8_t bcd_inc(uint8_t v) {
    return (v & 0x0f) == 9 ? (uint8_t)((v & 0xf0) + 0x10) : (uint8_t)(v + 1);
}

void c64_cia_tod_tick(c64_cia *c) {
    if (c->tod_stopped)
        return;
    uint8_t *t = c->tod;
    t[0] = t[0] >= 9 ? 0 : t[0] + 1;
    if (t[0] == 0) {
        t[1] = bcd_inc(t[1]);
        if (t[1] == 0x60) {
            t[1] = 0;
            t[2] = bcd_inc(t[2]);
            if (t[2] == 0x60) {
                t[2] = 0;
                uint8_t h = t[3] & 0x1f, pm = t[3] & 0x80;
                if (h == 0x11)
                    pm ^= 0x80;
                h = (h == 0x12) ? 0x01 : bcd_inc(h);
                t[3] = h | pm;
            }
        }
    }
    if (!memcmp(c->tod, c->alarm, 4))
        event(c, ICR_ALARM);
}

uint8_t c64_cia_read(c64_cia *c, uint8_t reg) {
    switch (reg & 0x0f) {
    case 0x0:
    case 0x1: {
        int port = reg & 1;
        return c->port_in ? c->port_in(c->ctx, c, port) : c64_cia_out(c, port);
    }
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return c->ta & 0xff;
    case 0x5: return c->ta >> 8;
    case 0x6: return c->tb & 0xff;
    case 0x7: return c->tb >> 8;
    case 0x8: {
        uint8_t v = c->tod_latched ? c->tod_latch[0] : c->tod[0];
        c->tod_latched = false;
        return v;
    }
    case 0x9:
    case 0xa:
        return c->tod_latched ? c->tod_latch[reg & 3] : c->tod[(reg & 0x0f) - 8];
    case 0xb:
        if (!c->tod_latched) {
            memcpy(c->tod_latch, c->tod, 4);
            c->tod_latched = true;
        }
        return c->tod_latch[3];
    case 0xc: return c->sdr;
    case 0xd: {
        uint8_t v = c->icr_data | (c->irq ? 0x80 : 0);
        c->icr_data = 0;
        c->irq = false;
        return v;
    }
    case 0xe: return c->cra & ~CR_LOAD;
    default:  return c->crb & ~CR_LOAD;
    }
}

void c64_cia_write(c64_cia *c, uint8_t reg, uint8_t v) {
    switch (reg & 0x0f) {
    case 0x0: c->pra = v; break;
    case 0x1: c->prb = v; break;
    case 0x2: c->ddra = v; break;
    case 0x3: c->ddrb = v; break;
    case 0x4: c->ta_latch = (c->ta_latch & 0xff00) | v; break;
    case 0x5:
        c->ta_latch = (c->ta_latch & 0x00ff) | (uint16_t)(v << 8);
        if (!(c->cra & CR_START))
            c->ta = c->ta_latch;
        break;
    case 0x6: c->tb_latch = (c->tb_latch & 0xff00) | v; break;
    case 0x7:
        c->tb_latch = (c->tb_latch & 0x00ff) | (uint16_t)(v << 8);
        if (!(c->crb & CR_START))
            c->tb = c->tb_latch;
        break;
    case 0x8:
    case 0x9:
    case 0xa:
    case 0xb: {
        unsigned i = (reg & 0x0f) - 8;
        static const uint8_t mask[4] = {0x0f, 0x7f, 0x7f, 0x9f};
        v &= mask[i];
        if (c->crb & 0x80) {
            c->alarm[i] = v;
        } else {
            // Writing 12 AM/PM flips the AM/PM bit, as the chip does.
            if (i == 3 && (v & 0x1f) == 0x12)
                v ^= 0x80;
            c->tod[i] = v;
            if (i == 3)
                c->tod_stopped = true;
            else if (i == 0)
                c->tod_stopped = false;
        }
        break;
    }
    case 0xc:
        c->sdr = v;
        if (c->cra & 0x40)  // output mode: say it was sent at once
            event(c, ICR_SP);
        break;
    case 0xd:
        if (v & 0x80)
            c->icr_mask |= v & 0x1f;
        else
            c->icr_mask &= ~v;
        update_irq(c);
        break;
    case 0xe:
        if (v & CR_LOAD)
            c->ta = c->ta_latch;
        c->cra = v & ~CR_LOAD;
        break;
    default:
        if (v & CR_LOAD)
            c->tb = c->tb_latch;
        c->crb = v & ~CR_LOAD;
        break;
    }
}
