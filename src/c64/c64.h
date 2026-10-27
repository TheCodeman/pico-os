/**
 * @file c64.h
 * @brief A PAL Commodore 64.
 *
 * the 6510 with its I/O port, the PLA's memory banking (with a cartridge's
 * modes), 64 KB of RAM and the 1 KB colour RAM, the BASIC, KERNAL and
 * character ROMs, the VIC-II, two CIAs (keyboard and joysticks on CIA 1, the
 * VIC bank and an empty serial bus on CIA 2) and the SID's registers (the
 * sound itself is the caller's, through sid_write). Portable C with no Pico
 * SDK calls: the board and the PC test harness both drive it through this
 * interface.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "c64_cia.h"
#include "c64_cpu.h"
#include "c64_sid.h"
#include "c64_vic.h"

/// An emulated PAL Commodore 64 (see struct c64).
typedef struct c64 c64;
/// A cartridge (c64_cart.h).
struct c64_cart;
/// A true 1541 drive (c64_drive.h).
struct c64_drive;

/// The whole C64: CPU, memory, VIC-II, SID, two CIAs, keyboard, joysticks, cartridge and drive.
struct c64 {
    c64_cpu cpu;  ///< the 6510
    c64_cia cia1, cia2;  ///< CIA 1 (keyboard, joysticks) and CIA 2 (serial bus, VIC bank)
    c64_vic vic;  ///< the VIC-II
    uint64_t cycles;  ///< since power-on

    /// Memory. The caller points these anywhere (the PC uses malloc).
    uint8_t *ram;     ///< 64 KB
    uint8_t *color;   ///< 1 KB, low nybbles
    const uint8_t *basic, *kernal, *charrom;  ///< 8 KB, 8 KB, 4 KB

    /// A cartridge (c64_cart.c), or NULL.
    struct c64_cart *cart;
    /// A 1541 on the serial bus (c64_drive.c), or NULL. With one, LOAD and
    /// SAVE on device 8 go to it instead of the load and save hooks.
    struct c64_drive *drive;

    /// The 6510's I/O port: $00 is the direction, $01 the data.
    uint8_t port_dir, port_data;
    /// Pages for the CPU: where each 256-byte page reads from and writes to,
    /// recomputed when the port changes. NULL: I/O ($D0-$DF with I/O banked
    /// in). $0000 and $0001 in RAM hold the port's direction and what reading
    /// it gives, so zero page reads straight from RAM too.
    const uint8_t *rd[256];
    uint8_t *wr[256];  ///< RAM, or NULL for the I/O pages while I/O is banked in
    bool io_in;  ///< I/O is mapped in at $D000

    /// The keyboard matrix: bit b of keys[a] is the key at CIA 1 port A bit a
    /// (the column the KERNAL drives low) and port B bit b. Joysticks: bits 0-4
    /// are up, down, left, right and fire, 1 when pressed. RESTORE is wired to
    /// the NMI.
    uint8_t keys[8];
    uint8_t joy[2];  ///< ports 1 and 2
    bool restore;  ///< the RESTORE key is down
    bool nmi_line;  ///< the NMI line now

    /// SID registers as last written, and a hook for making them into sound:
    /// called with the cycle count of each write.
    uint8_t sid[0x20];
    /// Voice 3 for reading back ($D41B, $D41C: games use its noise for random
    /// numbers), run on to the cycle count only when it is read or written.
    c64_sid sid_v3;
    uint64_t sid_v3_cycles;  ///< the cycle the SID's voice 3 last ran to
    void (*sid_write)(void *user, uint8_t reg, uint8_t value, uint64_t cycle);  ///< the SID's register writes, for the host to play
    void *user;  ///< the host program's pointer

    /// LOAD from device 8 and up, answered by the caller instead of a drive on
    /// the serial bus: called with the name as typed (PETSCII) when the
    /// KERNAL's LOAD starts, it puts the file in load_buf (its two address
    /// bytes first, up to 0x10002 bytes) and returns its length, or
    /// C64_LOAD_NOT_FOUND or C64_LOAD_NO_DEVICE. NULL: devices 8 and up aren't
    /// there.
    int (*load)(void *user, unsigned device, const uint8_t *name, int len, uint8_t *buf);
    uint8_t *load_buf;  ///< where LOAD puts a file
    /// SAVE to device 8 and up, the same way: called with the name and the
    /// file (its two address bytes first, in load_buf); returns 0, or
    /// C64_LOAD_NO_DEVICE, or C64_SAVE_FAILED (the disk is full, say).
    int (*save)(void *user, unsigned device, const uint8_t *name, int len, const uint8_t *data, int n);

    /// Skip the KERNAL's RAM test at power-on: its result (the top of BASIC's
    /// memory: the first page that isn't RAM) is worked out from the memory
    /// map at once, and READY. comes about two seconds sooner.
    bool fast_boot;

    /// Optional: a microsecond clock, to add up the time drawing lines takes
    /// (the VIC and line_out) in video_us.
    uint32_t (*time_us)(void);
    uint64_t video_us;  ///< time spent drawing

    int budget;       ///< CPU cycles left over (negative: overrun) in this line
    uint64_t line_cycle;  ///< the cycle count at the line's start
    uint32_t tod_acc;  ///< cycles towards the next tenth of a second
};

/// What the load hook returns when it has no file (the KERNAL's error codes,
/// negated).
#define C64_LOAD_NOT_FOUND (-4)
/// LOAD: no device answered (?DEVICE NOT PRESENT).
#define C64_LOAD_NO_DEVICE (-5)
#define C64_SAVE_FAILED    (-7)  ///< BASIC says NOT OUTPUT FILE

/// Sets up a machine whose memory and ROM pointers are already set, and
/// resets it (RAM is left alone: fill it first for a cold start).
void c64_init(c64 *m);
/// The reset line.
void c64_reset(c64 *m);
/// Plugs a 1541 in as device 8 (NULL takes it out).
void c64_attach_drive(c64 *m, struct c64_drive *d);
/// The cartridge's banks or lines changed: maps memory again.
void c64_cart_changed(c64 *m);
/// Switching it off and on: RAM back to its power-on pattern (stripes of $00
/// and $FF), the colour RAM cleared, then the reset line.
void c64_power_on(c64 *m);
/// True while BASIC is at READY. waiting for a key (the KERNAL's input loop).
bool c64_ready(const c64 *m);
/// Puts a program (its two address bytes first) into memory, as LOAD"...",8,1
/// does; one at $0801 (BASIC) gets BASIC's pointers set and RUN typed into
/// the keyboard buffer. Call it once c64_ready.
void c64_start_prg(c64 *m, const uint8_t *prg, int len);
/// Runs one raster line, or a whole frame (312 lines, 19656 cycles).
void c64_run_line(c64 *m);
/// Runs one frame (312 lines).
void c64_run_frame(c64 *m);

/// The CPU's view of memory, for debuggers and tests (reads have side
/// effects in I/O, as on the machine).
uint8_t c64_peek(c64 *m, uint16_t addr);
/// Writes memory as the CPU would (I/O included).
void c64_poke(c64 *m, uint16_t addr, uint8_t value);

/// Keys by matrix position (port A bit * 8 + port B bit).
void c64_key(c64 *m, unsigned key, bool down);
/// Keyboard matrix positions for c64_key (port A bit * 8 + port B bit).
enum {
    C64K_DEL = 0x00, C64K_RETURN = 0x01, C64K_CRSR_LR = 0x02, C64K_F7 = 0x03,
    C64K_F1 = 0x04, C64K_F3 = 0x05, C64K_F5 = 0x06, C64K_CRSR_UD = 0x07,
    C64K_LSHIFT = 0x0f, C64K_RSHIFT = 0x34, C64K_HOME = 0x33,
    C64K_CTRL = 0x3a, C64K_CBM = 0x3d, C64K_RUNSTOP = 0x3f, C64K_SPACE = 0x3c,
    C64K_LEFTARROW = 0x39, C64K_POUND = 0x30, C64K_UPARROW = 0x36,
};
/// The key and whether it needs Shift for an ASCII character (lower case
/// letters are the unshifted keys); false if the C64 has no such key.
bool c64_ascii_key(char ch, unsigned *key, bool *shift);
/// A screen code as the nearest ASCII character (for text dumps).
char c64_screen_ascii(uint8_t code);
