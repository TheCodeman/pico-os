/**
 * @file fpfuzz.c
 * @brief Differential test of as_float.c against the floating-point routines of a real Applesoft ROM, run on the 65C02 core.
 *
 *   fpfuzz ROM16K [iterations] [seed]
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_cpu.h"
#include "as_float.h"

static uint8_t ram[0x10000];
static uint8_t rom[0x4000];

static uint8_t rd(void *ctx, uint16_t a) {
    if (a < 0xc000)
        return ram[a];
    if (a < 0xc100)
        return 0;
    return rom[a - 0xc000];
}

static void wr(void *ctx, uint16_t a, uint8_t v) {
    if (a < 0xc000)
        ram[a] = v;
}

static as_cpu last;  ///< registers when the routine returned

/// Runs the ROM routine at addr; returns 0, or the Applesoft error code.
static int call_rom(uint16_t addr, uint8_t a, uint8_t x, uint8_t y) {
    ram[0x300] = 0x20;  // JSR addr
    ram[0x301] = addr & 0xff;
    ram[0x302] = addr >> 8;
    as_cpu c = {.read = rd, .write = wr};
    c.pc = 0x300;
    c.s = 0xff;
    c.p = AS_U;
    c.a = a;
    c.x = x;
    c.y = y;
    for (long n = 0; n < 20000000; ++n) {
        if (c.pc == 0x303) {
            last = c;
            return 0;
        }
        if (c.pc == 0xd412) {
            last = c;
            return c.x;
        }
        as_cpu_step(&c);
    }
    fprintf(stderr, "ROM routine $%04X ran away\n", addr);
    exit(1);
}

static jmp_buf err_jmp;
static int err_code;
static void on_error(int code) {
    err_code = code;
    longjmp(err_jmp, 1);
}

static void fac_to_ram(const as_fp *f) {
    ram[0x9d] = f->exp;
    ram[0x9e] = f->mant >> 24;
    ram[0x9f] = f->mant >> 16;
    ram[0xa0] = f->mant >> 8;
    ram[0xa1] = f->mant;
    ram[0xa2] = f->neg ? 0xff : 0x00;
    ram[0xac] = f->ext;
}

static as_fp fac_from_ram(void) {
    as_fp f;
    f.exp = ram[0x9d];
    f.mant = (uint32_t)ram[0x9e] << 24 | ram[0x9f] << 16 | ram[0xa0] << 8 | ram[0xa1];
    f.neg = ram[0xa2] & 0x80;
    f.ext = ram[0xac];
    return f;
}

static void arg_to_ram(const as_fp *a) {
    ram[0xa5] = a->exp;
    ram[0xa6] = a->mant >> 24;
    ram[0xa7] = a->mant >> 16;
    ram[0xa8] = a->mant >> 8;
    ram[0xa9] = a->mant;
    ram[0xaa] = a->neg ? 0xff : 0x00;
    ram[0xab] = ram[0xaa] ^ ram[0xa2];
}

static uint64_t rng_state = 88172645463325252ull;
static uint32_t rnd32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static as_fp random_fp(bool with_ext) {
    as_fp f;
    uint32_t r = rnd32();
    switch (r % 8) {
    case 0: f.exp = rnd32() & 0xff; break;
    case 1: f.exp = 0; break;
    default: f.exp = 0x70 + rnd32() % 0x30; break;
    }
    switch (rnd32() % 6) {
    case 0: f.mant = 0x80000000u; break;
    case 1: f.mant = 0xffffffffu; break;
    case 2: f.mant = 0x80000000u | (rnd32() & 0xff000000u); break;
    default: f.mant = 0x80000000u | rnd32(); break;
    }
    f.neg = rnd32() & 1;
    f.ext = with_ext ? ((rnd32() & 3) ? rnd32() & 0xff : 0) : 0;
    if (!f.exp)
        f.neg = false;
    return f;
}

static void random_packed(uint8_t *m) {
    as_fp f = random_fp(false);
    m[0] = f.exp;
    m[1] = (uint8_t)((f.mant >> 24) & (f.neg ? 0xff : 0x7f));
    m[2] = f.mant >> 16;
    m[3] = f.mant >> 8;
    m[4] = f.mant;
}

static bool same(const as_fp *a, const as_fp *b, bool check_ext) {
    if (a->exp != b->exp)
        return false;
    if (!a->exp)
        return true;
    return a->mant == b->mant && a->neg == b->neg && (!check_ext || a->ext == b->ext);
}

static void show(const char *what, const as_fp *f) {
    printf("  %s exp=%02X mant=%08X ext=%02X %s\n", what, f->exp, f->mant, f->ext,
           f->neg ? "neg" : "pos");
}

static long failures;

typedef struct { const char *s; int pos; } fin_text;
static int fin_peek(fin_text *t) {
    return (uint8_t)t->s[t->pos];
}
static void fin_next(fin_text *t) {
    if (t->s[t->pos])
        t->pos++;
    while (t->s[t->pos] == ' ')
        t->pos++;
}

static void report(const char *op, const as_fp *in, const uint8_t *m, const as_fp *rom_r,
                   int rom_err, const as_fp *my_r, int my_err) {
    if (++failures > 40)
        return;
    printf("%s mismatch\n", op);
    show("FAC in", in);
    if (m)
        printf("  mem    %02X %02X %02X %02X %02X\n", m[0], m[1], m[2], m[3], m[4]);
    show("ROM   ", rom_r);
    show("ours  ", my_r);
    printf("  errors: ROM %d ours %d\n", rom_err, my_err);
}

/// Runs one operation both ways and compares.
typedef void (*my_op)(as_fp *f, const uint8_t *m);

static void check(const char *name, uint16_t addr, my_op mine, as_fp in, const uint8_t *m,
                  bool check_ext) {
    memcpy(&ram[0x400], m ? m : (const uint8_t *)"\0\0\0\0\0", 5);
    fac_to_ram(&in);
    int rom_err = call_rom(addr, 0x00, 0x00, 0x04);
    as_fp rom_r = fac_from_ram();
    as_fp my_r = in;
    int my_err = 0;
    if (!setjmp(err_jmp))
        mine(&my_r, m);
    else
        my_err = err_code;
    if (rom_err != my_err || (!rom_err && !same(&rom_r, &my_r, check_ext)))
        report(name, &in, m, &rom_r, rom_err, &my_r, my_err);
}

static void my_add(as_fp *f, const uint8_t *m) { as_fp a = fp_unpack(m); fp_add(f, &a); }
static void my_sub(as_fp *f, const uint8_t *m) { as_fp a = fp_unpack(m); fp_sub(f, &a); }
static void my_mul(as_fp *f, const uint8_t *m) { as_fp a = fp_unpack(m); fp_mul(f, &a); }
static void my_div(as_fp *f, const uint8_t *m) { as_fp a = fp_unpack(m); fp_div(f, &a); }
static void my_int(as_fp *f, const uint8_t *m) { fp_int(f); }
static void my_mul10(as_fp *f, const uint8_t *m) { fp_mul10(f); }
static void my_div10(as_fp *f, const uint8_t *m) { fp_div10(f); }
static void my_sqr(as_fp *f, const uint8_t *m) { fp_sqr(f); }
static void my_log(as_fp *f, const uint8_t *m) { fp_log(f); }
static void my_exp(as_fp *f, const uint8_t *m) { fp_exp(f); }
static void my_sin(as_fp *f, const uint8_t *m) { fp_sin(f); }
static void my_cos(as_fp *f, const uint8_t *m) { fp_cos(f); }
static void my_tan(as_fp *f, const uint8_t *m) { fp_tan(f); }
static void my_atn(as_fp *f, const uint8_t *m) { fp_atn(f); }

/// A value in a range the functions accept, mostly.
static as_fp moderate_fp(void) {
    as_fp f = random_fp(rnd32() & 1);
    if (f.exp && (rnd32() & 3))
        f.exp = 0x78 + rnd32() % 0x14;
    return f;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fpfuzz ROM16K [iterations] [seed]\n");
        return 2;
    }
    FILE *fr = fopen(argv[1], "rb");
    if (!fr || fread(rom, 1, sizeof rom, fr) != sizeof rom)
        return 1;
    fclose(fr);
    long iters = argc > 2 ? atol(argv[2]) : 20000;
    if (argc > 3)
        rng_state ^= strtoull(argv[3], NULL, 0) * 0x9e3779b97f4a7c15ull;
    as_fp_error = on_error;
    // CHRGET and the RND seed, as the ROM's cold start puts them in zero page.
    memcpy(&ram[0xb1], &rom[0xf10b - 0xc000], 0x1c);

    for (long i = 0; i < iters; ++i) {
        uint8_t m[5];
        random_packed(m);
        // ARG shares the exponent range often, to exercise alignment.
        if (rnd32() & 1) {
            as_fp f = random_fp(true);
            if (f.exp && m[0] && (rnd32() & 1))
                m[0] = (uint8_t)(f.exp + (int)(rnd32() % 11) - 5);
            check("FADD", 0xe7be, my_add, f, m, true);
            check("FSUB", 0xe7a7, my_sub, f, m, true);
        }
        as_fp f = random_fp(true);
        check("FMUL", 0xe97f, my_mul, f, m, true);
        f = random_fp(true);
        check("FDIV", 0xea66, my_div, f, m, true);
        f = random_fp(true);
        check("INT", 0xec23, my_int, f, NULL, true);
        f = random_fp(true);
        check("MUL10", 0xea39, my_mul10, f, NULL, true);
        check("DIV10", 0xea55, my_div10, f, NULL, true);

        // Pack (rounding) and compare.
        f = random_fp(true);
        fac_to_ram(&f);
        int rom_err = call_rom(0xeb2b, 0, 0x00, 0x04);
        as_fp mine = f;
        uint8_t mm[5];
        volatile int my_err = 0;
        if (!setjmp(err_jmp))
            fp_pack(&mine, mm);
        else
            my_err = err_code;
        if (rom_err != my_err || (!rom_err && memcmp(mm, &ram[0x400], 5)))
            report("PACK", &f, &ram[0x400], &f, rom_err, &mine, my_err);

        f = random_fp(true);
        random_packed(m);
        if (rnd32() & 1) {
            m[0] = f.exp;
            m[1] = (uint8_t)((f.mant >> 24) & (f.neg ? 0xff : 0x7f));
            if (rnd32() & 1) {
                m[2] = f.mant >> 16;
                m[3] = f.mant >> 8;
                m[4] = (uint8_t)(f.mant + (int)(rnd32() % 3) - 1);
            }
        }
        memcpy(&ram[0x400], m, 5);
        fac_to_ram(&f);
        call_rom(0xebb2, 0x00, 0, 0x04);
        int rom_c = (int8_t)last.a, my_c = fp_compare(&f, m);
        if (rom_c != my_c && ++failures <= 40) {
            printf("FCOMP mismatch: ROM %d ours %d\n", rom_c, my_c);
            show("FAC in", &f);
            printf("  mem    %02X %02X %02X %02X %02X\n", m[0], m[1], m[2], m[3], m[4]);
        }

        // Text to number.
        {
            static const char alphabet[] = "0123456789012345678901234567.E-+ ";
            char text[24];
            int n = 1 + rnd32() % 14;
            for (int k = 0; k < n; ++k)
                text[k] = alphabet[rnd32() % (sizeof alphabet - 1)];
            text[n] = 0;
            memcpy(&ram[0x500], text, n + 1);
            ram[0xb8] = 0x00;
            ram[0xb9] = 0x05;
            // CHRGOT, then FIN
            ram[0x310] = 0x20; ram[0x311] = 0xb7; ram[0x312] = 0x00;
            ram[0x313] = 0x4c; ram[0x314] = 0x4a; ram[0x315] = 0xec;
            rom_err = call_rom(0x310, 0, 0, 0);
            as_fp rom_r = fac_from_ram();
            int rom_end = ram[0xb8] | ram[0xb9] << 8;
            fin_text src = {text, 0};
            while (text[src.pos] == ' ')
                src.pos++;
            as_fin_src fs = {
                .peek = (int (*)(void *))fin_peek,
                .next = (void (*)(void *))fin_next,
                .ctx = &src,
            };
            my_err = 0;
            static as_fp my_r;
            my_r = (as_fp){0};
            if (!setjmp(err_jmp))
                my_r = fp_parse(&fs);
            else
                my_err = err_code;
            if (rom_err != my_err ||
                (!rom_err && (!same(&rom_r, &my_r, false) || rom_end - 0x500 != src.pos))) {
                if (++failures <= 40) {
                    printf("FIN mismatch on \"%s\": end ROM %d ours %d, errors %d %d\n", text,
                           rom_end - 0x500, src.pos, rom_err, my_err);
                    show("ROM ", &rom_r);
                    show("ours", &my_r);
                }
            }
        }

        // RND, from the seed
        {
            as_fp a = random_fp(true);
            if (rnd32() & 1)
                a.neg = false;
            uint8_t seed[5];
            random_packed(seed);
            memcpy(&ram[0xc9], seed, 5);
            fac_to_ram(&a);
            rom_err = call_rom(0xefae, 0, 0, 0);
            as_fp rom_r = fac_from_ram();
            static as_fp my_r;
            my_r = a;
            my_err = 0;
            if (!setjmp(err_jmp))
                fp_rnd(&my_r, seed);
            else
                my_err = err_code;
            if (rom_err != my_err ||
                (!rom_err && (!same(&rom_r, &my_r, false) || memcmp(seed, &ram[0xc9], 5)))) {
                if (++failures <= 40) {
                    printf("RND mismatch\n");
                    show("in  ", &a);
                    show("ROM ", &rom_r);
                    show("ours", &my_r);
                }
            }
        }

        // ARG ^ FAC
        {
            as_fp a = moderate_fp(), x = moderate_fp();
            a.ext = 0;
            if (rnd32() & 1) {  // small integer powers, often of negatives
                x = fp_from_int16((int)(rnd32() % 9) - 4);
            }
            fac_to_ram(&x);
            arg_to_ram(&a);
            ram[0x320] = 0xa5; ram[0x321] = 0x9d;  // LDA $9D
            ram[0x322] = 0x4c; ram[0x323] = 0x97; ram[0x324] = 0xee;
            rom_err = call_rom(0x320, 0, 0, 0);
            as_fp rom_r = fac_from_ram(), my_r = x;
            my_err = 0;
            if (!setjmp(err_jmp))
                fp_pow(&my_r, &a);
            else
                my_err = err_code;
            if (rom_err != my_err || (!rom_err && !same(&rom_r, &my_r, true))) {
                report("POW", &x, NULL, &rom_r, rom_err, &my_r, my_err);
                show("ARG   ", &a);
            }
        }

        // To a 16-bit integer (AYINT) and to an address (GETADR)
        {
            as_fp a = random_fp(true);
            if (rnd32() & 1)
                a.exp = 0x80 + rnd32() % 0x14;
            fac_to_ram(&a);
            rom_err = call_rom(0xe10c, 0, 0, 0);
            volatile int rom_v = (int16_t)(ram[0xa0] << 8 | ram[0xa1]);
            as_fp b = a;
            my_err = 0;
            volatile int my_v = 0;
            if (!setjmp(err_jmp))
                my_v = fp_to_int16(&b);
            else
                my_err = err_code;
            if (rom_err != my_err || (!rom_err && rom_v != my_v)) {
                if (++failures <= 40) {
                    printf("AYINT mismatch: ROM %d (%d) ours %d (%d)\n", rom_v, rom_err, my_v,
                           my_err);
                    show("in", &a);
                }
            }
            fac_to_ram(&a);
            rom_err = call_rom(0xe752, 0, 0, 0);
            rom_v = ram[0x50] | ram[0x51] << 8;
            b = a;
            my_err = 0;
            my_v = 0;
            if (!setjmp(err_jmp))
                my_v = fp_to_addr(&b);
            else
                my_err = err_code;
            if (rom_err != my_err || (!rom_err && rom_v != my_v)) {
                if (++failures <= 40) {
                    printf("GETADR mismatch: ROM %d (%d) ours %d (%d)\n", rom_v, rom_err, my_v,
                           my_err);
                    show("in", &a);
                }
            }
            int16_t v = (int16_t)rnd32();
            call_rom(0xe2f2, (uint8_t)(v >> 8), 0, (uint8_t)v);
            as_fp rr = fac_from_ram(), mr = fp_from_int16(v);
            if (!same(&rr, &mr, true) && ++failures <= 40)
                printf("GIVAYF mismatch for %d\n", v);
        }

        // Number to text.
        f = random_fp(rnd32() & 1);
        fac_to_ram(&f);
        rom_err = call_rom(0xed34, 0, 0, 0);
        char my_text[24];
        as_fp g = f;
        my_err = 0;
        if (!setjmp(err_jmp))
            fp_format(&g, my_text);
        else
            my_err = err_code;
        if (rom_err != my_err || (!rom_err && strcmp(my_text, (char *)&ram[0x100]))) {
            if (++failures <= 40) {
                printf("FOUT mismatch: ROM \"%s\" ours \"%s\" (errors %d %d)\n",
                       rom_err ? "" : (char *)&ram[0x100], my_text, rom_err, my_err);
                show("FAC in", &f);
            }
        }

        f = moderate_fp();
        check("SQR", 0xee8d, my_sqr, f, NULL, true);
        f = moderate_fp();
        check("LOG", 0xe941, my_log, f, NULL, true);
        f = moderate_fp();
        check("EXP", 0xef09, my_exp, f, NULL, true);
        f = moderate_fp();
        check("SIN", 0xeff1, my_sin, f, NULL, true);
        f = moderate_fp();
        check("COS", 0xefea, my_cos, f, NULL, true);
        f = moderate_fp();
        check("TAN", 0xf03a, my_tan, f, NULL, true);
        f = moderate_fp();
        check("ATN", 0xf09e, my_atn, f, NULL, true);
    }
    printf("%ld iterations, %ld failures\n", iters, failures);
    return failures != 0;
}
