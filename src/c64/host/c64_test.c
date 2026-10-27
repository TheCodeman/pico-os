/**
 * @file c64_test.c
 * @brief Tests of the C64 core that need no ROMs.
 *
 * the 6510 against the Applesoft app's 65C02 on every documented opcode
 * (binary mode, where the two agree), NMOS decimal mode against Bruce
 * Clark's description of it, the undocumented opcodes, interrupts, the CIA
 * timers, and the machine's banking, keyboard, VIC-II text, bitmap and
 * sprites and raster interrupt with small programs in a made-up KERNAL.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_cpu.h"
#include "c64.h"
#include "c64_cart.h"
#include "c64_d64.h"
#include "c64_drive.h"
#include "c64_via.h"
#include "c64_sid.h"

static int failures, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        ++checks;                                           \
        if (!(cond)) {                                      \
            ++failures;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// ---- A flat 64 KB for CPU tests -------------------------------------------------

typedef struct {
    uint8_t mem[0x10000];
    uint16_t wrote[16];
    int nwrote;
} flat;

static uint8_t flat_read(void *ctx, uint16_t a) {
    return ((flat *)ctx)->mem[a];
}

static void flat_write(void *ctx, uint16_t a, uint8_t v) {
    flat *f = ctx;
    f->mem[a] = v;
    if (f->nwrote < 16)
        f->wrote[f->nwrote++] = a;
}

static flat fa, fb;

static void cpu_on(c64_cpu *c, flat *f) {
    memset(c, 0, sizeof *c);
    c->read = flat_read;
    c->write = flat_write;
    c->ctx = f;
}

/// Runs the bytes at $0200 until BRK at the end; returns the cycles.
static int run_prog(c64_cpu *c, const uint8_t *code, size_t n) {
    memset(fa.mem, 0, sizeof fa.mem);
    memcpy(fa.mem + 0x200, code, n);
    fa.mem[0xfffe] = 0x00;
    fa.mem[0xffff] = 0xff;  // BRK goes to $FF00
    c->pc = 0x200;
    int cyc = 0;
    for (int i = 0; i < 1000 && c->pc != 0xff00; ++i)
        cyc += c64_cpu_step(c);
    return cyc - 7;  // without the BRK
}

// ---- 6510 against the 65C02 ------------------------------------------------------

static const uint8_t documented[] = {
    0x00, 0x01, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0d, 0x0e, 0x10, 0x11, 0x15, 0x16, 0x18, 0x19, 0x1d,
    0x1e, 0x20, 0x21, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a, 0x2c, 0x2d, 0x2e, 0x30, 0x31, 0x35, 0x36,
    0x38, 0x39, 0x3d, 0x3e, 0x40, 0x41, 0x45, 0x46, 0x48, 0x49, 0x4a, 0x4c, 0x4d, 0x4e, 0x50, 0x51,
    0x55, 0x56, 0x58, 0x59, 0x5d, 0x5e, 0x60, 0x61, 0x65, 0x66, 0x68, 0x69, 0x6a, 0x6c, 0x6d, 0x6e,
    0x70, 0x71, 0x75, 0x76, 0x78, 0x79, 0x7d, 0x7e, 0x81, 0x84, 0x85, 0x86, 0x88, 0x8a, 0x8c, 0x8d,
    0x8e, 0x90, 0x91, 0x94, 0x95, 0x96, 0x98, 0x99, 0x9a, 0x9d, 0xa0, 0xa1, 0xa2, 0xa4, 0xa5, 0xa6,
    0xa8, 0xa9, 0xaa, 0xac, 0xad, 0xae, 0xb0, 0xb1, 0xb4, 0xb5, 0xb6, 0xb8, 0xb9, 0xba, 0xbc, 0xbd,
    0xbe, 0xc0, 0xc1, 0xc4, 0xc5, 0xc6, 0xc8, 0xc9, 0xca, 0xcc, 0xcd, 0xce, 0xd0, 0xd1, 0xd5, 0xd6,
    0xd8, 0xd9, 0xdd, 0xde, 0xe0, 0xe1, 0xe4, 0xe5, 0xe6, 0xe8, 0xe9, 0xea, 0xec, 0xed, 0xee, 0xf0,
    0xf1, 0xf5, 0xf6, 0xf8, 0xf9, 0xfd, 0xfe,
};

static uint8_t c02_read(void *ctx, uint16_t a) { return flat_read(ctx, a); }
static void c02_write(void *ctx, uint16_t a, uint8_t v) { flat_write(ctx, a, v); }

static void test_against_65c02(void) {
    CHECK(sizeof documented == 151, "%zu documented opcodes", sizeof documented);
    int bad[256] = {0};
    for (int iter = 0; iter < 400000; ++iter) {
        uint8_t op = documented[rnd() % sizeof documented];
        if (op == 0x6c)
            continue;  // JMP (ind): the NMOS page bug, tested on its own
        for (int i = 0; i < 0x10000; i += 4) {
            uint32_t r = rnd();
            memcpy(fa.mem + i, &r, 4);
        }
        uint16_t pc = rnd() & 0xffff;
        fa.mem[pc] = op;
        memcpy(fb.mem, fa.mem, sizeof fa.mem);
        fa.nwrote = fb.nwrote = 0;

        c64_cpu c;
        cpu_on(&c, &fa);
        as_cpu d = {0};
        d.read = c02_read;
        d.write = c02_write;
        d.ctx = &fb;
        c.pc = d.pc = pc;
        c.a = d.a = (uint8_t)rnd();
        c.x = d.x = (uint8_t)rnd();
        c.y = d.y = (uint8_t)rnd();
        c.s = d.s = (uint8_t)rnd();
        c.p = d.p = (uint8_t)((rnd() & ~C64_D) | C64_U);
        c64_cpu_step(&c);
        as_cpu_step(&d);

        bool same = c.pc == d.pc && c.a == d.a && c.x == d.x && c.y == d.y && c.s == d.s &&
                    (c.p | C64_B) == (d.p | AS_B);
        for (int i = 0; i < fa.nwrote; ++i)
            same &= fa.mem[fa.wrote[i]] == fb.mem[fa.wrote[i]];
        for (int i = 0; i < fb.nwrote; ++i)
            same &= fa.mem[fb.wrote[i]] == fb.mem[fb.wrote[i]];
        if (!same && bad[op]++ == 0)
            CHECK(0, "opcode %02X differs from the 65C02: pc %04X/%04X a %02X/%02X x %02X/%02X "
                     "y %02X/%02X s %02X/%02X p %02X/%02X",
                  op, c.pc, d.pc, c.a, d.a, c.x, d.x, c.y, d.y, c.s, d.s, c.p, d.p);
        else
            ++checks;
    }
}

// ---- Decimal mode ----------------------------------------------------------------

static void test_decimal(void) {
    c64_cpu c;
    cpu_on(&c, &fa);
    int bad_adc = 0, bad_sbc = 0;
    for (int a = 0; a < 256; ++a)
        for (int b = 0; b < 256; ++b)
            for (int carry = 0; carry < 2; ++carry) {
                // ADC (Clark, sequences 1 and 2; Z from the binary sum).
                int al = (a & 15) + (b & 15) + carry;
                if (al >= 10)
                    al = ((al + 6) & 15) + 16;
                int s1 = (a & 0xf0) + (b & 0xf0) + al;
                int s2 = (int)(int8_t)(a & 0xf0) + (int)(int8_t)(b & 0xf0) + al;
                bool n = s1 & 0x80, v = s2 < -128 || s2 > 127, z = !((a + b + carry) & 0xff);
                if (s1 >= 0xa0)
                    s1 += 0x60;
                uint8_t want_a = (uint8_t)s1;
                bool want_c = s1 >= 0x100;
                fa.mem[0x300] = 0x69;  // ADC #b
                fa.mem[0x301] = (uint8_t)b;
                c.pc = 0x300;
                c.a = (uint8_t)a;
                c.p = C64_U | C64_D | (carry ? C64_C : 0);
                c64_cpu_step(&c);
                if (c.a != want_a || !!(c.p & C64_C) != want_c || !!(c.p & C64_N) != n ||
                    !!(c.p & C64_V) != v || !!(c.p & C64_Z) != z) {
                    if (bad_adc++ < 3)
                        CHECK(0, "ADC %02X+%02X+%d: a %02X p %02X, want a %02X c%d n%d v%d z%d", a, b,
                              carry, c.a, c.p, want_a, want_c, n, v, z);
                }

                // SBC (Clark, sequence 3; the flags are the binary ones).
                al = (a & 15) - (b & 15) + carry - 1;
                if (al < 0)
                    al = ((al - 6) & 15) - 16;
                int s3 = (a & 0xf0) - (b & 0xf0) + al;
                if (s3 < 0)
                    s3 -= 0x60;
                int bin = a - b - (1 - carry);
                fa.mem[0x300] = 0xe9;  // SBC #b
                c.pc = 0x300;
                c.a = (uint8_t)a;
                c.p = C64_U | C64_D | (carry ? C64_C : 0);
                c64_cpu_step(&c);
                bool sv = ((a ^ b) & (a ^ bin) & 0x80) != 0;
                if (c.a != (uint8_t)s3 || !!(c.p & C64_C) != (bin >= 0) ||
                    !!(c.p & C64_N) != !!(bin & 0x80) || !!(c.p & C64_Z) != !(bin & 0xff) ||
                    !!(c.p & C64_V) != sv) {
                    if (bad_sbc++ < 3)
                        CHECK(0, "SBC %02X-%02X c%d: a %02X p %02X, want a %02X", a, b, carry, c.a,
                              c.p, (uint8_t)s3);
                }
            }
    CHECK(bad_adc == 0, "%d decimal ADC cases wrong", bad_adc);
    CHECK(bad_sbc == 0, "%d decimal SBC cases wrong", bad_sbc);
}

// ---- Undocumented opcodes, cycles and interrupts ------------------------------------

static void test_undocumented(void) {
    c64_cpu c;
    cpu_on(&c, &fa);

    // LAX, SAX, SLO, DCP and ISC, with the memory set up by the program.
    {
        const uint8_t p[] = {0xa9, 0x5a, 0x85, 0x10,  // $10 = $5A
                             0xa9, 0x81, 0x85, 0x12,  // $12 = $81
                             0xa9, 0x05, 0x85, 0x13,  // $13 = $05
                             0xa9, 0x10, 0x85, 0x14,  // $14 = $10
                             0xa2, 0x0f, 0xa7, 0x10,  // LDX #$0F ; LAX $10
                             0xe0, 0x5a,              // CPX #$5A (Z set if LAX loaded X)
                             0x08, 0x68, 0x85, 0x20,  // $20 = P
                             0xa9, 0x5a, 0xa2, 0x0f, 0x87, 0x11,  // LDA #$5A ; LDX #$0F ; SAX $11
                             0xa9, 0x01, 0x07, 0x12,  // LDA #$01 ; SLO $12
                             0x85, 0x21,              // $21 = A ($03)
                             0xa9, 0x05, 0xc7, 0x13,  // LDA #$05 ; DCP $13 ($04; A > M: C, not Z)
                             0x08, 0x68, 0x85, 0x22,  // $22 = P
                             0x38, 0xa9, 0x20, 0xe7, 0x14,  // SEC ; LDA #$20 ; ISC $14 ($11; A = $0F)
                             0x85, 0x23,
                             0x00};
        c.p = C64_U;
        c.s = 0xff;
        run_prog(&c, p, sizeof p);
        CHECK(fa.mem[0x20] & C64_Z, "LAX loads X");
        CHECK(fa.mem[0x11] == 0x0a, "SAX stores A & X: %02X", fa.mem[0x11]);
        CHECK(fa.mem[0x12] == 0x02 && fa.mem[0x21] == 0x03, "SLO: %02X %02X", fa.mem[0x12], fa.mem[0x21]);
        CHECK(fa.mem[0x13] == 0x04 && (fa.mem[0x22] & (C64_C | C64_Z)) == C64_C, "DCP: %02X p %02X",
              fa.mem[0x13], fa.mem[0x22]);
        CHECK(fa.mem[0x14] == 0x11 && fa.mem[0x23] == 0x0f, "ISC: %02X %02X", fa.mem[0x14], fa.mem[0x23]);
    }

    // Immediate ones, one at a time.
    struct { uint8_t op, a, x, imm, p_in; uint8_t want_a, want_x, p_mask, want_p; } imm[] = {
        {0x0b, 0xf0, 0, 0x80, 0, 0x80, 0, C64_C | C64_N, C64_C | C64_N},      // ANC
        {0x4b, 0xff, 0, 0x03, 0, 0x01, 0, C64_C, C64_C},                      // ALR
        {0x6b, 0xff, 0, 0xff, C64_C, 0xff, 0, C64_C | C64_V | C64_N, C64_C | C64_N},  // ARR
        {0x6b, 0xff, 0, 0x7f, 0, 0x3f, 0, C64_C | C64_V, C64_V},             // ARR: V = b6^b5
        {0xcb, 0xf0, 0x3c, 0x10, 0, 0xf0, 0x20, C64_C, C64_C},               // SBX
        {0xcb, 0x0f, 0x3c, 0x10, 0, 0x0f, 0xfc, C64_C | C64_N, C64_N},       // SBX borrow
        {0xeb, 0x10, 0, 0x01, C64_C, 0x0f, 0, C64_C, C64_C},                  // SBC # (EB)
        {0xab, 0x00, 0, 0x5f, 0, 0x4e, 0x4e, 0, 0},                           // LXA (magic $EE)
        {0x80, 0x12, 0x34, 0x56, 0, 0x12, 0x34, 0, 0},                        // NOP #
    };
    for (size_t i = 0; i < sizeof imm / sizeof imm[0]; ++i) {
        memset(fa.mem, 0, 0x400);
        fa.mem[0x300] = imm[i].op;
        fa.mem[0x301] = imm[i].imm;
        c.pc = 0x300;
        c.a = imm[i].a;
        c.x = imm[i].x;
        c.p = C64_U | imm[i].p_in;
        int cyc = c64_cpu_step(&c);
        CHECK(c.a == imm[i].want_a && c.x == imm[i].want_x && (c.p & imm[i].p_mask) == imm[i].want_p &&
                  cyc == 2 && c.pc == 0x302,
              "opcode %02X: a %02X x %02X p %02X cycles %d", imm[i].op, c.a, c.x, c.p, cyc);
    }

    // JMP ($10FF) takes its high byte from $1000.
    memset(fa.mem, 0, sizeof fa.mem);
    fa.mem[0x300] = 0x6c, fa.mem[0x301] = 0xff, fa.mem[0x302] = 0x10;
    fa.mem[0x10ff] = 0x34, fa.mem[0x1000] = 0x12, fa.mem[0x1100] = 0x56;
    c.pc = 0x300;
    CHECK(c64_cpu_step(&c) == 5 && c.pc == 0x1234, "JMP (ind) page bug: %04X", c.pc);

    // Cycles: page crossings on reads, not on stores; branches.
    {
        const struct { uint8_t code[4]; uint8_t x; int want; } t[] = {
            {{0xbd, 0xf0, 0x10}, 0x0f, 4},  // LDA $10F0,X  no crossing
            {{0xbd, 0xf0, 0x10}, 0x10, 5},  // LDA $10F0,X  crossing
            {{0x9d, 0xf0, 0x10}, 0x10, 5},  // STA $10F0,X  always 5
            {{0xfe, 0xf0, 0x10}, 0x00, 7},  // INC $10F0,X  always 7
            {{0x1c, 0xf0, 0x10}, 0x10, 5},  // NOP $10F0,X  crossing
            {{0x1f, 0xf0, 0x10}, 0x10, 7},  // SLO $10F0,X
            {{0xd0, 0x02}, 0, 3},           // BNE taken (Z clear)
            {{0xd0, 0x7f}, 0, 4},           // BNE taken across a page
            {{0xf0, 0x02}, 0, 2},           // BEQ not taken
        };
        for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i) {
            memcpy(fa.mem + 0x3f0, t[i].code, 4);
            c.pc = 0x3f0;
            c.x = t[i].x;
            c.p = C64_U;
            int cyc = c64_cpu_step(&c);
            CHECK(cyc == t[i].want, "cycles of %02X (case %zu): %d, want %d", t[i].code[0], i, cyc, t[i].want);
        }
    }

    // SHX $10F0,Y stores X & $11; across a page that value is the high byte.
    memset(fa.mem, 0, sizeof fa.mem);
    fa.mem[0x300] = 0x9e, fa.mem[0x301] = 0xf0, fa.mem[0x302] = 0x10;
    c.pc = 0x300, c.x = 0xff, c.y = 0x05;
    CHECK(c64_cpu_step(&c) == 5 && fa.mem[0x10f5] == 0x11, "SHX");
    c.pc = 0x300, c.x = 0x0f, c.y = 0x20;
    c64_cpu_step(&c);
    CHECK(fa.mem[0x0110] == 0x01 && fa.mem[0x1110] == 0, "SHX across a page");
    // LAS $1000,Y: A, X and S all get M & S.
    fa.mem[0x300] = 0xbb, fa.mem[0x301] = 0x00, fa.mem[0x302] = 0x10;
    fa.mem[0x1000] = 0xf3;
    c.pc = 0x300, c.y = 0, c.s = 0x3c;
    c64_cpu_step(&c);
    CHECK(c.a == 0x30 && c.x == 0x30 && c.s == 0x30, "LAS");

    // A JAM stops the CPU.
    fa.mem[0x300] = 0x02;
    c.pc = 0x300;
    c64_cpu_step(&c);
    c64_cpu_step(&c);
    CHECK(c.jammed && c.pc == 0x300, "JAM");
    c.jammed = false;

    // Read-modify-write writes the old value, then the new.
    memset(fa.mem, 0, sizeof fa.mem);
    fa.mem[0x300] = 0xee, fa.mem[0x301] = 0x19, fa.mem[0x302] = 0xd0;  // INC $D019
    fa.mem[0xd019] = 0x81;
    fa.nwrote = 0;
    c.pc = 0x300;
    c64_cpu_step(&c);
    CHECK(fa.nwrote == 2 && fa.wrote[0] == 0xd019 && fa.mem[0xd019] == 0x82, "INC writes twice");
}

static void test_interrupts(void) {
    c64_cpu c;
    cpu_on(&c, &fa);
    memset(fa.mem, 0, sizeof fa.mem);
    fa.mem[0xfffc] = 0x00, fa.mem[0xfffd] = 0x03;  // reset to $0300
    fa.mem[0xfffe] = 0x00, fa.mem[0xffff] = 0x40;  // IRQ/BRK to $4000
    fa.mem[0xfffa] = 0x00, fa.mem[0xfffb] = 0x50;  // NMI to $5000
    fa.mem[0x300] = 0x58;  // CLI
    fa.mem[0x301] = 0xea;  // NOP
    c64_cpu_reset(&c);
    CHECK(c.pc == 0x300 && c.s == 0xfd && (c.p & C64_I), "reset");
    c.irq = true;
    c64_cpu_step(&c);  // CLI
    CHECK(c64_cpu_step(&c) == 7 && c.pc == 0x4000, "IRQ taken: %04X", c.pc);
    CHECK(!(fa.mem[0x1fb] & C64_B) && fa.mem[0x1fc] == 0x01 && fa.mem[0x1fd] == 0x03, "IRQ pushes P without B");
    CHECK(c.p & C64_I, "IRQ sets I");
    // The IRQ is masked now; an NMI isn't.
    fa.mem[0x4000] = 0xea;
    c64_cpu_step(&c);
    CHECK(c.pc == 0x4001, "IRQ masked");
    c.nmi = true;
    c64_cpu_step(&c);
    CHECK(c.pc == 0x5000 && !c.nmi, "NMI taken");
    // BRK pushes the address after its padding byte, with B.
    c.irq = false;
    fa.mem[0x5000] = 0x00;
    c64_cpu_step(&c);
    CHECK(c.pc == 0x4000 && (fa.mem[0x100 + c.s + 1] & C64_B) && fa.mem[0x100 + c.s + 2] == 0x02,
          "BRK");
}

// ---- CIA ---------------------------------------------------------------------------

static void test_cia(void) {
    c64_cia c = {0};
    c64_cia_reset(&c);
    // Timer A continuous with latch 99: an underflow every 100 cycles.
    c64_cia_write(&c, 4, 99);
    c64_cia_write(&c, 5, 0);
    c64_cia_write(&c, 0x0d, 0x81);
    c64_cia_write(&c, 0x0e, 0x11);  // load, start
    c64_cia_tick(&c, 99);
    CHECK(!c.irq && c.ta == 0, "timer A counts down: %u", c.ta);
    c64_cia_tick(&c, 1);
    CHECK(c.irq && c.ta == 99, "timer A underflows: %u", c.ta);
    CHECK(c64_cia_read(&c, 0x0d) == 0x81 && !c.irq && c64_cia_read(&c, 0x0d) == 0, "ICR read clears");
    c64_cia_tick(&c, 250);  // 2 underflows, 50 into the next
    CHECK(c.irq && c.ta == 49, "timer A after 250: %u", c.ta);

    // Timer B counting timer A's underflows, one-shot.
    c64_cia_reset(&c);
    c64_cia_write(&c, 4, 9), c64_cia_write(&c, 5, 0);
    c64_cia_write(&c, 6, 2), c64_cia_write(&c, 7, 0);
    c64_cia_write(&c, 0x0d, 0x82);
    c64_cia_write(&c, 0x0f, 0x59);  // count TA, one-shot, load, start
    c64_cia_write(&c, 0x0e, 0x11);
    c64_cia_tick(&c, 29);
    CHECK(!c.irq && c.tb == 0, "timer B counted 2 of TA: %u", c.tb);
    c64_cia_tick(&c, 10);
    CHECK(c.irq && !(c.crb & 1), "timer B one-shot fired and stopped");

    // One-shot timer A stops.
    c64_cia_reset(&c);
    c64_cia_write(&c, 4, 5), c64_cia_write(&c, 5, 0);
    c64_cia_write(&c, 0x0e, 0x19);
    c64_cia_tick(&c, 100);
    CHECK(!(c.cra & 1) && c.ta == 5 && (c64_cia_read(&c, 0x0d) & 1), "one-shot timer A");

    // TOD: counts tenths; reading the hours latches.
    c64_cia_reset(&c);
    c64_cia_write(&c, 0x0b, 0x11);  // 11 AM, stops the clock
    c64_cia_write(&c, 0x0a, 0x59);
    c64_cia_write(&c, 0x09, 0x59);
    c64_cia_write(&c, 0x08, 0x09);  // starts it
    c64_cia_tod_tick(&c);
    CHECK(c64_cia_read(&c, 0x0b) == 0x92 && c64_cia_read(&c, 0x0a) == 0 && c64_cia_read(&c, 0x09) == 0 &&
              c64_cia_read(&c, 0x08) == 0,
          "TOD rolls over to 12 PM");
}

// ---- The machine -------------------------------------------------------------------

static uint8_t ram[0x10000], color[0x400], basic[0x2000], kernal[0x2000], charrom[0x1000];
static uint8_t frame[C64_SCREEN_W * C64_SCREEN_H];
static c64 m;

/// Puts code into the made-up KERNAL at $E000 and resets to it.
static void boot(const uint8_t *code, size_t n, const uint8_t *irq, size_t irq_n) {
    memset(ram, 0, sizeof ram);
    memset(color, 0, sizeof color);
    memset(kernal, 0xea, sizeof kernal);
    memcpy(kernal, code, n);
    if (irq)
        memcpy(kernal + 0x1000, irq, irq_n);  // IRQ handler at $F000
    kernal[0x1ffc] = 0x00, kernal[0x1ffd] = 0xe0;
    kernal[0x1ffe] = 0x00, kernal[0x1fff] = 0xf0;
    kernal[0x1ffa] = 0x00, kernal[0x1ffb] = 0xf0;
    for (int i = 0; i < 0x2000; ++i)
        basic[i] = (uint8_t)(i ^ 0xa5);
    // Character n's rows are n; character 1 ('A') is solid.
    for (int i = 0; i < 0x1000; ++i)
        charrom[i] = (uint8_t)(i >> 3);
    memset(charrom + 8, 0xff, 8);
    memset(&m, 0, sizeof m);
    m.ram = ram, m.color = color, m.basic = basic, m.kernal = kernal, m.charrom = charrom;
    m.vic.frame = frame;
    c64_init(&m);
}

static uint8_t pixel(int sx, int sy) {  // by sprite X / raster line
    return frame[(sy - C64_FIRST_LINE) * C64_SCREEN_W + (sx - C64_FIRST_X)];
}

static void test_banking(void) {
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    boot(loop, sizeof loop, NULL, 0);
    // After reset the port is all inputs: everything banked in.
    CHECK(c64_peek(&m, 0xa000) == basic[0] && c64_peek(&m, 0xe000) == 0x4c, "ROMs in after reset");
    c64_poke(&m, 0, 0x2f);
    c64_poke(&m, 1, 0x37);
    c64_poke(&m, 0xa000, 0x11);  // lands in RAM under BASIC
    CHECK(c64_peek(&m, 0xa000) == basic[0] && ram[0xa000] == 0x11, "write under ROM");
    c64_poke(&m, 1, 0x36);  // BASIC out
    CHECK(c64_peek(&m, 0xa000) == 0x11 && c64_peek(&m, 0xe000) == 0x4c, "BASIC out, KERNAL in");
    c64_poke(&m, 1, 0x35);  // KERNAL and BASIC out, I/O in
    CHECK(c64_peek(&m, 0xe000) == 0x00 && c64_peek(&m, 0xa000) == 0x11, "both ROMs out");
    c64_poke(&m, 0xd020, 0x05);
    CHECK(m.vic.reg[0x20] == 5 && ram[0xd020] == 0, "I/O in");
    c64_poke(&m, 1, 0x33);  // character ROM
    CHECK(c64_peek(&m, 0xd008) == 0xff && c64_peek(&m, 0xd010) == 0x02, "character ROM in");
    c64_poke(&m, 0xd400, 0x77);
    CHECK(ram[0xd400] == 0x77, "write under the character ROM");
    c64_poke(&m, 1, 0x30);  // all RAM
    CHECK(c64_peek(&m, 0xd400) == 0x77 && c64_peek(&m, 0xe000) == 0, "all RAM");
    CHECK(c64_peek(&m, 1) == (0x30 | 0x10), "port reads the cassette sense: %02X", c64_peek(&m, 1));
    c64_poke(&m, 1, 0x37);
    c64_poke(&m, 0xd800, 0xf3);
    CHECK((c64_peek(&m, 0xd800) & 0x0f) == 3, "colour RAM");
}

static void test_keyboard(void) {
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    boot(loop, sizeof loop, NULL, 0);
    unsigned k;
    bool shift;
    CHECK(c64_ascii_key('A', &k, &shift) && k == 0x0a && !shift, "A is PA1/PB2");
    CHECK(c64_ascii_key('"', &k, &shift) && k == 0x3b && shift, "\" is shift-2");
    c64_key(&m, 0x0a, true);
    c64_poke(&m, 0xdc02, 0xff);
    c64_poke(&m, 0xdc03, 0x00);
    c64_poke(&m, 0xdc00, 0xfd);  // column 1
    CHECK(c64_peek(&m, 0xdc01) == 0xfb, "A seen on row 2: %02X", c64_peek(&m, 0xdc01));
    c64_poke(&m, 0xdc00, 0xfe);
    CHECK(c64_peek(&m, 0xdc01) == 0xff, "not on column 0");
    // Joystick 2 on port A, joystick 1 on port B.
    c64_key(&m, 0x0a, false);
    m.joy[1] = 0x10;
    CHECK(c64_peek(&m, 0xdc00) == 0xee, "joystick 2 fire: %02X", c64_peek(&m, 0xdc00));
    m.joy[0] = 0x01;
    CHECK(c64_peek(&m, 0xdc01) == 0xfe, "joystick 1 up");
}

static void test_video(void) {
    // Text: border 6, background 0; 'A' (solid) in colour 2 at row 0 col 0,
    // and at row 24 col 39; the VIC on bank 0 with the screen at $0400 and
    // the character ROM at $1000.
    static const uint8_t init[] = {
        0xa9, 0x06, 0x8d, 0x20, 0xd0,  // LDA #6 ; STA $D020
        0xa9, 0x00, 0x8d, 0x21, 0xd0,  // LDA #0 ; STA $D021
        0xa9, 0x1b, 0x8d, 0x11, 0xd0,  // LDA #$1B ; STA $D011
        0xa9, 0xc8, 0x8d, 0x16, 0xd0,  // LDA #$C8 ; STA $D016
        0xa9, 0x14, 0x8d, 0x18, 0xd0,  // LDA #$14 ; STA $D018
        0xa9, 0x01, 0x8d, 0x00, 0x04,  // LDA #1 ; STA $0400
        0x8d, 0xe7, 0x07,              // STA $07E7
        0xa9, 0x02, 0x8d, 0x00, 0xd8,  // LDA #2 ; STA $D800
        0x8d, 0xe7, 0xdb,              // STA $DBE7
        0x4c, 0x29, 0xe0,              // JMP *
    };
    boot(init, sizeof init, NULL, 0);
    memset(ram + 0x400, 0x20, 1000);  // character $20 everywhere (the program sets two)
    c64_run_frame(&m);
    c64_run_frame(&m);
    CHECK(pixel(24, 51) == 2 && pixel(31, 58) == 2, "character at the top left: %d", pixel(24, 51));
    CHECK(pixel(32, 51) == 0, "background beside it: %d", pixel(32, 51));
    CHECK(pixel(23, 51) == 6 && pixel(24, 50) == 6, "border round it: %d %d", pixel(23, 51), pixel(24, 50));
    CHECK(pixel(343, 250) == 2 && pixel(344, 250) == 6 && pixel(343, 251) == 6, "bottom right character");
    // Character $20's rows are $20: the third pixel. Colour RAM 0 makes it
    // black on black; give row 1 colour 1 to see it.
    memset(color + 40, 1, 40);
    c64_run_frame(&m);
    CHECK(pixel(24 + 2, 59) == 1 && pixel(24 + 3, 59) == 0, "space's pattern: %d %d", pixel(26, 59),
          pixel(27, 59));

    // 38 columns, 24 rows, and a scroll of 3 to the right.
    c64_poke(&m, 0xd016, 0xc3);
    c64_poke(&m, 0xd011, 0x13);
    c64_run_frame(&m);
    CHECK(pixel(30, 60) == 6 && pixel(31, 60) == 0, "38-column border: %d %d", pixel(30, 60), pixel(31, 60));
    CHECK(pixel(40, 54) == 6 && pixel(40, 55) != 6, "24-row border");
    CHECK(pixel(24 + 3 + 8 + 2, 59) == 1 && pixel(24 + 8 + 2, 59) == 0, "scrolled right by 3");

    // Display off: all border.
    c64_poke(&m, 0xd011, 0x0b);
    c64_run_frame(&m);
    c64_run_frame(&m);
    CHECK(pixel(100, 100) == 6, "blank screen");

    // Multicolour bitmap at $2000, screen $0400.
    c64_poke(&m, 0xd011, 0x3b);
    c64_poke(&m, 0xd016, 0xd8);
    c64_poke(&m, 0xd018, 0x18);
    c64_poke(&m, 0xd021, 0x0b);
    ram[0x2000] = 0x1b;  // 00 01 10 11
    ram[0x400] = 0x34;   // 01 -> 3, 10 -> 4
    color[0] = 0x05;     // 11 -> 5
    c64_run_frame(&m);
    CHECK(pixel(24, 51) == 0x0b && pixel(26, 51) == 3 && pixel(28, 51) == 4 && pixel(30, 51) == 5,
          "multicolour bitmap: %d %d %d %d", pixel(24, 51), pixel(26, 51), pixel(28, 51), pixel(30, 51));
}

static void test_midline(void) {
    // A border colour written part way along line 100 changes it from
    // there on: cycle 40's write lands at sprite X 228.
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    boot(loop, sizeof loop, NULL, 0);
    c64_poke(&m, 0xd011, 0x1b);
    c64_poke(&m, 0xd020, 6);
    c64_run_frame(&m);
    while (m.vic.raster != 100)
        c64_run_line(&m);
    c64_vic_line_start(&m.vic);
    c64_vic_line_fetch(&m.vic);
    m.vic.write_x = 0;
    c64_vic_write(&m.vic, 0x11, 0x13);  // 24 rows: line 100 is still open; nothing to see
    m.vic.write_x = (int16_t)c64_vic_cycle_x(40);
    c64_vic_write(&m.vic, 0x20, 2);
    m.vic.write_x = (int16_t)c64_vic_cycle_x(5);  // too late for line 100's start: the next
    c64_vic_line_finish(&m.vic);
    c64_vic_line_end(&m.vic);
    int x = c64_vic_cycle_x(40) + C64_FIRST_X;
    CHECK(x == 228, "cycle 40 is X %d", x);
    CHECK(pixel(0, 100) == 6 && pixel(20, 100) == 6, "left border as before");
    CHECK(pixel(350, 100) == 2 && pixel(370, 100) == 2, "right border the new colour");
    CHECK(c64_vic_cycle_x(3) == 0 && c64_vic_cycle_x(60) == C64_SCREEN_W, "before and after the picture");
}

static void test_raster_irq(void) {
    // Raster interrupt on line 100, counted at $02; CIA interrupts off.
    static const uint8_t init[] = {
        0x78,                          // SEI
        0xa9, 0x7f, 0x8d, 0x0d, 0xdc,  // LDA #$7F ; STA $DC0D
        0xa9, 0x1b, 0x8d, 0x11, 0xd0,  // LDA #$1B ; STA $D011
        0xa9, 0x64, 0x8d, 0x12, 0xd0,  // LDA #100 ; STA $D012
        0xa9, 0x01, 0x8d, 0x1a, 0xd0,  // LDA #1 ; STA $D01A
        0x8d, 0x19, 0xd0,              // STA $D019 (line 0 matched the compare)
        0x58,                          // CLI
        0x4c, 0x19, 0xe0,              // JMP *
    };
    static const uint8_t irq[] = {
        0xe6, 0x02,                    // INC $02
        0xad, 0x12, 0xd0, 0x85, 0x03,  // LDA $D012 ; STA $03
        0xee, 0x19, 0xd0,              // INC $D019 (acknowledge)
        0x40,                          // RTI
    };
    boot(init, sizeof init, irq, sizeof irq);
    for (int i = 0; i < 5; ++i)
        c64_run_frame(&m);
    CHECK(ram[2] == 5 && ram[3] == 100, "5 raster interrupts in 5 frames: %d, on line %d", ram[2], ram[3]);

    // CIA 1 timer A at the KERNAL's PAL rate ($4025): about 60 a second.
    static const uint8_t init2[] = {
        0xa9, 0x25, 0x8d, 0x04, 0xdc,  // LDA #$25 ; STA $DC04
        0xa9, 0x40, 0x8d, 0x05, 0xdc,  // LDA #$40 ; STA $DC05
        0xa9, 0x81, 0x8d, 0x0d, 0xdc,  // LDA #$81 ; STA $DC0D
        0xa9, 0x11, 0x8d, 0x0e, 0xdc,  // LDA #$11 ; STA $DC0E
        0x58,                          // CLI
        0x4c, 0x15, 0xe0,              // JMP *
    };
    static const uint8_t irq2[] = {
        0xe6, 0x02,                    // INC $02
        0xd0, 0x02, 0xe6, 0x03,        // BNE +2 ; INC $03
        0xad, 0x0d, 0xdc,              // LDA $DC0D (acknowledge)
        0x40,                          // RTI
    };
    boot(init2, sizeof init2, irq2, sizeof irq2);
    for (int i = 0; i < 50; ++i)  // a second
        c64_run_frame(&m);
    int n = ram[2] | ram[3] << 8;
    CHECK(n >= 59 && n <= 61, "timer interrupts in a second: %d", n);
    CHECK(m.cycles >= 50 * C64_FRAME_CYCLES && m.cycles < 50 * C64_FRAME_CYCLES + 10, "cycles: %llu",
          (unsigned long long)m.cycles);
}

static void test_sprites(void) {
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    boot(loop, sizeof loop, NULL, 0);
    c64_poke(&m, 0xd011, 0x1b);
    c64_poke(&m, 0xd016, 0xc8);
    c64_poke(&m, 0xd018, 0x14);
    c64_poke(&m, 0xd020, 6);
    c64_poke(&m, 0xd021, 0);
    memset(ram + 0x400, 0, 1000);  // character 0 is blank
    ram[0x400 + 5 * 40 + 5] = 1;  // a solid character at row 5 col 5: x 64-71, y 91-98
    color[5 * 40 + 5] = 2;
    // Sprite 0: solid, colour 7, at x 60 y 90 (shown from line 91); sprite 1
    // expanded both ways, colour 8, at x 200 y 150.
    memset(ram + 0x3000, 0xff, 63);
    ram[0x7f8] = 0xc0;  // $3000 / 64
    ram[0x7f9] = 0xc0;
    c64_poke(&m, 0xd000, 60);
    c64_poke(&m, 0xd001, 90);
    c64_poke(&m, 0xd002, 200);
    c64_poke(&m, 0xd003, 150);
    c64_poke(&m, 0xd017, 0x02);
    c64_poke(&m, 0xd01d, 0x02);
    c64_poke(&m, 0xd027, 7);
    c64_poke(&m, 0xd028, 8);
    c64_poke(&m, 0xd015, 0x03);
    c64_run_frame(&m);
    c64_run_frame(&m);
    CHECK(pixel(60, 91) == 7 && pixel(83, 111) == 7 && pixel(84, 91) != 7 && pixel(60, 90) != 7 &&
              pixel(60, 112) != 7,
          "sprite 0 is 24x21 at (60,91)");
    CHECK(pixel(200, 151) == 8 && pixel(247, 192) == 8 && pixel(248, 151) != 8 && pixel(200, 193) != 8,
          "sprite 1 is 48x42 when expanded");
    CHECK(c64_peek(&m, 0xd01f) == 0x01, "sprite 0 hit the character");
    CHECK(c64_peek(&m, 0xd01f) == 0x00, "reading clears the collision");
    // Behind the character with priority.
    c64_poke(&m, 0xd01b, 0x01);
    c64_run_frame(&m);
    CHECK(pixel(64, 92) == 2 && pixel(60, 92) == 7, "sprite behind the character");
    // Sprites over each other: 0 in front of 1, and they collide.
    c64_poke(&m, 0xd002, 70);
    c64_poke(&m, 0xd003, 95);
    c64_peek(&m, 0xd01e);
    c64_run_frame(&m);
    CHECK(pixel(75, 100) == 7 && c64_peek(&m, 0xd01e) == 0x03, "sprite-sprite");
}

// ---- Disks -----------------------------------------------------------------------

static uint8_t d64[C64_D64_SIZE];

static uint8_t *d64_sector(int track, int sec) {
    int at = 0;
    for (int t = 1; t < track; ++t)
        at += t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17;
    return d64 + (at + sec) * 256;
}

static void d64_name(uint8_t *at, const char *name) {
    memset(at, 0xa0, 16);
    memcpy(at, name, strlen(name));
}

/// A test disk "TEST DISK", ID "01": HELLO, a BASIC program at $0801 that
/// prints HELLO FROM DISK; DATA, 300 bytes at $C000 (two blocks: 0, 1, 2...).
static void make_d64(void) {
    memset(d64, 0, sizeof d64);
    uint8_t *bam = d64_sector(18, 0);
    bam[0] = 18, bam[1] = 1, bam[2] = 0x41;
    for (int t = 1; t <= 35; ++t) {
        int n = t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17;
        bam[4 * t] = (uint8_t)n;
        for (int sec = 0; sec < n; ++sec)
            bam[4 * t + 1 + sec / 8] |= (uint8_t)(1 << (sec % 8));  // free
    }
    bam[4 * 1] -= 3, bam[4 * 1 + 1] &= ~0x07;    // the files' sectors
    bam[4 * 18] -= 2, bam[4 * 18 + 1] &= ~0x03;  // BAM and directory
    d64_name(bam + 0x90, "TEST DISK");
    memcpy(bam + 0xa2, "01" "\xa0" "2A", 5);

    static const uint8_t hello[] = {
        0x01, 0x08, 0x18, 0x08, 0x0a, 0x00, 0x99, '"', 'H', 'E', 'L', 'L', 'O', ' ', 'F', 'R', 'O',
        'M', ' ', 'D', 'I', 'S', 'K', '"', 0x00, 0x00, 0x00,
    };
    uint8_t *s = d64_sector(1, 0);
    s[0] = 0, s[1] = (uint8_t)(sizeof hello + 1);
    memcpy(s + 2, hello, sizeof hello);
    // DATA: 2 address bytes + 300 = 302 bytes: 254 in the first sector, 48 in the second.
    uint8_t data[302] = {0x00, 0xc0};
    for (int i = 0; i < 300; ++i)
        data[2 + i] = (uint8_t)i;
    s = d64_sector(1, 1);
    s[0] = 1, s[1] = 2;
    memcpy(s + 2, data, 254);
    s = d64_sector(1, 2);
    s[0] = 0, s[1] = 48 + 1;
    memcpy(s + 2, data + 254, 48);

    uint8_t *dir = d64_sector(18, 1);
    dir[0] = 0, dir[1] = 0xff;
    dir[2] = 0x82, dir[3] = 1, dir[4] = 0;
    d64_name(dir + 5, "HELLO");
    dir[30] = 1;
    dir[32 + 2] = 0x82, dir[32 + 3] = 1, dir[32 + 4] = 1;
    d64_name(dir + 32 + 5, "DATA");
    dir[32 + 30] = 2;

    // WRECK, at $C000: leaves the machine as a game might, for the test that
    // switching programs starts afresh. BASIC and KERNAL banked out, its own
    // IRQ handler on a raster interrupt, a red border, all sprites on, CIA 1's
    // timer stopped, and 40 columns of 38.
    static const uint8_t wreck[] = {
        0x00, 0xc0,
        0x78,                          // SEI
        0xa9, 0x35, 0x85, 0x01,        // LDA #$35 ; STA $01
        0xa9, 0x2c, 0x8d, 0xfe, 0xff,  // LDA #<irq ; STA $FFFE
        0xa9, 0xc0, 0x8d, 0xff, 0xff,  // LDA #>irq ; STA $FFFF
        0xa9, 0x02, 0x8d, 0x20, 0xd0,  // LDA #2 ; STA $D020
        0xa9, 0xff, 0x8d, 0x15, 0xd0,  // LDA #$FF ; STA $D015
        0xa9, 0x01, 0x8d, 0x1a, 0xd0,  // LDA #1 ; STA $D01A
        0xa9, 0x00, 0x8d, 0x0e, 0xdc,  // LDA #0 ; STA $DC0E
        0xa9, 0xc0, 0x8d, 0x16, 0xd0,  // LDA #$C0 ; STA $D016
        0x58,                          // CLI
        0x4c, 0x29, 0xc0,              // JMP *
        0xee, 0x19, 0xd0, 0x40,        // irq ($C02C): INC $D019 ; RTI
    };
    s = d64_sector(1, 3);
    s[0] = 0, s[1] = (uint8_t)(sizeof wreck + 1);
    memcpy(s + 2, wreck, sizeof wreck);
    dir[64 + 2] = 0x82, dir[64 + 3] = 1, dir[64 + 4] = 3;
    d64_name(dir + 64 + 5, "WRECK");
    dir[64 + 30] = 1;
    bam[4 * 1] -= 1, bam[4 * 1 + 1] &= ~0x08;
}

static uint8_t load_buf[0x10002];

static int test_load(void *user, unsigned device, const uint8_t *name, int len, uint8_t *buf) {
    if (device != 8)
        return C64_LOAD_NO_DEVICE;
    int n = c64_d64_load(d64, sizeof d64, name, len, buf);
    return n < 0 ? C64_LOAD_NOT_FOUND : n;
}

static void test_disk(void) {
    uint8_t name[16];
    d64_name(name, "HELLO");
    CHECK(c64_name_match((const uint8_t *)"HELLO", 5, name), "exact name");
    CHECK(!c64_name_match((const uint8_t *)"HELL", 4, name), "shorter name");
    CHECK(c64_name_match((const uint8_t *)"HE*", 3, name), "pattern *");
    CHECK(c64_name_match((const uint8_t *)"H?LLO", 5, name), "pattern ?");
    CHECK(c64_name_match((const uint8_t *)"0:HELLO", 7, name), "drive 0:");
    CHECK(c64_name_match((const uint8_t *)"*", 1, name), "*");

    make_d64();
    int n = c64_d64_load(d64, sizeof d64, (const uint8_t *)"DATA", 4, load_buf);
    bool ok = n == 302 && load_buf[0] == 0 && load_buf[1] == 0xc0;
    for (int i = 0; ok && i < 300; ++i)
        ok = load_buf[2 + i] == (uint8_t)i;
    CHECK(ok, "DATA across two sectors: %d bytes", n);
    CHECK(c64_d64_load(d64, sizeof d64, (const uint8_t *)"*", 1, load_buf) == 27 && load_buf[1] == 0x08,
          "* loads the first file");
    CHECK(c64_d64_load(d64, sizeof d64, (const uint8_t *)"NONE", 4, load_buf) < 0, "not found");

    n = c64_d64_directory(d64, sizeof d64, load_buf);
    // $0401: header line 0, then "1 HELLO", "2 DATA", "658 BLOCKS FREE."
    const uint8_t *p = load_buf + 2;
    CHECK(load_buf[0] == 0x01 && load_buf[1] == 0x04 && p[2] == 0 && p[3] == 0 && p[4] == 0x12 &&
              !memcmp(p + 5, "\"TEST DISK", 10),
          "directory heading");
    uint16_t next = p[0] | p[1] << 8;
    const uint8_t *l2 = load_buf + 2 + (next - 0x401);
    CHECK(l2[2] == 1 && !memcmp(l2 + 4, "   \"HELLO\"", 10), "directory line for HELLO");
    CHECK(!memcmp(load_buf + n - 2 - 13, "BLOCKS FREE.", 12) && load_buf[n - 1] == 0 && load_buf[n - 2] == 0,
          "BLOCKS FREE");

    // Saving: a 60 KB file comes back the same, a second save of the same
    // name replaces it, a full disk and a full directory are refused.
    static uint8_t big[60002];
    big[0] = 0x00, big[1] = 0x10;
    for (int i = 2; i < (int)sizeof big; ++i)
        big[i] = (uint8_t)(i * 7);
    unsigned free0 = c64_d64_blocks_free(d64, sizeof d64);
    CHECK(c64_d64_save(d64, sizeof d64, (const uint8_t *)"BIG", 3, big, sizeof big) == 0, "save 60 KB");
    CHECK(c64_d64_blocks_free(d64, sizeof d64) == free0 - 237, "237 blocks used: %u",
          free0 - c64_d64_blocks_free(d64, sizeof d64));
    n = c64_d64_load(d64, sizeof d64, (const uint8_t *)"BIG", 3, load_buf);
    CHECK(n == (int)sizeof big && !memcmp(load_buf, big, sizeof big), "60 KB file reads back: %d", n);
    CHECK(c64_d64_save(d64, sizeof d64, (const uint8_t *)"@0:BIG", 6, big, 1000) == 0 &&
              c64_d64_blocks_free(d64, sizeof d64) == free0 - 4,
          "@0: replaces it");
    CHECK(c64_d64_load(d64, sizeof d64, (const uint8_t *)"BIG", 3, load_buf) == 1000, "the new one");
    CHECK(c64_d64_save(d64, sizeof d64, (const uint8_t *)"B*", 2, big, 10) == C64_D64_SAVE_FAILED,
          "no patterns in a saved name");
    CHECK(c64_d64_scratch(d64, sizeof d64, (const uint8_t *)"BIG", 3) == 1 &&
              c64_d64_blocks_free(d64, sizeof d64) == free0,
          "scratch gives the blocks back");
    int saved = 0, r = 0;
    for (; saved < 5; ++saved) {  // 660 blocks free: two of 237 fit, not three
        char nm[8];
        snprintf(nm, sizeof nm, "F%d", saved);
        if ((r = c64_d64_save(d64, sizeof d64, (const uint8_t *)nm, (int)strlen(nm), big, sizeof big)) != 0)
            break;
    }
    CHECK(r == C64_D64_DISK_FULL && saved == 2 && c64_d64_blocks_free(d64, sizeof d64) == 660 - 474,
          "disk full after two 60 KB files: %d saved, %d", saved, r);
    make_d64();
    for (saved = 0; saved < 200; ++saved) {
        char nm[8];
        snprintf(nm, sizeof nm, "N%d", saved);
        if ((r = c64_d64_save(d64, sizeof d64, (const uint8_t *)nm, (int)strlen(nm), big, 10)) != 0)
            break;
    }
    CHECK(r == C64_D64_DIR_FULL && saved == 144 - 3, "the directory holds 144: %d more, %d", saved, r);
    n = c64_d64_directory(d64, sizeof d64, load_buf);
    CHECK(n > 144 * 30, "a full directory lists: %d bytes", n);
    make_d64();

    // The KERNAL LOAD trap: LOAD "DATA",8,1 from code in the made-up KERNAL.
    static const uint8_t code[] = {
        0xa9, 0x08, 0x85, 0xba,        // LDA #8 ; STA $BA (device)
        0xa9, 0x01, 0x85, 0xb9,        // LDA #1 ; STA $B9 (secondary address)
        0xa9, 0x04, 0x85, 0xb7,        // LDA #4 ; STA $B7 (name length)
        0xa9, 0x40, 0x85, 0xbb,        // LDA #$40 ; STA $BB
        0xa9, 0x03, 0x85, 0xbc,        // LDA #$03 ; STA $BC (name at $0340)
        0xa9, 0x00, 0x85, 0x93,        // LDA #0 ; STA $93 (load)
        0x20, 0xa5, 0xf4,              // JSR $F4A5
        0x86, 0x02, 0x84, 0x03,        // STX $02 ; STY $03
        0x08, 0x68, 0x85, 0x04,        // PHP ; PLA ; STA $04
        0x4c, 0x23, 0xe0,              // JMP *
    };
    boot(code, sizeof code, NULL, 0);
    memcpy(ram + 0x340, "DATA", 4);
    m.load = test_load;
    m.load_buf = load_buf;
    c64_run_frame(&m);
    CHECK(ram[0xc000] == 0 && ram[0xc12b] == 43 && ram[2] == 0x2c && ram[3] == 0xc1 && !(ram[4] & C64_C),
          "LOAD trap: end %02X%02X, p %02X", ram[3], ram[2], ram[4]);
    // Not found: carry set, A = 4.
    boot(code, sizeof code, NULL, 0);
    memcpy(ram + 0x340, "XXXX", 4);
    m.load = test_load;
    m.load_buf = load_buf;
    c64_run_frame(&m);
    CHECK((ram[4] & C64_C) && ram[0x90] == 0x42, "LOAD trap: file not found");
}

// ---- SID ---------------------------------------------------------------------------

static void test_sid(void) {
    c64_sid s;
    // Voice 1: sawtooth at 440 Hz, attack 0, sustain 15, full volume.
    c64_sid_init(&s, C64_CPU_HZ, 48000);
    uint16_t f = (uint16_t)(440.0 * 16777216.0 / C64_CPU_HZ + 0.5);
    c64_sid_write(&s, 0, f & 0xff);
    c64_sid_write(&s, 1, f >> 8);
    c64_sid_write(&s, 5, 0x00);
    c64_sid_write(&s, 6, 0xf0);
    c64_sid_write(&s, 0x18, 0x0f);
    c64_sid_write(&s, 4, 0x21);
    int up = 0, prev = 0, peak = 0;
    for (int i = 0; i < 48000; ++i) {
        int v = c64_sid_sample(&s);
        if (i > 4800) {  // after the DC blocker has settled
            if (prev < 0 && v >= 0)
                ++up;
            if (v > peak)
                peak = v;
        }
        prev = v;
    }
    up = up * 10 / 9;  // counted over 0.9 s
    CHECK(up >= 437 && up <= 443, "sawtooth at 440 Hz: %d", up);
    CHECK(peak > 4000 && peak < 20000, "sawtooth level: %d", peak);

    // Attack rate 8: 0 to 255 in about 100 ms (392 cycles a step).
    c64_sid_init(&s, C64_CPU_HZ, 48000);
    c64_sid_write(&s, 5, 0x80);
    c64_sid_write(&s, 6, 0x80);  // sustain 8: $88
    c64_sid_write(&s, 4, 0x11);
    int top = -1;
    for (int i = 0; i < 48000 && top < 0; ++i) {
        c64_sid_sample(&s);
        if (s.v[0].env == 0xff)
            top = i;
    }
    CHECK(top >= 4700 && top <= 4900, "attack 8 takes about 100 ms: %d samples", top);
    for (int i = 0; i < 48000; ++i)
        c64_sid_sample(&s);
    CHECK(s.v[0].env == 0x88, "decays to sustain 8: %02X", s.v[0].env);
    c64_sid_write(&s, 4, 0x10);  // gate off: release (rate 0)
    for (int i = 0; i < 48000; ++i)
        c64_sid_sample(&s);
    CHECK(s.v[0].env == 0, "released: %02X", s.v[0].env);

    // Pulse width: a quarter of the time high.
    c64_sid_init(&s, C64_CPU_HZ, 48000);
    s.v[0].freq = 0x1000;
    c64_sid_write(&s, 2, 0x00);
    c64_sid_write(&s, 3, 0x0c);  // $C00: high from 3/4 of the way
    c64_sid_write(&s, 6, 0xf0);
    c64_sid_write(&s, 4, 0x41);
    c64_sid_write(&s, 0x18, 0x0f);
    int high = 0;
    for (int i = 0; i < 48000; ++i) {
        c64_sid_sample(&s);
        if ((s.v[0].acc >> 12) >= s.v[0].pw)
            ++high;
    }
    CHECK(high > 11000 && high < 13000, "pulse 25%%: %d of 48000", high);

    // The volume's DC step: a write to $D418 alone makes a click (a digi).
    c64_sid_init(&s, C64_CPU_HZ, 48000);
    for (int i = 0; i < 100; ++i)
        c64_sid_sample(&s);
    c64_sid_write(&s, 0x18, 0x0f);
    int click = c64_sid_sample(&s);
    CHECK(click > 3000, "volume step clicks: %d", click);

    // Voice 3's noise read back through the machine: random bytes.
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    boot(loop, sizeof loop, NULL, 0);
    c64_poke(&m, 0xd40e, 0xff);
    c64_poke(&m, 0xd40f, 0xff);
    c64_poke(&m, 0xd412, 0x80);
    bool seen[256] = {false};
    int distinct = 0;
    for (int i = 0; i < 64; ++i) {
        m.cycles += 137;
        uint8_t v = c64_peek(&m, 0xd41b);
        if (!seen[v]) {
            seen[v] = true;
            ++distinct;
        }
    }
    CHECK(distinct > 40, "voice 3 noise reads back random: %d different of 64", distinct);
    // And its envelope: attack 0 reaches 255 within a couple of ms.
    c64_poke(&m, 0xd413, 0x00);
    c64_poke(&m, 0xd414, 0xf0);
    c64_poke(&m, 0xd412, 0x81);
    m.cycles += 3000;
    CHECK(c64_peek(&m, 0xd41c) == 0xff, "voice 3 envelope reads back: %02X", c64_peek(&m, 0xd41c));
}

// ---- Cartridges --------------------------------------------------------------------

static uint8_t crt[0x30000];

/// A .crt image: the header, then a CHIP packet for each bank (filled with
/// bank * 16 + 1 for ROML and + 2 for ROMH unless code is given for bank 0).
static size_t make_crt(int type, int exrom, int game, int banks, uint16_t load, uint16_t size,
                       const uint8_t *code, size_t code_n) {
    memset(crt, 0, sizeof crt);
    memcpy(crt, "C64 CARTRIDGE   ", 16);
    crt[0x13] = 0x40;
    crt[0x14] = 1;
    crt[0x16] = (uint8_t)(type >> 8), crt[0x17] = (uint8_t)type;
    crt[0x18] = (uint8_t)exrom, crt[0x19] = (uint8_t)game;
    memcpy(crt + 0x20, "TEST", 4);
    size_t at = 0x40;
    for (int b = 0; b < banks; ++b) {
        uint8_t *p = crt + at;
        uint32_t len = 16u + size;
        memcpy(p, "CHIP", 4);
        p[4] = (uint8_t)(len >> 24), p[5] = (uint8_t)(len >> 16), p[6] = (uint8_t)(len >> 8), p[7] = (uint8_t)len;
        p[11] = (uint8_t)b;
        p[12] = (uint8_t)(load >> 8), p[13] = (uint8_t)load;
        p[14] = (uint8_t)(size >> 8), p[15] = (uint8_t)size;
        memset(p + 16, b * 16 + 1, size > 0x2000 ? 0x2000 : size);
        if (size > 0x2000)
            memset(p + 16 + 0x2000, b * 16 + 2, size - 0x2000);
        if (!b && code)
            memcpy(p + 16, code, code_n);
        at += len;
    }
    return at;
}

/// A cartridge that starts itself (CBM80) and puts CART OK on the screen.
static size_t make_test_crt(void) {
    static const uint8_t code[] = {
        0x09, 0x80, 0x09, 0x80,              // $8000: cold and warm start: $8009
        0xc3, 0xc2, 0xcd, 0x38, 0x30,        // "CBM80"
        0x78, 0xa2, 0xff, 0x9a, 0xd8,        // $8009: SEI ; LDX #$FF ; TXS ; CLD
        0x20, 0x84, 0xff,                    // JSR $FF84 (IOINIT)
        0x20, 0x87, 0xff,                    // JSR $FF87 (RAMTAS)
        0x20, 0x8a, 0xff,                    // JSR $FF8A (RESTOR)
        0x20, 0x81, 0xff,                    // JSR $FF81 (CINT)
        0xa2, 0x00,                          // LDX #0
        0xbd, 0x2a, 0x80,                    // loop: LDA text,X
        0xf0, 0x06,                          // BEQ done
        0x9d, 0x00, 0x04,                    // STA $0400,X
        0xe8, 0xd0, 0xf5,                    // INX ; BNE loop
        0x4c, 0x27, 0x80,                    // done: JMP done
        0x03, 0x01, 0x12, 0x14, 0x20, 0x0f, 0x0b, 0x00,  // $802A: "CART OK" in screen codes
    };
    return make_crt(0, 0, 1, 1, 0x8000, 0x2000, code, sizeof code);
}

static c64_cart cart;

static void plug(size_t n) {
    int type;
    CHECK(c64_cart_parse(&cart, crt, n, &type) == 0, "parse type %d", type);
    c64_cart_attach(&m, &cart);
    c64_reset(&m);
}

static void test_cart(void) {
    static const uint8_t loop[] = {0x4c, 0x00, 0xe0};
    int type;
    CHECK(c64_cart_parse(&cart, (const uint8_t *)"not a cartridge at all..........................................", 64,
                         &type) == C64_CART_NOT_CRT,
          "not a .crt");
    make_crt(4, 0, 0, 1, 0x8000, 0x4000, NULL, 0);
    CHECK(c64_cart_parse(&cart, crt, 0x4050, &type) == C64_CART_UNSUPPORTED && type == 4, "unsupported type");

    // 8 KB: ROML at $8000 while LORAM and HIRAM are high.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(0, 0, 1, 1, 0x8000, 0x2000, NULL, 0));
    CHECK(c64_peek(&m, 0x8000) == 0x01 && c64_peek(&m, 0xa000) == basic[0], "8 KB at $8000, BASIC at $A000");
    c64_poke(&m, 0, 0x2f);
    c64_poke(&m, 1, 0x36);
    CHECK(c64_peek(&m, 0x8000) == ram[0x8000], "LORAM low: RAM at $8000");

    // 16 KB: ROMH at $A000 instead of BASIC.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(0, 0, 0, 1, 0x8000, 0x4000, NULL, 0));
    CHECK(c64_peek(&m, 0x8000) == 0x01 && c64_peek(&m, 0xa000) == 0x02, "16 KB");

    // Ultimax: ROMH at $E000 instead of the KERNAL, I/O still there.
    boot(loop, sizeof loop, NULL, 0);
    size_t n = make_crt(0, 1, 0, 1, 0xe000, 0x2000, NULL, 0);
    plug(n);
    CHECK(c64_peek(&m, 0xe000) == 0x01, "Ultimax ROMH at $E000");
    c64_poke(&m, 0xd020, 3);
    CHECK(m.vic.reg[0x20] == 3, "Ultimax I/O");

    // Ocean: $DE00 picks the bank; ROMH mirrors it.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(5, 0, 0, 4, 0x8000, 0x2000, NULL, 0));
    c64_poke(&m, 0xde00, 2);
    CHECK(c64_peek(&m, 0x8000) == 0x21 && c64_peek(&m, 0xa000) == 0x21, "Ocean bank 2");
    c64_reset(&m);
    CHECK(c64_peek(&m, 0x8000) == 0x01, "reset: bank 0");

    // System 3: the address written picks the bank.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(15, 0, 1, 4, 0x8000, 0x2000, NULL, 0));
    c64_poke(&m, 0xde03, 0);
    CHECK(c64_peek(&m, 0x8000) == 0x31, "System 3 bank 3");

    // Magic Desk: bank by value, bit 7 switches the cartridge off.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(19, 0, 1, 4, 0x8000, 0x2000, NULL, 0));
    c64_poke(&m, 0xde00, 1);
    CHECK(c64_peek(&m, 0x8000) == 0x11, "Magic Desk bank 1");
    c64_poke(&m, 0xde00, 0x80);
    CHECK(c64_peek(&m, 0x8000) == ram[0x8000], "Magic Desk off");

    // EasyFlash: boots Ultimax; $DE02 = 7 gives 16 KB mode; RAM at $DF00.
    boot(loop, sizeof loop, NULL, 0);
    plug(make_crt(32, 1, 0, 2, 0x8000, 0x4000, NULL, 0));
    CHECK(c64_peek(&m, 0xe000) == 0x02, "EasyFlash boots Ultimax: %02X", c64_peek(&m, 0xe000));
    c64_poke(&m, 0xde02, 0x07);
    c64_poke(&m, 0xde00, 1);
    CHECK(c64_peek(&m, 0x8000) == 0x11 && c64_peek(&m, 0xa000) == 0x12 && c64_peek(&m, 0xe000) == 0x4c,
          "EasyFlash 16 KB mode, bank 1");
    c64_poke(&m, 0xdf42, 0x5a);
    CHECK(c64_peek(&m, 0xdf42) == 0x5a, "EasyFlash RAM");
    c64_cart_attach(&m, NULL);
}

// ---- The 1541's parts ----------------------------------------------------------------

static void test_via(void) {
    c64_via v = {0};
    c64_via_reset(&v);
    // Timer 1 free-running with latch 98: an interrupt every 100 cycles.
    c64_via_write(&v, 0x0b, 0x40);
    c64_via_write(&v, 0x0e, 0xc0);
    c64_via_write(&v, 0x04, 98);
    c64_via_write(&v, 0x05, 0);
    c64_via_tick(&v, 98);
    CHECK(!v.irq, "T1 not yet");
    c64_via_tick(&v, 1);
    CHECK(v.irq && (c64_via_read(&v, 0x0d) & 0xc0) == 0xc0, "T1 interrupts");
    c64_via_read(&v, 0x04);
    CHECK(!v.irq, "reading T1 low clears it");
    c64_via_tick(&v, 100);
    CHECK(v.irq, "and again 100 cycles on");
    // One-shot timer 2: once only.
    c64_via_reset(&v);
    c64_via_write(&v, 0x0e, 0xa0);
    c64_via_write(&v, 0x08, 10);
    c64_via_write(&v, 0x09, 0);
    c64_via_tick(&v, 11);
    CHECK(v.irq, "T2 one-shot");
    c64_via_write(&v, 0x0d, 0x20);
    c64_via_tick(&v, 70000);
    CHECK(!v.irq, "T2 only once");
    // CA1: the PCR picks the edge.
    c64_via_reset(&v);
    c64_via_write(&v, 0x0c, 0x01);
    c64_via_write(&v, 0x0e, 0x82);
    c64_via_set_ca1(&v, true);
    CHECK(v.irq, "CA1 rising edge");
    c64_via_read(&v, 0x01);
    c64_via_set_ca1(&v, false);
    CHECK(!v.irq, "not on the falling one");
    // Manual CA2/CB2 outputs.
    c64_via_write(&v, 0x0c, 0xec);
    CHECK(!c64_via_ca2(&v) && c64_via_cb2(&v), "CA2 low, CB2 high");
}

static void test_gcr(void) {
    // The test disk made into GCR tracks and read back gives the same disk.
    make_d64();
    static uint8_t copy[C64_D64_SIZE], dram[0x800], drom[0x4000], gcr[C64_DRIVE_TRACKS * C64_GCR_TRACK_MAX];
    static c64_drive d;
    static c64_cia cia;
    memcpy(copy, d64, sizeof d64);
    memset(&d, 0, sizeof d);
    d.ram = dram, d.rom = drom, d.gcr = gcr;
    drom[0x3ffc] = 0x00, drom[0x3ffd] = 0xc0;  // reset to $C000: JMP *
    drom[0] = 0x4c, drom[1] = 0x00, drom[2] = 0xc0;
    c64_cia_reset(&cia);
    c64_drive_init(&d, &cia);
    c64_drive_insert(&d, d64, sizeof d64, false);
    CHECK(d.track_len[0] == 7692 && d.track_len[17] == 7142 && d.track_len[34] == 6250, "track lengths");
    // Every track as if written, then read back.
    memset(d64, 0, sizeof d64);
    for (int t = 0; t < C64_DRIVE_TRACKS; ++t)
        d.track_dirty[t] = true;
    CHECK(c64_drive_flush(&d), "flushed");
    CHECK(!memcmp(d64, copy, sizeof d64), "GCR there and back");
    // The first bytes of track 18: a sync, then the header of sector 0.
    const uint8_t *t18 = gcr + 17 * C64_GCR_TRACK_MAX;
    CHECK(t18[0] == 0xff && t18[4] == 0xff && t18[5] == 0x52, "sync and header ID 8 in GCR: %02X", t18[5]);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--mkcrt")) {
        size_t n = make_test_crt();
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(crt, 1, n, f) != n || fclose(f)) {
            perror(argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--mkd64")) {
        make_d64();
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(d64, 1, sizeof d64, f) != sizeof d64 || fclose(f)) {
            perror(argv[2]);
            return 1;
        }
        return 0;
    }
    test_against_65c02();
    test_decimal();
    test_undocumented();
    test_interrupts();
    test_cia();
    test_banking();
    test_keyboard();
    test_video();
    test_raster_irq();
    test_midline();
    test_sprites();
    test_disk();
    test_sid();
    test_cart();
    test_via();
    test_gcr();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
