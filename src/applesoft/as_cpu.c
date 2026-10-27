/**
 * @file as_cpu.c
 * @brief Implementation of as_cpu.h: a 65C02 as fitted to the enhanced Apple IIe (no Rockwell bit instructions; their opcodes are NOPs), with cycle counts.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "as_cpu.h"

// On the board the interpreter loop runs from SRAM, away from the XIP cache
// that flash code and the PSRAM share.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_apple2." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#else
#define FAST(f) f
#endif

/// Base cycles per opcode (65C02). Page crossings, taken branches and decimal
/// mode add theirs as the instructions run.
static const uint8_t base_cycles[256] = {
    7, 6, 2, 1, 5, 3, 5, 1, 3, 2, 2, 1, 6, 4, 6, 1,  // 0x
    2, 5, 5, 1, 5, 4, 6, 1, 2, 4, 2, 1, 6, 4, 6, 1,  // 1x
    6, 6, 2, 1, 3, 3, 5, 1, 4, 2, 2, 1, 4, 4, 6, 1,  // 2x
    2, 5, 5, 1, 4, 4, 6, 1, 2, 4, 2, 1, 4, 4, 6, 1,  // 3x
    6, 6, 2, 1, 3, 3, 5, 1, 3, 2, 2, 1, 3, 4, 6, 1,  // 4x
    2, 5, 5, 1, 4, 4, 6, 1, 2, 4, 3, 1, 8, 4, 6, 1,  // 5x
    6, 6, 2, 1, 3, 3, 5, 1, 4, 2, 2, 1, 6, 4, 6, 1,  // 6x
    2, 5, 5, 1, 4, 4, 6, 1, 2, 4, 4, 1, 6, 4, 6, 1,  // 7x
    2, 6, 2, 1, 3, 3, 3, 1, 2, 2, 2, 1, 4, 4, 4, 1,  // 8x
    2, 6, 5, 1, 4, 4, 4, 1, 2, 5, 2, 1, 4, 5, 5, 1,  // 9x
    2, 6, 2, 1, 3, 3, 3, 1, 2, 2, 2, 1, 4, 4, 4, 1,  // Ax
    2, 5, 5, 1, 4, 4, 4, 1, 2, 4, 2, 1, 4, 4, 4, 1,  // Bx
    2, 6, 2, 1, 3, 3, 5, 1, 2, 2, 2, 1, 4, 4, 6, 1,  // Cx
    2, 5, 5, 1, 4, 4, 6, 1, 2, 4, 3, 1, 4, 4, 7, 1,  // Dx
    2, 6, 2, 1, 3, 3, 5, 1, 2, 2, 2, 1, 4, 4, 6, 1,  // Ex
    2, 5, 5, 1, 4, 4, 6, 1, 2, 4, 4, 1, 4, 4, 7, 1,  // Fx
};

#define RD(addr)     c->read(c->ctx, (uint16_t)(addr))
#define WR(addr, v)  c->write(c->ctx, (uint16_t)(addr), (uint8_t)(v))

static inline uint8_t fetch(as_cpu *c) {
    return RD(c->pc++);
}

static inline uint16_t fetch16(as_cpu *c) {
    uint16_t lo = fetch(c);
    return lo | (uint16_t)fetch(c) << 8;
}

static inline uint16_t read16_zp(as_cpu *c, uint8_t z) {
    return RD(z) | (uint16_t)RD((uint8_t)(z + 1)) << 8;
}

void as_cpu_push(as_cpu *c, uint8_t v) {
    WR(0x100 | c->s, v);
    c->s--;
}

static inline uint8_t pull(as_cpu *c) {
    c->s++;
    return RD(0x100 | c->s);
}

static inline void set_nz(as_cpu *c, uint8_t v) {
    c->p = (c->p & ~(AS_N | AS_Z)) | (v & AS_N) | (v ? 0 : AS_Z);
}

void as_cpu_reset(as_cpu *c) {
    c->s = 0xfd;
    c->p = AS_U | AS_I;
    c->pc = RD(0xfffc) | (uint16_t)RD(0xfffd) << 8;
}

static void FAST(adc)(as_cpu *c, uint8_t m, int *cyc) {
    unsigned carry = c->p & AS_C;
    if (c->p & AS_D) {
        unsigned lo = (c->a & 0x0f) + (m & 0x0f) + carry;
        if (lo > 9)
            lo += 6;
        unsigned hi = (c->a >> 4) + (m >> 4) + (lo > 0x0f);
        unsigned bin = (hi << 4) & 0xff;
        c->p &= ~(AS_C | AS_V);
        if (~(c->a ^ m) & (c->a ^ bin) & 0x80)
            c->p |= AS_V;
        if (hi > 9)
            hi += 6;
        if (hi > 15)
            c->p |= AS_C;
        c->a = (uint8_t)((hi << 4) | (lo & 0x0f));
        set_nz(c, c->a);
        ++*cyc;
    } else {
        unsigned t = c->a + m + carry;
        c->p &= ~(AS_C | AS_V);
        if (~(c->a ^ m) & (c->a ^ t) & 0x80)
            c->p |= AS_V;
        if (t > 0xff)
            c->p |= AS_C;
        c->a = (uint8_t)t;
        set_nz(c, c->a);
    }
}

static void FAST(sbc)(as_cpu *c, uint8_t m, int *cyc) {
    if (c->p & AS_D) {
        unsigned borrow = (c->p & AS_C) ? 0 : 1;
        int t = c->a - m - borrow;
        int lo = (c->a & 0x0f) - (m & 0x0f) - (int)borrow;
        int hi = (c->a >> 4) - (m >> 4);
        if (lo < 0) {
            lo -= 6;
            hi--;
        }
        if (hi < 0)
            hi -= 6;
        c->p &= ~(AS_C | AS_V);
        if ((c->a ^ m) & (c->a ^ t) & 0x80)
            c->p |= AS_V;
        if (t >= 0)
            c->p |= AS_C;
        c->a = (uint8_t)((hi << 4) | (lo & 0x0f));
        set_nz(c, c->a);
        ++*cyc;
    } else {
        adc(c, (uint8_t)~m, cyc);
    }
}

static inline void compare(as_cpu *c, uint8_t r, uint8_t m) {
    c->p = (c->p & ~AS_C) | (r >= m ? AS_C : 0);
    set_nz(c, (uint8_t)(r - m));
}

static inline uint8_t asl(as_cpu *c, uint8_t v) {
    c->p = (c->p & ~AS_C) | (v >> 7);
    v <<= 1;
    set_nz(c, v);
    return v;
}

static inline uint8_t lsr(as_cpu *c, uint8_t v) {
    c->p = (c->p & ~AS_C) | (v & 1);
    v >>= 1;
    set_nz(c, v);
    return v;
}

static inline uint8_t rol(as_cpu *c, uint8_t v) {
    uint8_t r = (uint8_t)(v << 1) | (c->p & AS_C);
    c->p = (c->p & ~AS_C) | (v >> 7);
    set_nz(c, r);
    return r;
}

static inline uint8_t ror(as_cpu *c, uint8_t v) {
    uint8_t r = (uint8_t)(v >> 1) | (uint8_t)((c->p & AS_C) << 7);
    c->p = (c->p & ~AS_C) | (v & 1);
    set_nz(c, r);
    return r;
}

static inline void bit(as_cpu *c, uint8_t m) {
    c->p = (c->p & ~(AS_N | AS_V | AS_Z)) | (m & (AS_N | AS_V)) | ((c->a & m) ? 0 : AS_Z);
}

int FAST(as_cpu_step)(as_cpu *c) {
    if (c->irq && !(c->p & AS_I)) {
        // As BRK, but with B clear and the current instruction not skipped.
        as_cpu_push(c, (uint8_t)(c->pc >> 8));
        as_cpu_push(c, (uint8_t)c->pc);
        as_cpu_push(c, (uint8_t)((c->p & ~AS_B) | AS_U));
        c->p = (c->p | AS_I) & ~AS_D;
        c->pc = RD(0xfffe) | (uint16_t)RD(0xffff) << 8;
        c->cycles += 7;
        return 7;
    }
    uint8_t op = fetch(c);
    int cyc = base_cycles[op];
    uint16_t ea = 0;
    uint8_t m;

    // Effective address for the common addressing modes, by opcode's low
    // bits. Instructions that don't fit the pattern are handled first.
#define ZP()    (ea = fetch(c))
#define ZPX()   (ea = (uint8_t)(fetch(c) + c->x))
#define ZPY()   (ea = (uint8_t)(fetch(c) + c->y))
#define ABS()   (ea = fetch16(c))
#define ABSX(pen)                                   \
    do {                                            \
        uint16_t b_ = fetch16(c);                   \
        ea = b_ + c->x;                             \
        if ((pen) && ((b_ ^ ea) & 0xff00))          \
            cyc++;                                  \
    } while (0)
#define ABSY(pen)                                   \
    do {                                            \
        uint16_t b_ = fetch16(c);                   \
        ea = b_ + c->y;                             \
        if ((pen) && ((b_ ^ ea) & 0xff00))          \
            cyc++;                                  \
    } while (0)
#define INDX()  (ea = read16_zp(c, (uint8_t)(fetch(c) + c->x)))
#define INDY(pen)                                   \
    do {                                            \
        uint16_t b_ = read16_zp(c, fetch(c));       \
        ea = b_ + c->y;                             \
        if ((pen) && ((b_ ^ ea) & 0xff00))          \
            cyc++;                                  \
    } while (0)
#define INDZ()  (ea = read16_zp(c, fetch(c)))
#define BRANCH(cond)                                        \
    do {                                                    \
        int8_t off_ = (int8_t)fetch(c);                     \
        if (cond) {                                         \
            uint16_t t_ = (uint16_t)(c->pc + off_);         \
            cyc += ((t_ ^ c->pc) & 0xff00) ? 2 : 1;         \
            c->pc = t_;                                     \
        }                                                   \
    } while (0)

    switch (op) {
    // ---- Loads
    case 0xa9: c->a = fetch(c); set_nz(c, c->a); break;
    case 0xa5: ZP(); goto lda;
    case 0xb5: ZPX(); goto lda;
    case 0xad: ABS(); goto lda;
    case 0xbd: ABSX(1); goto lda;
    case 0xb9: ABSY(1); goto lda;
    case 0xa1: INDX(); goto lda;
    case 0xb1: INDY(1); goto lda;
    case 0xb2: INDZ();
    lda:
        c->a = RD(ea);
        set_nz(c, c->a);
        break;
    case 0xa2: c->x = fetch(c); set_nz(c, c->x); break;
    case 0xa6: ZP(); goto ldx;
    case 0xb6: ZPY(); goto ldx;
    case 0xae: ABS(); goto ldx;
    case 0xbe: ABSY(1);
    ldx:
        c->x = RD(ea);
        set_nz(c, c->x);
        break;
    case 0xa0: c->y = fetch(c); set_nz(c, c->y); break;
    case 0xa4: ZP(); goto ldy;
    case 0xb4: ZPX(); goto ldy;
    case 0xac: ABS(); goto ldy;
    case 0xbc: ABSX(1);
    ldy:
        c->y = RD(ea);
        set_nz(c, c->y);
        break;

    // ---- Stores
    case 0x85: ZP(); goto sta;
    case 0x95: ZPX(); goto sta;
    case 0x8d: ABS(); goto sta;
    case 0x9d: ABSX(0); goto sta;
    case 0x99: ABSY(0); goto sta;
    case 0x81: INDX(); goto sta;
    case 0x91: INDY(0); goto sta;
    case 0x92: INDZ();
    sta:
        WR(ea, c->a);
        break;
    case 0x86: ZP(); WR(ea, c->x); break;
    case 0x96: ZPY(); WR(ea, c->x); break;
    case 0x8e: ABS(); WR(ea, c->x); break;
    case 0x84: ZP(); WR(ea, c->y); break;
    case 0x94: ZPX(); WR(ea, c->y); break;
    case 0x8c: ABS(); WR(ea, c->y); break;
    case 0x64: ZP(); WR(ea, 0); break;
    case 0x74: ZPX(); WR(ea, 0); break;
    case 0x9c: ABS(); WR(ea, 0); break;
    case 0x9e: ABSX(0); WR(ea, 0); break;

    // ---- Arithmetic and logic: aaa bbb 01 group, plus (zp) forms
#define ALU_GROUP(base, zpind, body)                            \
    case base + 0x09: m = fetch(c); body; break;                \
    case base + 0x05: ZP(); m = RD(ea); body; break;            \
    case base + 0x15: ZPX(); m = RD(ea); body; break;           \
    case base + 0x0d: ABS(); m = RD(ea); body; break;           \
    case base + 0x1d: ABSX(1); m = RD(ea); body; break;         \
    case base + 0x19: ABSY(1); m = RD(ea); body; break;         \
    case base + 0x01: INDX(); m = RD(ea); body; break;          \
    case base + 0x11: INDY(1); m = RD(ea); body; break;         \
    case zpind: INDZ(); m = RD(ea); body; break;
    ALU_GROUP(0x00, 0x12, (c->a |= m, set_nz(c, c->a)))
    ALU_GROUP(0x20, 0x32, (c->a &= m, set_nz(c, c->a)))
    ALU_GROUP(0x40, 0x52, (c->a ^= m, set_nz(c, c->a)))
    ALU_GROUP(0x60, 0x72, adc(c, m, &cyc))
    ALU_GROUP(0xc0, 0xd2, compare(c, c->a, m))
    ALU_GROUP(0xe0, 0xf2, sbc(c, m, &cyc))
#undef ALU_GROUP

    case 0xe0: compare(c, c->x, fetch(c)); break;
    case 0xe4: ZP(); compare(c, c->x, RD(ea)); break;
    case 0xec: ABS(); compare(c, c->x, RD(ea)); break;
    case 0xc0: compare(c, c->y, fetch(c)); break;
    case 0xc4: ZP(); compare(c, c->y, RD(ea)); break;
    case 0xcc: ABS(); compare(c, c->y, RD(ea)); break;

    case 0x89: c->p = (c->p & ~AS_Z) | ((c->a & fetch(c)) ? 0 : AS_Z); break;
    case 0x24: ZP(); bit(c, RD(ea)); break;
    case 0x34: ZPX(); bit(c, RD(ea)); break;
    case 0x2c: ABS(); bit(c, RD(ea)); break;
    case 0x3c: ABSX(1); bit(c, RD(ea)); break;

    case 0x04: ZP(); goto tsb;
    case 0x0c: ABS();
    tsb:
        m = RD(ea);
        c->p = (c->p & ~AS_Z) | ((c->a & m) ? 0 : AS_Z);
        WR(ea, m | c->a);
        break;
    case 0x14: ZP(); goto trb;
    case 0x1c: ABS();
    trb:
        m = RD(ea);
        c->p = (c->p & ~AS_Z) | ((c->a & m) ? 0 : AS_Z);
        WR(ea, m & ~c->a);
        break;

    // ---- Shifts, increments (read-modify-write)
#define RMW_GROUP(base, fn)                                     \
    case base + 0x06: ZP(); goto rmw_##fn;                      \
    case base + 0x16: ZPX(); goto rmw_##fn;                     \
    case base + 0x0e: ABS(); goto rmw_##fn;                     \
    case base + 0x1e: ABSX(1);                                  \
    rmw_##fn:                                                   \
        WR(ea, fn(c, RD(ea)));                                  \
        break;
    RMW_GROUP(0x00, asl)
    RMW_GROUP(0x20, rol)
    RMW_GROUP(0x40, lsr)
    RMW_GROUP(0x60, ror)
#undef RMW_GROUP
    case 0x0a: c->a = asl(c, c->a); break;
    case 0x2a: c->a = rol(c, c->a); break;
    case 0x4a: c->a = lsr(c, c->a); break;
    case 0x6a: c->a = ror(c, c->a); break;

    case 0xe6: ZP(); goto inc;
    case 0xf6: ZPX(); goto inc;
    case 0xee: ABS(); goto inc;
    case 0xfe: ABSX(0);
    inc:
        m = RD(ea) + 1;
        WR(ea, m);
        set_nz(c, m);
        break;
    case 0xc6: ZP(); goto dec;
    case 0xd6: ZPX(); goto dec;
    case 0xce: ABS(); goto dec;
    case 0xde: ABSX(0);
    dec:
        m = RD(ea) - 1;
        WR(ea, m);
        set_nz(c, m);
        break;
    case 0x1a: set_nz(c, ++c->a); break;
    case 0x3a: set_nz(c, --c->a); break;
    case 0xe8: set_nz(c, ++c->x); break;
    case 0xca: set_nz(c, --c->x); break;
    case 0xc8: set_nz(c, ++c->y); break;
    case 0x88: set_nz(c, --c->y); break;

    // ---- Transfers and stack
    case 0xaa: c->x = c->a; set_nz(c, c->x); break;
    case 0x8a: c->a = c->x; set_nz(c, c->a); break;
    case 0xa8: c->y = c->a; set_nz(c, c->y); break;
    case 0x98: c->a = c->y; set_nz(c, c->a); break;
    case 0xba: c->x = c->s; set_nz(c, c->x); break;
    case 0x9a: c->s = c->x; break;
    case 0x48: as_cpu_push(c, c->a); break;
    case 0x68: c->a = pull(c); set_nz(c, c->a); break;
    case 0xda: as_cpu_push(c, c->x); break;
    case 0xfa: c->x = pull(c); set_nz(c, c->x); break;
    case 0x5a: as_cpu_push(c, c->y); break;
    case 0x7a: c->y = pull(c); set_nz(c, c->y); break;
    case 0x08: as_cpu_push(c, c->p | AS_B | AS_U); break;
    case 0x28: c->p = (pull(c) | AS_U) & ~AS_B; break;

    // ---- Flags
    case 0x18: c->p &= ~AS_C; break;
    case 0x38: c->p |= AS_C; break;
    case 0x58: c->p &= ~AS_I; break;
    case 0x78: c->p |= AS_I; break;
    case 0xb8: c->p &= ~AS_V; break;
    case 0xd8: c->p &= ~AS_D; break;
    case 0xf8: c->p |= AS_D; break;

    // ---- Jumps and branches
    case 0x4c: c->pc = fetch16(c); break;
    case 0x6c: ea = fetch16(c); c->pc = RD(ea) | (uint16_t)RD(ea + 1) << 8; break;
    case 0x7c: ea = fetch16(c) + c->x; c->pc = RD(ea) | (uint16_t)RD(ea + 1) << 8; break;
    case 0x20: {
        uint16_t t = fetch16(c);
        uint16_t ret = c->pc - 1;
        as_cpu_push(c, ret >> 8);
        as_cpu_push(c, ret & 0xff);
        c->pc = t;
        break;
    }
    case 0x60: {
        uint16_t lo = pull(c);
        c->pc = (lo | (uint16_t)pull(c) << 8) + 1;
        break;
    }
    case 0x40: {
        c->p = (pull(c) | AS_U) & ~AS_B;
        uint16_t lo = pull(c);
        c->pc = lo | (uint16_t)pull(c) << 8;
        break;
    }
    case 0x00: {
        uint16_t ret = c->pc + 1;
        as_cpu_push(c, ret >> 8);
        as_cpu_push(c, ret & 0xff);
        as_cpu_push(c, c->p | AS_B | AS_U);
        c->p = (c->p | AS_I) & ~AS_D;
        c->pc = RD(0xfffe) | (uint16_t)RD(0xffff) << 8;
        break;
    }
    case 0x10: BRANCH(!(c->p & AS_N)); break;
    case 0x30: BRANCH(c->p & AS_N); break;
    case 0x50: BRANCH(!(c->p & AS_V)); break;
    case 0x70: BRANCH(c->p & AS_V); break;
    case 0x90: BRANCH(!(c->p & AS_C)); break;
    case 0xb0: BRANCH(c->p & AS_C); break;
    case 0xd0: BRANCH(!(c->p & AS_Z)); break;
    case 0xf0: BRANCH(c->p & AS_Z); break;
    case 0x80: BRANCH(1); break;

    // ---- NOPs, defined and undefined, with the operand bytes they skip
    case 0xea: break;
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xc2: case 0xe2:
    case 0x44: case 0x54: case 0xd4: case 0xf4:
        c->pc += 1;
        break;
    case 0x5c: case 0xdc: case 0xfc:
        c->pc += 2;
        break;
    default:  // x3, x7, xB, xF: one-byte NOPs on this 65C02
        break;
    }
    c->cycles += cyc;
    return cyc;
#undef ZP
#undef ZPX
#undef ZPY
#undef ABS
#undef ABSX
#undef ABSY
#undef INDX
#undef INDY
#undef INDZ
#undef BRANCH
}
