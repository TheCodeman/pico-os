/**
 * @file c64_drive.h
 * @brief A 1541 disk drive, run on its own ROM.
 *
 * its 6502 (c64_cpu.c, without the port) with 2 KB of RAM, VIA 1 on the
 * serial bus and VIA 2 driving the disk mechanism, and the disk surface as
 * GCR, made from a .d64 when it goes in and turned back into one for what
 * was written. The drive runs in step with the C64 (c64.c calls
 * c64_drive_run_to after each instruction), so fast loaders that count
 * cycles on the serial bus work. Portable C.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c64_cia.h"
#include "c64_cpu.h"
#include "c64_via.h"

/// The 1541's 6502 clock, in Hz.
#define C64_DRIVE_HZ    1000000
/// Tracks on a 1541 disk.
#define C64_DRIVE_TRACKS 35
#define C64_GCR_TRACK_MAX 7928  ///< the longest track (zone 3: 7692) with room to spare

/// An emulated 1541 disk drive (see struct c64_drive).
typedef struct c64_drive c64_drive;

/// The 1541: its 6502, two VIAs, RAM, ROM, head and the GCR data of the disk.
struct c64_drive {
    c64_cpu cpu;  ///< the drive's 6502
    c64_via via1, via2;  ///< VIA 1 (the serial bus) and VIA 2 (the head and motor)
    uint8_t *ram;          ///< 2 KB
    const uint8_t *rom;    ///< 16 KB, $C000-$FFFF
    const uint8_t *rd[256];  ///< each 256-byte page's memory for reading
    uint8_t *wr[256];  ///< each 256-byte page's memory for writing (NULL: not writable)

    /// The disk: each track's GCR bytes (the caller gives the room,
    /// C64_DRIVE_TRACKS * C64_GCR_TRACK_MAX bytes) and the .d64 it came from.
    uint8_t *gcr;
    uint16_t track_len[C64_DRIVE_TRACKS];  ///< each track's length in GCR bytes
    uint8_t *d64;          ///< NULL: no disk
    size_t d64_size;  ///< the .d64 image's size
    bool track_dirty[C64_DRIVE_TRACKS];  ///< tracks written since loading
    bool changed;          ///< written to since the last c64_drive_flush
    bool write_protect;  ///< the disk can't be written

    /// The head: half-track 2 is track 1; where on the track it is, and
    /// cycles towards the next byte.
    int half_track;
    int phase_was;         ///< the stepper's last phase
    uint16_t head;  ///< the head's place on the track
    uint16_t byte_clock;  ///< cycles until the next byte passes the head
    uint8_t last_byte;  ///< the byte last read
    bool sync;  ///< on a SYNC mark

    /// Time: the drive's cycles; while it idles, the VIAs are only run up to
    /// via_at, and nothing need happen before wake_at (a timer's interrupt).
    uint64_t cycles, via_at, wake_at;

    /// The serial bus: the C64's CIA 2 (whose port A drives ATN, CLOCK and
    /// DATA through inverters).
    const c64_cia *cia2;
    bool atn_was;  ///< the ATN line as last seen
};

/// Sets up a drive whose ram, rom and gcr are already pointed to; resets it.
void c64_drive_init(c64_drive *d, const c64_cia *cia2);
/// Resets the drive (as at power-on).
void c64_drive_reset(c64_drive *d);
/// Puts a .d64 in (NULL takes the disk out). The drive writes into it only
/// at c64_drive_flush.
void c64_drive_insert(c64_drive *d, uint8_t *d64, size_t size, bool write_protect);
/// Writes the tracks changed back into the .d64; true if any were.
bool c64_drive_flush(c64_drive *d);
/// Starts the drive's clock at the C64's cycle count (when plugging it in).
void c64_drive_sync(c64_drive *d, uint64_t c64_cycles);
/// Runs the drive until it has caught up with the C64's cycle count.
void c64_drive_run_to(c64_drive *d, uint64_t c64_cycles);
/// The serial bus's CLOCK and DATA lines as the drive pulls them (true:
/// held low), for the C64's CIA 2 to read.
void c64_drive_bus(const c64_drive *d, bool *clk_low, bool *data_low);
/// The drive's LED and motor.
bool c64_drive_led(const c64_drive *d);
/// True while the drive's motor is on.
bool c64_drive_motor(const c64_drive *d);
/// The track the head is over (1-35, or 0 between tracks).
int c64_drive_track(const c64_drive *d);
