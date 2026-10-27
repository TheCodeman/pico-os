/**
 * @file c64_cpu.c
 * @brief Implementation of c64_cpu.h: the C64's 6510.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_cpu.h"

#include <stddef.h>

// On the board the CPU runs from SRAM, away from the XIP cache that flash
// code and the PSRAM share.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#define TABLE   __attribute__((section("app_ov_c64.tables")))  ///< in SRAM with the code, away from the XIP cache
#else
#define FAST(f) f
#define TABLE   const
#endif

/// Base cycles per opcode (NMOS 6502; the JAMs as 2). Page crossings on
/// reads and taken branches add theirs as the instructions run.
static TABLE uint8_t base_cycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,  // 0x
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 1x
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,  // 2x
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 3x
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,  // 4x
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 5x
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,  // 6x
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 7x
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // 8x
    2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,  // 9x
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // Ax
    2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,  // Bx
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // Cx
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // Dx
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // Ex
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // Fx
};

enum { IMP, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IZX, IZY, REL, IND };

enum {
    ADC, ALR, ANC, AND, ANE, ARR, ASL, BIT, BR, BRK, CLC, CLD, CLI, CLV, CMP, CPX,
    CPY, DCP, DEC, DEX, DEY, EOR, INC, INX, INY, ISC, JAM, JMP, JSR, LAS, LAX, LDA,
    LDX, LDY, LSR, LXA, NOP, ORA, PHA, PHP, PLA, PLP, RLA, ROL, ROR, RRA, RTI, RTS,
    SAX, SBC, SBX, SEC, SED, SEI, SHA, SHX, SHY, SLO, SRE, STA, STX, STY, TAS, TAX,
    TAY, TSX, TXA, TXS, TYA,
};

static TABLE uint8_t inst[256] = {
    BRK, ORA, JAM, SLO, NOP, ORA, ASL, SLO, PHP, ORA, ASL, ANC, NOP, ORA, ASL, SLO,  // 0x
    BR,  ORA, JAM, SLO, NOP, ORA, ASL, SLO, CLC, ORA, NOP, SLO, NOP, ORA, ASL, SLO,  // 1x
    JSR, AND, JAM, RLA, BIT, AND, ROL, RLA, PLP, AND, ROL, ANC, BIT, AND, ROL, RLA,  // 2x
    BR,  AND, JAM, RLA, NOP, AND, ROL, RLA, SEC, AND, NOP, RLA, NOP, AND, ROL, RLA,  // 3x
    RTI, EOR, JAM, SRE, NOP, EOR, LSR, SRE, PHA, EOR, LSR, ALR, JMP, EOR, LSR, SRE,  // 4x
    BR,  EOR, JAM, SRE, NOP, EOR, LSR, SRE, CLI, EOR, NOP, SRE, NOP, EOR, LSR, SRE,  // 5x
    RTS, ADC, JAM, RRA, NOP, ADC, ROR, RRA, PLA, ADC, ROR, ARR, JMP, ADC, ROR, RRA,  // 6x
    BR,  ADC, JAM, RRA, NOP, ADC, ROR, RRA, SEI, ADC, NOP, RRA, NOP, ADC, ROR, RRA,  // 7x
    NOP, STA, NOP, SAX, STY, STA, STX, SAX, DEY, NOP, TXA, ANE, STY, STA, STX, SAX,  // 8x
    BR,  STA, JAM, SHA, STY, STA, STX, SAX, TYA, STA, TXS, TAS, SHY, STA, SHX, SHA,  // 9x
    LDY, LDA, LDX, LAX, LDY, LDA, LDX, LAX, TAY, LDA, TAX, LXA, LDY, LDA, LDX, LAX,  // Ax
    BR,  LDA, JAM, LAX, LDY, LDA, LDX, LAX, CLV, LDA, TSX, LAS, LDY, LDA, LDX, LAX,  // Bx
    CPY, CMP, NOP, DCP, CPY, CMP, DEC, DCP, INY, CMP, DEX, SBX, CPY, CMP, DEC, DCP,  // Cx
    BR,  CMP, JAM, DCP, NOP, CMP, DEC, DCP, CLD, CMP, NOP, DCP, NOP, CMP, DEC, DCP,  // Dx
    CPX, SBC, NOP, ISC, CPX, SBC, INC, ISC, INX, SBC, NOP, SBC, CPX, SBC, INC, ISC,  // Ex
    BR,  SBC, JAM, ISC, NOP, SBC, INC, ISC, SED, SBC, NOP, ISC, NOP, SBC, INC, ISC,  // Fx
};

static TABLE uint8_t modes[256] = {
    IMP, IZX, IMP, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // 0x
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 1x
    ABS, IZX, IMP, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // 2x
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 3x
    IMP, IZX, IMP, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // 4x
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 5x
    IMP, IZX, IMP, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, IND, ABS, ABS, ABS,  // 6x
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 7x
    IMM, IZX, IMM, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // 8x
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPY, ZPY, IMP, ABY, IMP, ABY, ABX, ABX, ABY, ABY,  // 9x
    IMM, IZX, IMM, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // Ax
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPY, ZPY, IMP, ABY, IMP, ABY, ABX, ABX, ABY, ABY,  // Bx
    IMM, IZX, IMM, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // Cx
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // Dx
    IMM, IZX, IMM, IZX, ZP, ZP, ZP, ZP, IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // Ex
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // Fx
};

static inline uint8_t rd(c64_cpu *c, uint16_t addr) {
    const uint8_t *p = c->rpage ? c->rpage[addr >> 8] : NULL;
    return p ? p[addr & 0xff] : c->read(c->ctx, addr);
}

static inline void wr(c64_cpu *c, uint16_t addr, uint8_t v) {
    uint8_t *p = c->wpage && addr > 1 ? c->wpage[addr >> 8] : NULL;
    if (p)
        p[addr & 0xff] = v;
    else
        c->write(c->ctx, addr, v);
}

#define RD(addr)     rd(c, (uint16_t)(addr))
#define WR(addr, v)  wr(c, (uint16_t)(addr), (uint8_t)(v))

static inline uint8_t fetch(c64_cpu *c) {
    return RD(c->pc++);
}

static inline uint16_t fetch16(c64_cpu *c) {
    uint16_t lo = fetch(c);
    return lo | (uint16_t)fetch(c) << 8;
}

static inline void push(c64_cpu *c, uint8_t v) {
    WR(0x100 | c->s, v);
    c->s--;
}

static inline uint8_t pull(c64_cpu *c) {
    c->s++;
    return RD(0x100 | c->s);
}

static inline uint8_t nz(c64_cpu *c, uint8_t v) {
    c->p = (c->p & ~(C64_N | C64_Z)) | (v & C64_N) | (v ? 0 : C64_Z);
    return v;
}

static inline void set_flag(c64_cpu *c, uint8_t f, bool on) {
    c->p = on ? (c->p | f) : (c->p & ~f);
}

void c64_cpu_reset(c64_cpu *c) {
    c->s = 0xfd;
    c->p = C64_U | C64_I;
    c->jammed = false;
    c->nmi = false;
    c->pc = RD(0xfffc) | (uint16_t)RD(0xfffd) << 8;
}

static void FAST(adc)(c64_cpu *c, uint8_t m) {
    unsigned carry = c->p & C64_C;
    if (c->p & C64_D) {
        // NMOS: Z from the binary sum, N and V from the half-adjusted one.
        unsigned t = (c->a & 0x0f) + (m & 0x0f) + carry;
        if (t > 9)
            t += 6;
        t = (t <= 0x0f) ? (t & 0x0f) + (c->a & 0xf0) + (m & 0xf0)
                        : (t & 0x0f) + (c->a & 0xf0) + (m & 0xf0) + 0x10;
        set_flag(c, C64_Z, !((c->a + m + carry) & 0xff));
        set_flag(c, C64_N, t & 0x80);
        set_flag(c, C64_V, ((c->a ^ t) & 0x80) && !((c->a ^ m) & 0x80));
        if ((t & 0x1f0) > 0x90)
            t += 0x60;
        set_flag(c, C64_C, (t & 0xff0) > 0xf0);
        c->a = (uint8_t)t;
    } else {
        unsigned t = c->a + m + carry;
        set_flag(c, C64_V, ~(c->a ^ m) & (c->a ^ t) & 0x80);
        set_flag(c, C64_C, t > 0xff);
        c->a = nz(c, (uint8_t)t);
    }
}

static void FAST(sbc)(c64_cpu *c, uint8_t m) {
    unsigned borrow = (c->p & C64_C) ? 0 : 1;
    unsigned t = c->a - m - borrow;
    // The flags are the binary subtraction's in both modes.
    set_flag(c, C64_V, (c->a ^ m) & (c->a ^ t) & 0x80);
    set_flag(c, C64_C, t < 0x100);
    nz(c, (uint8_t)t);
    if (c->p & C64_D) {
        unsigned lo = (c->a & 0x0f) - (m & 0x0f) - borrow;
        unsigned r = (lo & 0x10) ? ((lo - 6) & 0x0f) | ((c->a & 0xf0) - (m & 0xf0) - 0x10)
                                 : (lo & 0x0f) | ((c->a & 0xf0) - (m & 0xf0));
        if (r & 0x100)
            r -= 0x60;
        c->a = (uint8_t)r;
    } else {
        c->a = (uint8_t)t;
    }
}

static inline void cmp(c64_cpu *c, uint8_t r, uint8_t m) {
    set_flag(c, C64_C, r >= m);
    nz(c, (uint8_t)(r - m));
}

static inline uint8_t asl(c64_cpu *c, uint8_t v) {
    set_flag(c, C64_C, v & 0x80);
    return nz(c, (uint8_t)(v << 1));
}

static inline uint8_t lsr(c64_cpu *c, uint8_t v) {
    set_flag(c, C64_C, v & 1);
    return nz(c, v >> 1);
}

static inline uint8_t rol(c64_cpu *c, uint8_t v) {
    uint8_t r = (uint8_t)(v << 1) | (c->p & C64_C);
    set_flag(c, C64_C, v & 0x80);
    return nz(c, r);
}

static inline uint8_t ror(c64_cpu *c, uint8_t v) {
    uint8_t r = (v >> 1) | (uint8_t)((c->p & C64_C) << 7);
    set_flag(c, C64_C, v & 1);
    return nz(c, r);
}

static void FAST(interrupt)(c64_cpu *c, uint16_t vector, bool brk) {
    push(c, c->pc >> 8);
    push(c, c->pc & 0xff);
    push(c, (c->p | C64_U | (brk ? C64_B : 0)) & (brk ? 0xff : ~C64_B));
    c->p |= C64_I;
    c->pc = RD(vector) | (uint16_t)RD(vector + 1) << 8;
}

/// Magic constant ORed into A by the unstable ANE and LXA.
#define MAGIC 0xee

int FAST(c64_cpu_step)(c64_cpu *c) {
    if (c->jammed)
        return 2;
    if (c->nmi) {
        c->nmi = false;
        interrupt(c, 0xfffa, false);
        return 7;
    }
    if (c->irq && !(c->p & C64_I)) {
        interrupt(c, 0xfffe, false);
        return 7;
    }

    uint8_t op = fetch(c);
    int cyc = base_cycles[op];
    uint8_t mode = modes[op];
    uint16_t ea = 0, base = 0;
    bool crossed = false;

    switch (mode) {
    case IMP:
        break;
    case IMM:
    case REL:
        ea = c->pc++;
        break;
    case ZP:
        ea = fetch(c);
        break;
    case ZPX:
        ea = (uint8_t)(fetch(c) + c->x);
        break;
    case ZPY:
        ea = (uint8_t)(fetch(c) + c->y);
        break;
    case ABS:
        ea = fetch16(c);
        break;
    case ABX:
        base = fetch16(c);
        ea = base + c->x;
        crossed = (ea ^ base) & 0xff00;
        break;
    case ABY:
        base = fetch16(c);
        ea = base + c->y;
        crossed = (ea ^ base) & 0xff00;
        break;
    case IZX: {
        uint8_t z = fetch(c) + c->x;
        ea = RD(z) | (uint16_t)RD((uint8_t)(z + 1)) << 8;
        break;
    }
    case IZY: {
        uint8_t z = fetch(c);
        base = RD(z) | (uint16_t)RD((uint8_t)(z + 1)) << 8;
        ea = base + c->y;
        crossed = (ea ^ base) & 0xff00;
        break;
    }
    case IND: {
        uint16_t ptr = fetch16(c);
        // The NMOS bug: the high byte comes from the same page.
        ea = RD(ptr) | (uint16_t)RD((ptr & 0xff00) | ((ptr + 1) & 0xff)) << 8;
        break;
    }
    }
    // Indexed reads take a cycle more across a page; their base count is
    // 4 (abs,x/abs,y) or 5 ((zp),y), where stores and read-modify-writes
    // have more.
    if (crossed && (cyc == 4 || (mode == IZY && cyc == 5)))
        ++cyc;

    uint8_t m, r;
    switch (inst[op]) {
    // Loads, stores and transfers.
    case LDA: c->a = nz(c, RD(ea)); break;
    case LDX: c->x = nz(c, RD(ea)); break;
    case LDY: c->y = nz(c, RD(ea)); break;
    case LAX: c->a = c->x = nz(c, RD(ea)); break;
    case STA: WR(ea, c->a); break;
    case STX: WR(ea, c->x); break;
    case STY: WR(ea, c->y); break;
    case SAX: WR(ea, c->a & c->x); break;
    case TAX: c->x = nz(c, c->a); break;
    case TAY: c->y = nz(c, c->a); break;
    case TXA: c->a = nz(c, c->x); break;
    case TYA: c->a = nz(c, c->y); break;
    case TSX: c->x = nz(c, c->s); break;
    case TXS: c->s = c->x; break;
    case PHA: push(c, c->a); break;
    case PHP: push(c, c->p | C64_B | C64_U); break;
    case PLA: c->a = nz(c, pull(c)); break;
    case PLP: c->p = (pull(c) & ~C64_B) | C64_U; break;

    // Arithmetic and logic.
    case ORA: c->a = nz(c, c->a | RD(ea)); break;
    case AND: c->a = nz(c, c->a & RD(ea)); break;
    case EOR: c->a = nz(c, c->a ^ RD(ea)); break;
    case ADC: adc(c, RD(ea)); break;
    case SBC: sbc(c, RD(ea)); break;
    case CMP: cmp(c, c->a, RD(ea)); break;
    case CPX: cmp(c, c->x, RD(ea)); break;
    case CPY: cmp(c, c->y, RD(ea)); break;
    case BIT:
        m = RD(ea);
        c->p = (c->p & ~(C64_N | C64_V | C64_Z)) | (m & (C64_N | C64_V)) | ((c->a & m) ? 0 : C64_Z);
        break;
    case INX: c->x = nz(c, c->x + 1); break;
    case INY: c->y = nz(c, c->y + 1); break;
    case DEX: c->x = nz(c, c->x - 1); break;
    case DEY: c->y = nz(c, c->y - 1); break;

    // Read-modify-write: the NMOS chip writes the old value back first
    // (which some programs rely on to acknowledge I/O, e.g. INC $D019).
    case ASL: case LSR: case ROL: case ROR: case INC: case DEC:
    case SLO: case SRE: case RLA: case RRA: case DCP: case ISC:
        if (mode == IMP) {
            switch (inst[op]) {
            case ASL: c->a = asl(c, c->a); break;
            case LSR: c->a = lsr(c, c->a); break;
            case ROL: c->a = rol(c, c->a); break;
            default:  c->a = ror(c, c->a); break;
            }
            break;
        }
        m = RD(ea);
        WR(ea, m);
        switch (inst[op]) {
        case ASL: r = asl(c, m); break;
        case LSR: r = lsr(c, m); break;
        case ROL: r = rol(c, m); break;
        case ROR: r = ror(c, m); break;
        case INC: r = nz(c, m + 1); break;
        case DEC: r = nz(c, m - 1); break;
        case SLO: r = asl(c, m); c->a = nz(c, c->a | r); break;
        case SRE: r = lsr(c, m); c->a = nz(c, c->a ^ r); break;
        case RLA: r = rol(c, m); c->a = nz(c, c->a & r); break;
        case RRA: r = ror(c, m); adc(c, r); break;
        case DCP: r = m - 1; cmp(c, c->a, r); break;
        default:  r = m + 1; sbc(c, r); break;  // ISC
        }
        WR(ea, r);
        break;

    // Undocumented immediate operations.
    case ANC:
        c->a = nz(c, c->a & RD(ea));
        set_flag(c, C64_C, c->a & 0x80);
        break;
    case ALR:
        c->a = lsr(c, c->a & RD(ea));
        break;
    case ARR: {
        uint8_t t = c->a & RD(ea);
        uint8_t v = (t >> 1) | (uint8_t)((c->p & C64_C) << 7);
        if (c->p & C64_D) {
            set_flag(c, C64_N, c->p & C64_C);
            set_flag(c, C64_Z, !v);
            set_flag(c, C64_V, (v ^ t) & 0x40);
            if ((t & 0x0f) + (t & 0x01) > 0x05)
                v = (v & 0xf0) | ((v + 0x06) & 0x0f);
            bool carry = (t & 0xf0) + (t & 0x10) > 0x50;
            if (carry)
                v = (v & 0x0f) | ((v + 0x60) & 0xf0);
            set_flag(c, C64_C, carry);
            c->a = v;
        } else {
            c->a = nz(c, v);
            set_flag(c, C64_C, v & 0x40);
            set_flag(c, C64_V, ((v >> 6) ^ (v >> 5)) & 1);
        }
        break;
    }
    case SBX: {
        uint8_t t = c->a & c->x;
        m = RD(ea);
        set_flag(c, C64_C, t >= m);
        c->x = nz(c, t - m);
        break;
    }
    case ANE: c->a = nz(c, (c->a | MAGIC) & c->x & RD(ea)); break;
    case LXA: c->a = c->x = nz(c, (c->a | MAGIC) & RD(ea)); break;
    case LAS: c->a = c->x = c->s = nz(c, RD(ea) & c->s); break;

    // The unstable stores: the value is ANDed with the base address's high
    // byte plus one, and across a page that value replaces the high byte.
    case SHA: case SHX: case SHY: case TAS: {
        uint8_t h = (uint8_t)((base >> 8) + 1);
        uint8_t v;
        switch (inst[op]) {
        case SHA: v = c->a & c->x & h; break;
        case SHX: v = c->x & h; break;
        case SHY: v = c->y & h; break;
        default:  c->s = c->a & c->x; v = c->s & h; break;  // TAS
        }
        if (crossed)
            ea = (uint16_t)(v << 8) | (ea & 0xff);
        WR(ea, v);
        break;
    }

    // Flow.
    case BR: {
        static TABLE uint8_t flag[4] = {C64_N, C64_V, C64_C, C64_Z};
        int8_t d = (int8_t)RD(ea);
        if (!(c->p & flag[op >> 6]) == !(op & 0x20)) {
            uint16_t to = c->pc + d;
            cyc += ((to ^ c->pc) & 0xff00) ? 2 : 1;
            c->pc = to;
        }
        break;
    }
    case JMP: c->pc = ea; break;
    case JSR:
        --c->pc;
        push(c, c->pc >> 8);
        push(c, c->pc & 0xff);
        c->pc = ea;
        break;
    case RTS:
        c->pc = pull(c);
        c->pc = (c->pc | (uint16_t)pull(c) << 8) + 1;
        break;
    case RTI:
        c->p = (pull(c) & ~C64_B) | C64_U;
        c->pc = pull(c);
        c->pc |= (uint16_t)pull(c) << 8;
        break;
    case BRK:
        ++c->pc;  // BRK skips a padding byte
        interrupt(c, 0xfffe, true);
        break;
    case JAM:
        c->jammed = true;
        --c->pc;
        break;

    // Flags.
    case CLC: c->p &= ~C64_C; break;
    case SEC: c->p |= C64_C; break;
    case CLI: c->p &= ~C64_I; break;
    case SEI: c->p |= C64_I; break;
    case CLD: c->p &= ~C64_D; break;
    case SED: c->p |= C64_D; break;
    case CLV: c->p &= ~C64_V; break;

    case NOP:
        if (mode != IMP)
            (void)RD(ea);  // the undocumented NOPs read their operand
        break;
    }
    return cyc;
}
