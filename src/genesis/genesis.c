/**
 * @file genesis.c
 * @brief The Gwenesis core behind genesis.h.
 *
 * The frame loop follows retro-go's gwenesis/main/main.c (GPLv3), which
 * drives the same snapshot of the core.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "genesis.h"

#include <string.h>

#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_vdp.h"
#include "m68k.h"
#include "ym2612.h"
#include "z80inst.h"

// The board runs genesis_render_pending from SRAM (genesis_fast.h).
#if defined(__arm__) && !defined(GWENESIS_FAST)
#define GWENESIS_FAST __attribute__((section("genesis_fast")))
#elif !defined(GWENESIS_FAST)
#define GWENESIS_FAST
#endif
// And its copies of the VDP's state in the apps' SRAM area (app_overlay.h).
#if defined(__arm__)
/// Puts a variable in the apps' SRAM area (on the board).
#define GENESIS_RAM __attribute__((section("app_ov_genesis.data")))
#else
#define GENESIS_RAM
#endif

// The core's own variables (gwenesis, documented there).
/** @cond GENESIS_CORE */
extern unsigned char *VRAM, *ZRAM;
extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned int gwenesis_vdp_status;
extern unsigned short CRAM565[256];
extern unsigned short VSRAM[];
extern unsigned char SAT_CACHE[];
extern int screen_width, screen_height;
extern int hint_pending;
/** @endcond */

/// The line being drawn (the core reads it).
int scan_line;
/// The YM2612's samples for the frame.
int16_t *gwenesis_ym2612_buffer;
/// Their count, and the YM2612's clock count.
///@{
int ym2612_index, ym2612_clock;
///@}
/// The SN76489's samples for the frame.
int16_t *gwenesis_sn76489_buffer;
/// Their count, and its clock count.
///@{
int sn76489_index, sn76489_clock;
///@}

/// The YM2612, the Z80 and the SN76489 are on.
///@{
static bool ym_on = true, z80_on = true, sn_on = true;
///@}
/// The clock for timing.
static uint32_t (*clock_us)(void);

genesis_times genesis_time;
/// The two pads' buttons.
static unsigned pads[2];

// ---- Drawing, a line at a time, perhaps on another core ----
///
/// The core's line drawing (gwenesis_vdp_gfx.c) is built with its VDP
/// registers, VSRAM and sprite table renamed to these copies (see the
/// Makefile and CMakeLists.txt), which genesis_render_pending fills from each
/// queued line, so it draws the line as it was even while the 68000 has moved
/// on. VRAM is shared.
unsigned char genesis_r_regs[0x20];
/// The VSRAM copy the line drawer reads.
unsigned short genesis_r_vsram[0x40] GENESIS_RAM;
/// The sprite table copy it reads.
unsigned char __attribute__((aligned(4))) genesis_r_sat[0x400] GENESIS_RAM;

/// A line to draw: the VDP state it is drawn with.
typedef struct {
    genesis_line_fn fn;  ///< where the line goes
    void *ctx;  ///< its argument
    int16_t line;  ///< the line
    uint8_t regs[0x20];  ///< the VDP registers
    uint16_t vsram[40];  ///< the 40 VSRAM entries
    uint16_t palette[64];  ///< the palette (64 RGB565 colours)
} line_job;
_Static_assert(sizeof(line_job) <= 288, "GENESIS_DRAW_MEM is too small");

static uint8_t *sat_queued;  ///< SAT_CACHE at the start of the last frame queued
static line_job *queue;      ///< GENESIS_DRAW_QUEUE of them
static uint8_t *draw_buf;    ///< the line being drawn (320, 4-aligned)
/// Lines queued and lines drawn.
///@{
static volatile uint32_t queue_head, queue_tail;
///@}
/// Core 1 draws the lines.
static bool worker;

/// Pads are set by genesis_set_pad as they change.
void gwenesis_io_get_buttons(void) {}

// Save states aren't kept yet: the core's calls (its interface, documented
// there) go nowhere.
/** @cond GENESIS_CORE */
SaveState *saveGwenesisStateOpenForRead(const char *name) { return NULL; }
SaveState *saveGwenesisStateOpenForWrite(const char *name) { return NULL; }
int saveGwenesisStateGet(SaveState *s, const char *tag) { return 0; }
void saveGwenesisStateSet(SaveState *s, const char *tag, int value) {}
void saveGwenesisStateGetBuffer(SaveState *s, const char *tag, void *buf, int len) {}
void saveGwenesisStateSetBuffer(SaveState *s, const char *tag, void *buf, int len) {}
/** @endcond */

_Static_assert(GENESIS_YM_FAST_SIZE == YM2612_FAST_TABLES && GENESIS_YM_SLOW_SIZE == YM2612_SLOW_TABLES,
               "the YM2612's tables");

void genesis_init(uint8_t *mem, void *ym_fast, void *ym_slow) {
    memset(mem, 0, GENESIS_MEM_SIZE);
    VRAM = mem;
    M68K_RAM = mem + VRAM_MAX_SIZE;
    ZRAM = M68K_RAM + MAX_RAM_SIZE;
    gwenesis_ym2612_buffer = (int16_t *)(ZRAM + MAX_Z80_RAM_SIZE);
    gwenesis_sn76489_buffer = gwenesis_ym2612_buffer + GENESIS_AUDIO_MAX;
    sat_queued = (uint8_t *)(gwenesis_sn76489_buffer + GENESIS_AUDIO_MAX);
    queue = (line_job *)(sat_queued + 0x400);
    draw_buf = (uint8_t *)(queue + GENESIS_DRAW_QUEUE);
    queue_head = queue_tail = 0;
    YM2612SetTables(ym_fast, ym_slow);
}

const char *genesis_load(uint8_t *rom, size_t size) {
    static char name[49];
    load_cartridge(rom, size);
    power_on();
    reset_emulation();
    memset(pads, 0, sizeof pads);
    // The overseas name, at $150 (bytes stored swapped, as the core keeps them).
    int n = 0;
    for (int i = 0; i < 48; i++)
        name[i] = rom[(0x150 + i) ^ 1];
    for (int i = 0; i < 48; i++)
        if (name[i] != ' ' || (n && name[n - 1] != ' '))
            name[n++] = name[i] >= 32 && name[i] < 127 ? name[i] : '?';
    while (n && name[n - 1] == ' ')
        --n;
    name[n] = 0;
    return name;
}

void genesis_reset(void) {
    reset_emulation();
}

void genesis_set_pad(int pad, unsigned buttons) {
    unsigned changed = buttons ^ pads[pad];
    pads[pad] = buttons;
    for (int b = 0; b < 8; b++)
        if (changed & (1u << b)) {
            if (buttons & (1u << b))
                gwenesis_io_pad_press_button(pad, b);
            else
                gwenesis_io_pad_release_button(pad, b);
        }
}

void genesis_set_clock(uint32_t (*clock)(void)) {
    clock_us = clock;
}

void genesis_set_worker(bool on) {
    worker = on;
}

/// Draws the lines queued (on whichever core calls it).
GWENESIS_FAST void genesis_render_pending(void) {
    uint32_t tail = queue_tail;
    while (tail != __atomic_load_n(&queue_head, __ATOMIC_ACQUIRE)) {
        line_job *j = &queue[tail % GENESIS_DRAW_QUEUE];
        uint32_t t0 = clock_us ? clock_us() : 0;
        memcpy(genesis_r_regs, j->regs, sizeof j->regs);
        memcpy(genesis_r_vsram, j->vsram, sizeof j->vsram);
        if (j->line == 0) {
            memcpy(genesis_r_sat, sat_queued, sizeof genesis_r_sat);
            screen_width = j->regs[12] & 1 ? 320 : 256;  // H40
            screen_height = j->regs[1] & 8 ? 240 : 224;  // V30
            gwenesis_vdp_render_config();
        }
        // The core draws line n at buffer + n * 320 (H32 starts 32 in).
        uint8_t *at = draw_buf + (screen_width == 256 ? 32 : 0);
        if (j->regs[0] & 1)  // display off
            memset(draw_buf, 0, SCREEN_WIDTH);
        gwenesis_vdp_set_buffer((unsigned short *)(at - j->line * SCREEN_WIDTH));
        gwenesis_vdp_render_line(j->line);
        uint32_t t1 = clock_us ? clock_us() : 0;
        j->fn(j->ctx, j->line, draw_buf, j->palette);
        if (clock_us) {
            uint32_t t2 = clock_us();
            genesis_time.draw_us += t2 - t0;
            genesis_time.put_us += t2 - t1;
        }
        __atomic_store_n(&queue_tail, ++tail, __ATOMIC_RELEASE);
    }
}

void genesis_render_wait(void) {
    while (__atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) != queue_head) {
        if (!worker)
            genesis_render_pending();
    }
}

/// Queues line for drawing as the VDP is now (and draws it, without a worker).
/// @param fn where the line goes
/// @param ctx its argument
/// @param line the line
static void queue_line(genesis_line_fn fn, void *ctx, int line) {
    uint32_t head = queue_head;
    if (head - __atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) == GENESIS_DRAW_QUEUE) {
        uint32_t t0 = clock_us ? clock_us() : 0;
        while (head - __atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) == GENESIS_DRAW_QUEUE) {
        }
        if (clock_us)
            genesis_time.wait_us += clock_us() - t0;
    }
    line_job *j = &queue[head % GENESIS_DRAW_QUEUE];
    j->fn = fn;
    j->ctx = ctx;
    j->line = (int16_t)line;
    memcpy(j->regs, gwenesis_vdp_regs, sizeof j->regs);
    memcpy(j->vsram, VSRAM, sizeof j->vsram);
    memcpy(j->palette, CRAM565, sizeof j->palette);
    // Sprites are taken as they are at the start of the frame. The worker is
    // never a frame behind, so it has read the last frame's copy by now.
    if (line == 0)
        memcpy(sat_queued, SAT_CACHE, 0x400);
    __atomic_store_n(&queue_head, head + 1, __ATOMIC_RELEASE);
    if (!worker)
        genesis_render_pending();
}

void genesis_ym_half_rate(bool on) {
    ym2612_half_rate = on;
}

void genesis_enable(bool ym2612, bool z80, bool sn76489) {
    ym_on = ym2612;
    z80_on = z80;
    sn_on = sn76489;
}

void genesis_run_frame(genesis_line_fn line, void *ctx) {
    int lines = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    int hint_counter = gwenesis_vdp_regs[10];
    int height = REG1_PAL ? 240 : 224;  // (the drawing keeps its own screen_height)

    // Each frame's clocks start at 0; one this far ahead never runs.
    int system_clock = 0;
    zclk = z80_on ? 0 : 0x1000000;
    ym2612_clock = ym_on ? 0 : 0x1000000;
    ym2612_index = 0;
    ym2612_timer_index = 0;
    sn76489_clock = sn_on ? 0 : 0x1000000;
    sn76489_index = 0;

    for (scan_line = 0; scan_line < lines;) {
        uint32_t t0 = clock_us ? clock_us() : 0;
        m68k_run(system_clock + VDP_CYCLES_PER_LINE);
        uint32_t t1 = clock_us ? clock_us() : 0;
        z80_run(system_clock + VDP_CYCLES_PER_LINE);
        if (clock_us) {
            uint32_t t2 = clock_us();
            genesis_time.m68k_us += t1 - t0;
            genesis_time.z80_us += t2 - t1;
        }
        // The sound chips keep up every few lines (as well as at each
        // write), so their work is spread over the frame: on the board it
        // then overlaps the drawing of the lines on core 1.
        if ((scan_line & 3) == 3) {
            uint32_t t0 = clock_us ? clock_us() : 0;
            gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
            ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
            if (clock_us)
                genesis_time.sound_us += clock_us() - t0;
        }

        if (line && scan_line < height)
            queue_line(line, ctx, scan_line);

        if (scan_line == 0 || scan_line > height)
            hint_counter = REG10_LINE_COUNTER;
        if (--hint_counter < 0) {
            if (REG0_LINE_INTERRUPT != 0 && scan_line <= height) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
            }
            hint_counter = REG10_LINE_COUNTER;
        }

        scan_line++;
        if (scan_line == height) {
            if (REG1_VBLANK_INTERRUPT != 0) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            z80_irq_line(1);
        }
        if (scan_line == height + 1)
            z80_irq_line(0);
        system_clock += VDP_CYCLES_PER_LINE;
    }

    // The sound chips catch up to the end of the frame.
    if (GWENESIS_AUDIO_ACCURATE == 1) {
        gwenesis_SN76489_run(system_clock);
        ym2612_run(system_clock);
    }
    m68k.cycles -= system_clock;
}

int genesis_width(void) {
    return REG12_MODE_H40 ? 320 : 256;
}

int genesis_height(void) {
    return REG1_PAL ? 240 : 224;
}

bool genesis_pal(void) {
    return REG1_PAL;
}

const uint16_t *genesis_palette(void) {
    return CRAM565;
}

int genesis_audio(const int16_t **ym2612, const int16_t **sn76489) {
    int n = ym2612_index > sn76489_index ? ym2612_index : sn76489_index;
    if (ym2612_index < n)
        memset(gwenesis_ym2612_buffer + ym2612_index, 0, (n - ym2612_index) * sizeof(int16_t));
    if (sn76489_index < n)
        memset(gwenesis_sn76489_buffer + sn76489_index, 0, (n - sn76489_index) * sizeof(int16_t));
    *ym2612 = gwenesis_ym2612_buffer;
    *sn76489 = gwenesis_sn76489_buffer;
    return n;
}
