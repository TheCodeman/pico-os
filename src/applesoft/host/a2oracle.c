/**
 * @file a2oracle.c
 * @brief Test oracle.
 *
 * runs a real enhanced Apple IIe ROM (which the project does not ship; give
 * its path) on the 65C02 core, types the input file at the keyboard, and
 * writes everything the ROM prints through COUT to stdout. The Applesoft
 * tests compare our interpreter's transcript with this one.
 *
 *   a2oracle ROM16K input.txt [-c max_cycles] [-d start-end outfile]...
 *
 * ROM16K is the $C000-$FFFF image (16 KB). Memory dumps (-d) are taken when
 * the run ends, e.g. -d 2000-3FFF hgr.bin.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_cpu.h"

static uint8_t ram[0xc000];
static uint8_t rom[0x4000];
static uint8_t key_latch;  ///< bit 7 set while a key is waiting
static const char *input;
static size_t input_len, input_pos;
static int idle_reads;     ///< keyboard reads in a row that found nothing

static uint8_t mem_read(void *ctx, uint16_t a) {
    (void)ctx;
    if (a < 0xc000)
        return ram[a];
    if (a < 0xc100) {
        switch (a) {
        case 0xc000:
            if (!(key_latch & 0x80) && input_pos < input_len) {
                char ch = input[input_pos++];
                if (ch == '\n')
                    ch = '\r';
                key_latch = (uint8_t)(ch | 0x80);
            }
            if (!(key_latch & 0x80))
                idle_reads++;
            else
                idle_reads = 0;
            return key_latch;
        case 0xc010:
            key_latch &= 0x7f;
            return 0;
        default:
            return 0;
        }
    }
    return rom[a - 0xc000];
}

static void mem_write(void *ctx, uint16_t a, uint8_t v) {
    (void)ctx;
    if (a < 0xc000)
        ram[a] = v;
    else if (a == 0xc010)
        key_latch &= 0x7f;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: a2oracle ROM16K input.txt [-c cycles] [-d start-end file]...\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) {
        fprintf(stderr, "can't read a 16 KB ROM from %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    static char inbuf[1 << 20];
    input_len = fread(inbuf, 1, sizeof inbuf, f);
    input = inbuf;
    fclose(f);

    unsigned long long max_cycles = 2000000000ull;
    struct { unsigned start, end; const char *file; } dumps[8];
    int ndumps = 0;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            max_cycles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d") && i + 2 < argc && ndumps < 8) {
            if (sscanf(argv[i + 1], "%x-%x", &dumps[ndumps].start, &dumps[ndumps].end) != 2)
                return 2;
            dumps[ndumps++].file = argv[i + 2];
            i += 2;
        }
    }

    as_cpu cpu = {.read = mem_read, .write = mem_write};
    as_cpu_reset(&cpu);
    unsigned long long total = 0;
    bool started = false;  // input is held back until the first prompt
    size_t saved_len = input_len;
    input_len = 0;
    while (total < max_cycles) {
        if (cpu.pc == 0xfdf0) {  // COUT1: a character going to the screen
            uint8_t ch = cpu.a & 0x7f;
            if (started)
                putchar(ch == '\r' ? '\n' : ch);
        }
        if (cpu.pc == 0xfd1b && !started) {  // KEYIN: first time at the prompt
            started = true;
            input_len = saved_len;
        }
        total += as_cpu_step(&cpu);
        if (started && input_pos >= input_len && idle_reads > 1000)
            break;
    }
    if (total >= max_cycles)
        printf("\n[oracle: cycle limit]\n");
    for (int i = 0; i < ndumps; ++i) {
        FILE *o = fopen(dumps[i].file, "wb");
        if (!o)
            return 1;
        for (unsigned a = dumps[i].start; a <= dumps[i].end; ++a)
            fputc(mem_read(NULL, (uint16_t)a), o);
        fclose(o);
    }
    return 0;
}
