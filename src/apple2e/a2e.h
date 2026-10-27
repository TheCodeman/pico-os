/**
 * @file a2e.h
 * @brief An enhanced Apple IIe.
 *
 * the 65C02 (from src/applesoft), 64 KB of main and 64 KB of auxiliary RAM
 * (an extended 80-column card), the language card, the MMU and IOU soft
 * switches, keyboard, speaker and game port, running the real 16 KB ROM.
 * Portable C with no Pico SDK calls: the board and the PC test harness both
 * drive it through this interface.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "as_cpu.h"

#define A2E_CPU_HZ        1020484  ///< NTSC: 14.31818 MHz / 14, stretched every 65th cycle
#define A2E_FRAME_CYCLES  17030    ///< 262 lines of 65 cycles
#define A2E_VBL_CYCLES    4550     ///< lines 192-261

/// An emulated enhanced Apple IIe (see struct a2e).
typedef struct a2e a2e;

/// The whole machine: CPU, memory banks, soft switches, slots, keyboard and video state.
struct a2e {
    as_cpu cpu;  ///< the 65C02
    uint64_t cycles;  ///< since power-on

    /// Memory. The caller may point these anywhere (the PC uses malloc).
    uint8_t *main;     ///< 64 KB; $D000-$DFFF here is language card bank 2
    uint8_t *aux;      ///< 64 KB, the same layout
    uint8_t *lc1_main; ///< 4 KB: language card bank 1 ($D000-$DFFF), main
    uint8_t *lc1_aux;  ///< 4 KB: and aux
    uint8_t rom[0x4000];  ///< $C000-$FFFF; $C100-$CFFF is the internal slot firmware
    uint8_t slot_rom[8][256];  ///< $Cn00-$CnFF for each slot that has a card
    uint8_t slot_has_rom;      ///< bit n: slot n has a ROM

    /// Pages for the CPU: where each 256-byte page of the address space reads
    /// from and writes to, recomputed whenever a memory switch changes. NULL:
    /// the I/O and slot ROM area ($C0-$CF), or (for writes) ROM.
    const uint8_t *rd[256];
    uint8_t *wr[256];  ///< where each 256-byte page is written (NULL: not writable)

    /// MMU switches.
    ///@{
    bool store80, ramrd, ramwrt, intcxrom, altzp, slotc3rom, intc8rom;
    ///@}
    /// language card banking: bank 2, reading RAM, writing RAM, write armed
    ///@{
    bool lc_bank2, lc_read_ram, lc_write_ram, lc_prewrite;
    ///@}
    /// IOU switches.
    ///@{
    bool text, mixed, page2, hires, col80, altchar, dhires, ioudis;
    ///@}
    bool an[4];  ///< the four annunciator outputs

    /// Keyboard: the last key (7 bits) and the strobe (bit 7 of $C000).
    uint8_t key;
    /// the keyboard strobe, and whether a key is held
    ///@{
    bool strobe, key_down;
    ///@}
    /// Game port: Open Apple, Solid Apple (buttons 0 and 1) and button 2;
    /// paddles 0-255.
    bool button[3];
    uint8_t paddle[4];  ///< the paddles' positions (0-255)
    uint64_t paddle_trigger;  ///< the cycle the paddles' timers started

    /// A card's I/O ($C0n0-$C0nF for slot n, n = 1-7), called with its
    /// slot_ctx. NULL if no card.
    uint8_t (*slot_io[8])(void *ctx, uint8_t reg, bool write, uint8_t v);
    void *slot_ctx[8];  ///< each slot card's own state
    /// A card that answers in its $Cn00-$CnFF space itself (the Mockingboard's
    /// VIAs), instead of a ROM. Called with slot_ctx.
    uint8_t (*slot_mem[8])(void *ctx, uint8_t addr, bool write, uint8_t v);
    /// Timed events: slot_event[n] is called once the cycle count reaches
    /// slot_event_at[n] (set them with a2e_set_event). next_event is the
    /// earliest of them.
    void (*slot_event[8])(void *ctx, uint64_t at);
    uint64_t slot_event_at[8];  ///< the cycle each slot wants its event at
    uint64_t next_event;  ///< the earliest slot event
    uint8_t irq_slots;  ///< bit n: slot n holds the IRQ line
    /// Called when the reset line goes (cards on the bus see it too).
    void (*slot_reset[8])(void *ctx);

    /// Each touch of $C030 flips the speaker cone, at this cycle count.
    void (*speaker)(void *user, uint64_t cycle);
    void *user;  ///< the host program's pointer
    uint32_t speaker_clicks;  ///< speaker clicks so far
};

/// Sets up a machine with its memory already pointed to (contents are kept:
/// clear them first for a cold start). rom16k is the $C000-$FFFF image.
void a2e_init(a2e *m, const uint8_t rom16k[0x4000]);
/// The reset line (power-on or Ctrl-Reset): the MMU's switches clear and the
/// CPU restarts at the reset vector. Hold m->button[0] and [1] across it for
/// the built-in self test.
void a2e_reset(a2e *m);
/// A cold start, as at power-on (the ROM then boots from the disk drives):
/// the power-up byte at $3F4 is made wrong and the reset line pulled.
void a2e_cold_start(a2e *m);
/// Runs whole instructions for at least `cycles` cycles; returns how many ran.
uint32_t a2e_run(a2e *m, uint32_t cycles);

/// For cards: asks for slot_event[slot] at cycle `at` (UINT64_MAX: never),
/// and holds or releases the IRQ line.
void a2e_set_event(a2e *m, int slot, uint64_t at);
/// Sets or clears the interrupt request of the card in `slot`.
void a2e_set_irq(a2e *m, int slot, bool on);

/// A typed character as the Apple gets it: with Caps Lock down the IIe's
/// keyboard gives capitals whether Shift is held or not, which most older
/// programs need (they only look for upper case).
static inline uint8_t a2e_caps(uint8_t c, bool caps_lock) {
    return caps_lock && c >= 'a' && c <= 'z' ? (uint8_t)(c - 0x20) : c;
}

/// A key arrives (ASCII, 0-127): latched at $C000 with the strobe set.
void a2e_key(a2e *m, uint8_t ascii);
/// Whether a key is held down ($C010 bit 7).
void a2e_key_held(a2e *m, bool down);

/// CPU-visible memory without side effects (for tests and debuggers): what a
/// read of addr returns now, except for the I/O page, which reads as 0.
uint8_t a2e_peek(const a2e *m, uint16_t addr);

/// True during vertical blanking, from the cycle count.
static inline bool a2e_in_vbl(const a2e *m) {
    return m->cycles % A2E_FRAME_CYCLES >= A2E_FRAME_CYCLES - A2E_VBL_CYCLES;
}

// ---- a2e_video.c ----------------------------------------------------------------

/// Width of the framebuffer the Apple IIe draws into, in pixels.
#define A2E_FB_WIDTH  640
/// Height of the framebuffer the Apple IIe draws into, in pixels.
#define A2E_FB_HEIGHT 480

/// Draws the whole display into a 640x480 RGB332 framebuffer (280x192 doubled
/// to 560x384 and centred), as the soft switches select: 40/80-column text,
/// lo-res and double lo-res, hi-res and double hi-res, mixed mode, page 2.
void a2e_video_draw(const a2e *m, uint8_t *fb);
/// Drawing only the lines that changed since the last call, for the board:
/// each line's screen memory is hashed and compared with what was drawn.
/// line_double: one framebuffer row per Apple line (640x240, the scanout
/// doubles them), else two (640x480). y0 is the first picture row.
typedef struct {
    uint8_t *fb;  ///< the framebuffer drawn into
    int y0;  ///< the first row used
    bool line_double;  ///< each line drawn twice
    uint32_t key[192];  ///< what each line showed when drawn; 0: draw again
} a2e_screen;
/// Returns how many of the 192 lines were drawn.
uint32_t a2e_video_update(const a2e *m, a2e_screen *s);
/// Everything is drawn again next time.
void a2e_video_invalidate(a2e_screen *s);
/// The text screen as 24 lines of ASCII (40 or 80 columns, as the display
/// shows it), each ending in '\n'; buf needs 24 * 81 + 1 bytes. Inverse and
/// flashing characters come out as plain ones.
void a2e_text_dump(const a2e *m, char *buf);
