/**
 * @file a2e_disk2.h
 * @brief A Disk II controller card with two 5.25" drives, for 140 KB disk images in DOS 3.3 order (.dsk, .do) or ProDOS order (.po).
 *
 * The track under the head is nibblized into a buffer when the head arrives
 * (6&2 encoding, as DOS 3.3 and ProDOS write it), and anything written to it
 * is decoded back into the image when the head leaves or the image is
 * flushed. Data moves a byte at a time: each read of the data latch gives
 * the next nibble. Portable C.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a2e.h"

#define A2E_DISK_SIZE   143360  ///< 35 tracks of 16 sectors of 256 bytes
#define A2E_NIB_TRACK   6656    ///< nibbles per track, as in .nib images

/// One Disk II drive: its disk image (as nibbles), head position and motor.
typedef struct {
    uint8_t *image;      ///< A2E_DISK_SIZE bytes, owned by the caller; NULL: empty
    bool prodos_order;   ///< .po; otherwise DOS 3.3 order
    bool write_protect;  ///< the disk can't be written
    bool dirty;          ///< the image has changed since it was inserted or saved
    int halftrack;       ///< 0-69
    int track;           ///< the track in nib[], or -1
    bool track_dirty;    ///< nib[] has been written to
    int pos;             ///< the next nibble under the head
    uint8_t nib[A2E_NIB_TRACK];  ///< the track under the head, as nibbles
} a2e_drive;

/// The Disk II controller card with its two drives.
typedef struct {
    a2e_drive drive[2];  ///< the controller's two drives
    int sel;             ///< 0 or 1
    uint8_t phases;      ///< stepper magnets on, bits 0-3
    bool motor, q6, q7;  ///< motor: the switch ($C0E9 on, $C0E8 off)
    uint8_t latch;  ///< the data latch the CPU reads and writes
    bool shifting;       ///< the next read sees the latch filling, not a nibble
    /// A drive keeps spinning for about a second after the motor is switched
    /// off (DOS counts on it: it switches off after each call and waits for
    /// the motor to come up to speed only if the disk has stopped).
    uint64_t spin_until;
    const uint64_t *now;  ///< the machine's cycle count
} a2e_disk2;

/// The P5 boot ROM (341-0027). Some dumps have data lines D4-D7 reversed
/// and address lines A5 and A7 swapped; this puts them right. False if it
/// isn't the boot ROM either way.
bool a2e_disk2_check_rom(uint8_t p5[256]);
/// Puts the card in a slot (6 as a rule) with its boot ROM.
void a2e_disk2_install(a2e *m, a2e_disk2 *c, int slot, const uint8_t p5[256]);
/// Inserts an image into drive 0 or 1 (NULL removes it, flushing first).
void a2e_disk2_insert(a2e_disk2 *c, int drive, uint8_t *image, bool prodos_order,
                      bool write_protect);
/// Decodes anything written to the current track back into the image. Call
/// before saving it.
void a2e_disk2_flush(a2e_disk2 *c, int drive);
/// Whether the selected drive's disk is turning.
static inline bool a2e_disk2_spinning(const a2e_disk2 *c) {
    return c->motor || *c->now < c->spin_until;
}
/// Whether the drive's light is on.
static inline bool a2e_disk2_active(const a2e_disk2 *c, int drive) {
    return a2e_disk2_spinning(c) && c->sel == drive;
}
