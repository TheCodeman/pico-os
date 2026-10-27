/**
 * @file genesis_test.c
 * @brief Tests for the Genesis core with a ROM made here (no game ROMs needed).
 *
 * a hand-assembled 68000 program that sets up the VDP (H40, a red backdrop,
 * a green tile top left and a blue one bottom right), writes a marker into
 * RAM, loads a small Z80 program that writes to the YM2612 and counts in Z80
 * RAM, and counts VBlank interrupts. The test runs it and checks each part.
 *
 *   genesis_test [--mkrom out.bin]   (also writes the ROM, for the harness)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"

extern unsigned char *M68K_RAM;
extern unsigned char *ZRAM;
extern unsigned char gwenesis_vdp_regs[0x20];

#define ROM_SIZE 0x10000
static uint8_t image[ROM_SIZE];
static unsigned pc;

static void w16(unsigned v) {
    image[pc++] = v >> 8;
    image[pc++] = v;
}

static void w32(uint32_t v) {
    w16(v >> 16);
    w16(v & 0xffff);
}

static void put32(unsigned at, uint32_t v) {
    image[at] = v >> 24;
    image[at + 1] = v >> 16;
    image[at + 2] = v >> 8;
    image[at + 3] = v;
}

static void text(unsigned at, const char *s, int len) {
    memset(image + at, ' ', len);
    memcpy(image + at, s, strlen(s));
}

#define VDP_REG(r, v) w16(0x30bc), w16(0x8000 | (r) << 8 | (v))  ///< move.w #$8rvv,(a0)
#define CTRL(v)       w16(0x20bc), w32(v)                         ///< move.l #v,(a0)
#define DATA16(v)     w16(0x32bc), w16(v)                         ///< move.w #v,(a1)

/// A VDP command to write VRAM at addr.
static uint32_t vram_write(unsigned addr) {
    return 0x40000000u | (addr & 0x3fff) << 16 | addr >> 14;
}

static void fill_tile(unsigned addr, uint32_t pattern) {
    CTRL(vram_write(addr));
    w16(0x7007);                  // moveq #7,d0
    w16(0x22bc), w32(pattern);    // move.l #pattern,(a1)
    w16(0x51c8), w16(0xfff8);     // dbra d0,(the move)
}

static const uint8_t z80_prog[] = {
    0x3e, 0x22, 0x32, 0x00, 0x40,  // ld a,$22 : ld ($4000),a   YM2612 register $22 (LFO)
    0x3e, 0x08, 0x32, 0x01, 0x40,  // ld a,$08 : ld ($4001),a   on
    0x2a, 0x00, 0x10,              // loop: ld hl,($1000)
    0x23,                          // inc hl
    0x22, 0x00, 0x10,              // ld ($1000),hl
    0x18, 0xf7,                    // jr loop
};

static void make_rom(void) {
    memset(image, 0, sizeof image);
    // Vectors: the stack, the start, everything else to a stop, but VBlank
    // (level 6 autovector, 30).
    put32(0, 0x00fffe00);
    put32(4, 0x200);
    for (int v = 2; v < 64; v++)
        put32(v * 4, v == 30 ? 0x1000 : 0x1010);
    text(0x100, "SEGA MEGA DRIVE", 16);
    text(0x120, "PICO-OS GENESIS TEST", 48);
    text(0x150, "PICO-OS GENESIS TEST", 48);
    text(0x1f0, "JUE", 3);

    pc = 0x200;
    w16(0x41f9), w32(0xc00004);  // lea $c00004,a0   VDP control
    w16(0x43f9), w32(0xc00000);  // lea $c00000,a1   VDP data
    VDP_REG(0, 0x04);
    VDP_REG(1, 0x74);   // display, VBlank interrupt, DMA, mode 5, 28 rows
    VDP_REG(2, 0x30);   // plane A at $c000
    VDP_REG(4, 0x07);   // plane B at $e000
    VDP_REG(5, 0x7c);   // sprites at $f800
    VDP_REG(7, 0x00);   // backdrop: palette 0, colour 0
    VDP_REG(12, 0x81);  // H40
    VDP_REG(13, 0x3f);  // horizontal scroll at $fc00
    VDP_REG(15, 0x02);  // auto-increment 2
    VDP_REG(16, 0x01);  // planes 64x32
    CTRL(0xc0000000);   // CRAM 0: red, green, blue
    DATA16(0x000e);
    DATA16(0x00e0);
    DATA16(0x0e00);
    fill_tile(0x20, 0x11111111);  // tile 1: colour 1
    fill_tile(0x40, 0x22222222);  // tile 2: colour 2
    CTRL(vram_write(0xc000));     // plane A (0, 0): tile 1
    DATA16(1);
    CTRL(vram_write(0xc000 + (27 * 64 + 39) * 2));  // (39, 27): tile 2
    DATA16(2);
    w16(0x23fc), w32(0x12345678), w32(0xff0000);  // move.l #$12345678,$ff0000

    // The Z80: take its bus, hold it in reset, copy its program, let it go.
    w16(0x33fc), w16(0x100), w32(0xa11100);  // move.w #$100,$a11100   bus request
    w16(0x33fc), w16(0x000), w32(0xa11200);  // move.w #0,$a11200      reset on
    for (unsigned i = 0; i < sizeof z80_prog; i++)
        w16(0x13fc), w16(z80_prog[i]), w32(0xa00000 + i);  // move.b #b,$a0000i
    w16(0x33fc), w16(0x100), w32(0xa11200);  // reset off
    w16(0x33fc), w16(0x000), w32(0xa11100);  // bus back
    w16(0x46fc), w16(0x2000);                // move #$2000,sr   interrupts on
    w16(0x60fe);                             // bra.s *

    if (pc > 0x1000) {
        fprintf(stderr, "the test program is too long\n");
        exit(1);
    }
    pc = 0x1000;  // VBlank
    w16(0x52b9), w32(0xff0004);  // addq.l #1,$ff0004
    w16(0x4e73);                 // rte
    pc = 0x1010;                 // anything else
    w16(0x60fe);
}

static int failures;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/// 68000 RAM keeps each word's bytes swapped.
static uint32_t ram32(unsigned addr) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | M68K_RAM[((addr + i) ^ 1) & 0xffff];
    return v;
}

static uint8_t frame[240][320];

static void put_line(void *ctx, int line, const uint8_t *pix, const uint16_t *palette) {
    memcpy(frame[line], pix, 320);
}

/// The colour at (x, y): 'r', 'g', 'b' if it is purely that, else '?'.
static char colour_at(int x, int y) {
    uint16_t c = genesis_palette()[frame[y][x]];
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    if (r && !g && !b)
        return 'r';
    if (!r && g && !b)
        return 'g';
    if (!r && !g && b)
        return 'b';
    return '?';
}

int main(int argc, char **argv) {
    make_rom();
    if (argc == 3 && !strcmp(argv[1], "--mkrom")) {
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(image, sizeof image, 1, f) != 1 || fclose(f)) {
            perror(argv[2]);
            return 1;
        }
        return 0;
    }

    uint8_t *rom = calloc(1, GENESIS_ROM_MAX);
    memcpy(rom, image, sizeof image);
    genesis_init(malloc(GENESIS_MEM_SIZE), malloc(GENESIS_YM_FAST_SIZE), malloc(GENESIS_YM_SLOW_SIZE));
    const char *name = genesis_load(rom, sizeof image);
    check(!strcmp(name, "PICO-OS GENESIS TEST"), "the header's name");

    const int frames = 60;
    int samples = 0;
    for (int n = 0; n < frames; n++) {
        genesis_run_frame(put_line, NULL);
        const int16_t *ym, *sn;
        samples += genesis_audio(&ym, &sn);
    }

    check(ram32(0xff0000) == 0x12345678, "the 68000 ran: its marker is in RAM");
    uint32_t vblanks = ram32(0xff0004);
    printf("     %u VBlank interrupts in %d frames\n", (unsigned)vblanks, frames);
    // One may be waiting already when interrupts go on.
    check(vblanks >= frames - 2 && vblanks <= frames + 1, "a VBlank interrupt each frame");
    check(gwenesis_vdp_regs[1] == 0x74 && gwenesis_vdp_regs[12] == 0x81, "the VDP registers were set");
    check(genesis_width() == 320 && genesis_height() == 224 && !genesis_pal(), "H40, 224 lines, NTSC");
    unsigned z80_count = ZRAM[0x1000] | ZRAM[0x1001] << 8;
    printf("     the Z80's counter is at %u\n", z80_count);
    check(z80_count > 1000, "the Z80 ran its program");
    printf("     %d sound samples (%.0f a second)\n", samples, samples * 60.0 / frames);
    check(samples > 880 * frames && samples < 900 * frames, "sound samples made at about 53 kHz");
    check(colour_at(0, 0) == 'g' && colour_at(7, 7) == 'g', "the green tile top left");
    check(colour_at(8, 0) == 'r' && colour_at(160, 112) == 'r' && colour_at(311, 223) == 'r',
          "the red backdrop");
    check(colour_at(312, 216) == 'b' && colour_at(319, 223) == 'b', "the blue tile bottom right");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
