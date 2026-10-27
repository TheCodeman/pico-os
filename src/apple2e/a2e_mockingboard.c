/**
 * @file a2e_mockingboard.c
 * @brief The Mockingboard's two 6522 VIAs and the AY-3-8912 control lines on their ports.
 *
 * Port A is the sound chip's data bus; port B bits 0-2 are its BC1, BDIR and
 * /RESET: latch a register number (BDIR and BC1 high), write it (BDIR), read
 * it (BC1). The timers count down a cycle at a time and interrupt through
 * IFR/IER; they are kept as start points and the cycle each next passes
 * zero, with an event booked for the earliest.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "a2e_mockingboard.h"

#include <string.h>

/// The 6522's registers.
enum {
    ORB, ORA, DDRB, DDRA, T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, SR, ACR, PCR, IFR, IER, ORA_NH,
};
/// The interrupt flag of timer 1.
#define IF_T1 0x40
/// The interrupt flag of timer 2.
#define IF_T2 0x20

/// Timer 1's value at a cycle.
/// @param v the VIA
/// @param now the cycle
/// @return the value
static uint16_t t1_now(const a2e_via *v, uint64_t now) {
    return (uint16_t)(v->t1_value - (uint16_t)(now - v->t1_base));
}

/// Timer 2's value at a cycle.
/// @param v the VIA
/// @param now the cycle
/// @return the value
static uint16_t t2_now(const a2e_via *v, uint64_t now) {
    return (uint16_t)(v->t2_value - (uint16_t)(now - v->t2_base));
}

/// Sets the IRQ line and asks for the next timer event.
/// @param mb the card
static void update(a2e_mockingboard *mb) {
    bool irq = false;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < 2; ++i) {
        const a2e_via *v = &mb->via[i];
        irq |= (v->ifr & v->ier & 0x7f) != 0;
        if (v->t1_at < next)
            next = v->t1_at;
        if (v->t2_at < next)
            next = v->t2_at;
    }
    a2e_set_irq(mb->m, mb->slot, irq);
    a2e_set_event(mb->m, mb->slot, next);
}

/// Passes a write to an AY chip on to the host.
/// @param mb the card
/// @param chip 0 or 1
/// @param reg the register
/// @param value the value
static void ay_out(a2e_mockingboard *mb, int chip, uint8_t reg, uint8_t value) {
    if (mb->ay_write)
        mb->ay_write(mb->user, chip, reg, value, mb->m->cycles);
}

/// Port B's lines to the sound chip after a write to it or port A.
/// @param mb the card
/// @param chip 0 or 1
static void ay_control(a2e_mockingboard *mb, int chip) {
    a2e_via *v = &mb->via[chip];
    uint8_t ctl = v->orb & v->ddrb, data = v->ora & v->ddra;
    if (!(ctl & 4)) {
        // /RESET: every register to 0, once per press.
        bool any = false;
        for (int r = 0; r < 16; ++r)
            any |= v->ay_regs[r] != 0;
        if (any) {
            memset(v->ay_regs, 0, sizeof v->ay_regs);
            for (uint8_t r = 0; r < 16; ++r)
                ay_out(mb, chip, r, 0);
        }
        return;
    }
    switch (ctl & 3) {
    case 3: v->ay_reg = data & 0x0f; break;              // latch address
    case 2:                                              // write
        v->ay_regs[v->ay_reg] = data;
        ay_out(mb, chip, v->ay_reg, data);
        break;
    case 1: v->ay_in = v->ay_regs[v->ay_reg]; break;     // read
    }
}

/// A VIA register read or written, with the timers worked out from the cycle count.
/// @param mb the card
/// @param chip 0 or 1
/// @param reg the register
/// @param write a write
/// @param val the byte written
/// @return the byte read
static uint8_t via_access(a2e_mockingboard *mb, int chip, uint8_t reg, bool write, uint8_t val) {
    a2e_via *v = &mb->via[chip];
    uint64_t now = mb->m->cycles;
    uint8_t r = 0;
    if (write) {
        switch (reg) {
        case ORB: v->orb = val; ay_control(mb, chip); break;
        case ORA: case ORA_NH: v->ora = val; ay_control(mb, chip); break;
        case DDRB: v->ddrb = val; break;
        case DDRA: v->ddra = val; break;
        case T1CL: case T1LL: v->t1_latch = (v->t1_latch & 0xff00) | val; break;
        case T1CH:
            // Load and start: the flag goes up as the count passes zero.
            v->t1_latch = (uint16_t)((v->t1_latch & 0xff) | val << 8);
            v->t1_value = v->t1_latch;
            v->t1_base = now;
            v->t1_at = now + v->t1_latch + 1;
            v->ifr &= ~IF_T1;
            break;
        case T1LH:
            v->t1_latch = (uint16_t)((v->t1_latch & 0xff) | val << 8);
            v->ifr &= ~IF_T1;
            break;
        case T2CL: v->t2_latch_lo = val; break;
        case T2CH:
            v->t2_value = (uint16_t)(v->t2_latch_lo | val << 8);
            v->t2_base = now;
            v->t2_at = now + v->t2_value + 1;
            v->ifr &= ~IF_T2;
            break;
        case SR: v->sr = val; break;
        case ACR: v->acr = val; break;
        case PCR: v->pcr = val; break;
        case IFR: v->ifr &= (uint8_t)~(val & 0x7f); break;
        case IER:
            if (val & 0x80)
                v->ier |= val & 0x7f;
            else
                v->ier &= (uint8_t)~val;
            break;
        }
    } else {
        switch (reg) {
        case ORB: r = (v->orb & v->ddrb) | (uint8_t)~v->ddrb; break;
        case ORA: case ORA_NH: r = (v->ora & v->ddra) | (v->ay_in & (uint8_t)~v->ddra); break;
        case DDRB: r = v->ddrb; break;
        case DDRA: r = v->ddra; break;
        case T1CL: r = (uint8_t)t1_now(v, now); v->ifr &= ~IF_T1; break;
        case T1CH: r = (uint8_t)(t1_now(v, now) >> 8); break;
        case T1LL: r = (uint8_t)v->t1_latch; break;
        case T1LH: r = (uint8_t)(v->t1_latch >> 8); break;
        case T2CL: r = (uint8_t)t2_now(v, now); v->ifr &= ~IF_T2; break;
        case T2CH: r = (uint8_t)(t2_now(v, now) >> 8); break;
        case SR: r = v->sr; break;
        case ACR: r = v->acr; break;
        case PCR: r = v->pcr; break;
        case IFR: r = v->ifr | ((v->ifr & v->ier & 0x7f) ? 0x80 : 0); break;
        case IER: r = v->ier | 0x80; break;
        }
    }
    update(mb);
    return r;
}

/// The card's $Cn00-$CnFF: the two VIAs.
/// @param ctx the card
/// @param addr the address's low byte
/// @param write a write
/// @param val the byte written
/// @return the byte read
static uint8_t mb_mem(void *ctx, uint8_t addr, bool write, uint8_t val) {
    return via_access(ctx, addr >> 7, addr & 0x0f, write, val);
}

/// A timer passing zero: its flag goes up; Timer 1 in free-run mode (ACR bit
/// 6) reloads from its latch, a cycle and a count later.
/// @param ctx the card
/// @param at the cycle it happened
static void mb_event(void *ctx, uint64_t at) {
    a2e_mockingboard *mb = ctx;
    uint64_t now = mb->m->cycles;
    for (int i = 0; i < 2; ++i) {
        a2e_via *v = &mb->via[i];
        while (v->t1_at <= now) {
            uint64_t z = v->t1_at;
            v->ifr |= IF_T1;
            if (v->acr & 0x40) {
                v->t1_value = v->t1_latch;
                v->t1_base = z + 1;
                v->t1_at = z + (uint64_t)v->t1_latch + 2;
            } else {
                v->t1_value = 0xffff;
                v->t1_base = z;
                v->t1_at = UINT64_MAX;
            }
        }
        if (v->t2_at <= now) {
            v->ifr |= IF_T2;
            v->t2_value = 0xffff;
            v->t2_base = v->t2_at;
            v->t2_at = UINT64_MAX;
        }
    }
    (void)at;
    update(mb);
}

/// The reset line: the VIAs' registers clear (their timers stop
/// interrupting) and the sound chips go quiet.
/// @param ctx the card
static void mb_reset(void *ctx) {
    a2e_mockingboard *mb = ctx;
    for (int i = 0; i < 2; ++i) {
        a2e_via *v = &mb->via[i];
        uint8_t regs[16];
        memcpy(regs, v->ay_regs, sizeof regs);
        memset(v, 0, sizeof *v);
        memcpy(v->ay_regs, regs, sizeof regs);
        v->t1_at = v->t2_at = UINT64_MAX;
        ay_control(mb, i);  // port B reads as 0: /RESET
    }
    update(mb);
}

void a2e_mockingboard_install(a2e *m, a2e_mockingboard *mb, int slot) {
    memset(mb, 0, sizeof *mb);
    mb->m = m;
    mb->slot = slot;
    for (int i = 0; i < 2; ++i)
        mb->via[i].t1_at = mb->via[i].t2_at = UINT64_MAX;
    m->slot_mem[slot] = mb_mem;
    m->slot_event[slot] = mb_event;
    m->slot_reset[slot] = mb_reset;
    m->slot_ctx[slot] = mb;
    update(mb);
}
