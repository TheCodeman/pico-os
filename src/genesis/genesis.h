/**
 * @file genesis.h
 * @brief A Sega Genesis / Mega Drive: the Gwenesis core in core/ (68000, Z80, VDP, YM2612 and SN76489; see README.md) behind a small interface, the way src/c64/ keeps its machine apart from the board.
 *
 * Portable C with no Pico SDK calls: the board and the PC harness (host/)
 * both drive it through this.
 *
 * The core keeps its state in globals, so there is one machine.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Samples one frame makes at most (PAL: 313 lines * 3420 / 1009 = 1060).
#define GENESIS_AUDIO_MAX 1100
/// Lines queued for drawing (genesis_render_pending), each up to 288 bytes,
/// and with them the sprite table and a line being drawn.
#define GENESIS_DRAW_QUEUE 8
/// Bytes for the queue of lines waiting to be drawn on the other core.
#define GENESIS_DRAW_MEM   (0x400 + GENESIS_DRAW_QUEUE * 288 + 320)
/// The memory the machine works in, which the caller places: VRAM, the
/// 68000's and the Z80's RAM, the sound chips' sample buffers and the
/// drawing queue (fast memory: the board uses SRAM); the YM2612's tables read
/// for every sample (fast too), and its LFO's table (read only with LFO
/// vibrato on).
#define GENESIS_MEM_SIZE (0x10000 + 0x10000 + 0x2000 + 2 * GENESIS_AUDIO_MAX * 2 + GENESIS_DRAW_MEM)
/// The 68000 reads anything below 8 MB straight from the ROM buffer (there
/// is no mirroring), so a ROM buffer this big is always safe to read.
#define GENESIS_ROM_MAX 0x800000
/// Bytes of the YM2612's tables that should be in fast memory.
#define GENESIS_YM_FAST_SIZE (13 * 2 * 256 * 2 + 1024 * 2)
/// Bytes of the YM2612's tables that can be in slow memory.
#define GENESIS_YM_SLOW_SIZE (128 * 8 * 16)

/// Pad buttons, for genesis_set_pad.
enum {
    GENESIS_UP = 1 << 0,  ///< D-pad up
    GENESIS_DOWN = 1 << 1,  ///< D-pad down
    GENESIS_LEFT = 1 << 2,  ///< D-pad left
    GENESIS_RIGHT = 1 << 3,  ///< D-pad right
    GENESIS_B = 1 << 4,  ///< the B button
    GENESIS_C = 1 << 5,  ///< the C button
    GENESIS_A = 1 << 6,  ///< the A button
    GENESIS_START = 1 << 7,  ///< Start
};

/// One raster line of the picture: 320 colour numbers (0-255; colour n is
/// palette[n & 63], RGB565, as CRAM was when the line was reached; in H32
/// mode the 256 pixels are in the middle and the rest is 0). Called as each
/// visible line is drawn: by genesis_run_frame, or with genesis_set_worker
/// by whatever calls genesis_render_pending.
typedef void (*genesis_line_fn)(void *ctx, int line, const uint8_t *pix, const uint16_t *palette);

/// Sets the machine up in that memory. It keeps using it until the next
/// genesis_init.
void genesis_init(uint8_t *mem, void *ym_fast, void *ym_slow);

/// Plugs in a cartridge: rom is size bytes of a .bin/.gen/.md (or .smd)
/// image, which the core byte-swaps (and de-interleaves) in place and then
/// reads from where it is, so it must stay put and be writable; see
/// GENESIS_ROM_MAX. Then powers the machine on. Returns the header's name.
const char *genesis_load(uint8_t *rom, size_t size);
/// Resets the machine, as the reset button does.
void genesis_reset(void);

/// The buttons held on pad 0 or 1 (GENESIS_ bits).
void genesis_set_pad(int pad, unsigned buttons);

/// Turns parts off to save time (all on by default): the YM2612, the Z80
/// (and with it most music), the SN76489.
void genesis_enable(bool ym2612, bool z80, bool sn76489);
/// The YM2612 working out every other sample only (and repeating it): about
/// half its time, losing the top octave (above about 13 kHz).
void genesis_ym_half_rate(bool on);

/// Runs one frame (262 lines NTSC, 313 PAL). With line set, each visible
/// line is drawn and passed to it; NULL skips drawing (frame skip).
void genesis_run_frame(genesis_line_fn line, void *ctx);

/// Drawing elsewhere: with worker set, genesis_run_frame only queues each
/// line (with the VDP's registers, scrolling, palette and sprite table as
/// they were then: VRAM is read as it is when drawn) and another core draws
/// it by calling genesis_render_pending, which draws what is queued and
/// returns. genesis_run_frame waits when GENESIS_DRAW_QUEUE lines are
/// waiting. genesis_render_wait waits until all are drawn.
void genesis_set_worker(bool worker);
/// Draws the queued lines (on the worker core) and returns.
void genesis_render_pending(void);
/// Waits until every queued line is drawn.
void genesis_render_wait(void);

/// Where the time goes: with a clock set (microseconds, any start), each
/// frame adds the time spent in the 68000 (with the bus and the VDP's ports
/// and DMA), the Z80, and drawing lines (the core's and the line callback's).
typedef struct {
    uint64_t m68k_us, z80_us;  ///< time in the 68000 and the Z80
    uint64_t draw_us, put_us;  ///< drawing lines; of that, the line callback
    uint64_t wait_us;          ///< genesis_run_frame waiting for the drawing to catch up
    uint64_t sound_us;         ///< the sound chips catching up (they also run at each write)
} genesis_times;
/// Sets the clock genesis_time measures with (microseconds).
void genesis_set_clock(uint32_t (*clock_us)(void));
/// Where the time of the last frames went (for -b benchmarks).
extern genesis_times genesis_time;

/// The picture's size: 320 or 256 wide (the line is always 320), 224 or 240
/// high; and whether the machine is PAL (50 Hz) or NTSC (60 Hz).
int genesis_width(void);
/// The picture's height: 224 or 240.
int genesis_height(void);
/// True for a PAL (50 Hz) machine.
bool genesis_pal(void);

/// The 256 colours a colour number means now, as RGB565 (64 CRAM colours,
/// four times over: the core keeps priority bits in the top two).
const uint16_t *genesis_palette(void);

/// The frame's sound: mono, GENESIS_AUDIO_RATE samples a second, from the
/// YM2612 and the SN76489 separately (mix them as wanted). Valid until the
/// next genesis_run_frame.
#define GENESIS_AUDIO_RATE_NTSC 53267
/// Samples per second of the sound on a PAL machine.
#define GENESIS_AUDIO_RATE_PAL  52781
/// The frame's samples from the YM2612 and the SN76489 (see above); their count.
int genesis_audio(const int16_t **ym2612, const int16_t **sn76489);
