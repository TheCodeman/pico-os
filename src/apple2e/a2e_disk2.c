/**
 * @file a2e_disk2.c
 * @brief The Disk II controller: the stepper, motor and drive select, the Q6/Q7 read/write/sense modes, and tracks nibblized from (and decoded back into) 140 KB sector images.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "app_overlay.h"
#include "a2e_disk2.h"

#include <string.h>

#define VOLUME 254

/// 6&2 encoding: the 64 disk bytes a 6-bit value is written as.
static const uint8_t to_disk[64] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6, 0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

/// Where each physical sector of a track is in the image file.
static const uint8_t dos_order[16] = {0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15};
static const uint8_t prodos_order[16] = {0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15};

static uint8_t *sector_data(a2e_drive *d, int track, int phys) {
    const uint8_t *order = d->prodos_order ? prodos_order : dos_order;
    return d->image + (track * 16 + order[phys]) * 256;
}

// ---- Nibblizing ----------------------------------------------------------------

static void nibblize(a2e_drive *d) {
    uint8_t *n = d->nib;
    int p = 0;
    memset(n, 0xff, A2E_NIB_TRACK);
    p += 48;  // gap 1
    for (int s = 0; s < 16; ++s) {
        // Address field: volume, track, sector and checksum in 4&4.
        n[p++] = 0xd5, n[p++] = 0xaa, n[p++] = 0x96;
        uint8_t field[4] = {VOLUME, (uint8_t)d->track, (uint8_t)s,
                            (uint8_t)(VOLUME ^ d->track ^ s)};
        for (int k = 0; k < 4; ++k) {
            n[p++] = (uint8_t)((field[k] >> 1) | 0xaa);
            n[p++] = (uint8_t)(field[k] | 0xaa);
        }
        n[p++] = 0xde, n[p++] = 0xaa, n[p++] = 0xeb;
        p += 6;  // gap 2

        // Data field: 86 bytes of the low two bits (each pair reversed),
        // 256 of the high six, each sent XORed with the one before, then a
        // checksum.
        n[p++] = 0xd5, n[p++] = 0xaa, n[p++] = 0xad;
        const uint8_t *src = sector_data(d, d->track, s);
        uint8_t buf[342];
        for (int i = 0; i < 86; ++i) {
            uint8_t v = 0;
            for (int k = 0; k < 3; ++k) {
                int j = i + 86 * k;
                if (j < 256)
                    v |= (uint8_t)((((src[j] & 1) << 1) | ((src[j] >> 1) & 1)) << (2 * k));
            }
            buf[i] = v;
        }
        for (int i = 0; i < 256; ++i)
            buf[86 + i] = src[i] >> 2;
        uint8_t last = 0;
        for (int i = 0; i < 342; ++i) {
            n[p++] = to_disk[buf[i] ^ last];
            last = buf[i];
        }
        n[p++] = to_disk[last];
        n[p++] = 0xde, n[p++] = 0xaa, n[p++] = 0xeb;
        p += 27;  // gap 3
    }
}

// ---- Decoding what was written ---------------------------------------------------

static uint8_t from_disk[256] APP_OV_DATA(apple2);  ///< 0xff: not a valid disk byte

static uint8_t nib_at(const a2e_drive *d, int i) {
    return d->nib[i % A2E_NIB_TRACK];
}

static int odd_even(const a2e_drive *d, int i) {
    return ((nib_at(d, i) << 1) | 1) & nib_at(d, i + 1);
}

/// Finds each sector's address and data fields on the track and stores the
/// sectors that decode cleanly back into the image.
static void denibblize(a2e_drive *d) {
    if (!from_disk[0x96]) {
        memset(from_disk, 0xff, sizeof from_disk);
        for (int i = 0; i < 64; ++i)
            from_disk[to_disk[i]] = (uint8_t)i;
    }
    for (int i = 0; i < A2E_NIB_TRACK; ++i) {
        if (nib_at(d, i) != 0xd5 || nib_at(d, i + 1) != 0xaa || nib_at(d, i + 2) != 0x96)
            continue;
        int vol = odd_even(d, i + 3), trk = odd_even(d, i + 5), sec = odd_even(d, i + 7);
        if (odd_even(d, i + 9) != (vol ^ trk ^ sec) || trk != d->track || sec > 15)
            continue;
        // The data field follows within a few dozen nibbles.
        int j = i + 11, end = i + 11 + 64;
        while (j < end && !(nib_at(d, j) == 0xd5 && nib_at(d, j + 1) == 0xaa &&
                            nib_at(d, j + 2) == 0xad))
            ++j;
        if (j >= end)
            continue;
        j += 3;
        uint8_t buf[342], last = 0;
        bool ok = true;
        for (int k = 0; k < 342 && ok; ++k) {
            uint8_t v = from_disk[nib_at(d, j + k)];
            ok = v != 0xff;
            buf[k] = last = (uint8_t)(v ^ last);
        }
        if (!ok || from_disk[nib_at(d, j + 342)] != last)
            continue;
        uint8_t *dst = sector_data(d, d->track, sec);
        uint8_t data[256];
        for (int k = 0; k < 256; ++k) {
            uint8_t lo = (buf[k % 86] >> (2 * (k / 86))) & 3;
            data[k] = (uint8_t)(buf[86 + k] << 2 | (lo & 1) << 1 | (lo >> 1));
        }
        if (memcmp(dst, data, 256)) {
            memcpy(dst, data, 256);
            d->dirty = true;
        }
    }
}

// ---- The drive -----------------------------------------------------------------

static void flush_track(a2e_drive *d) {
    if (d->track_dirty && d->image && d->track >= 0)
        denibblize(d);
    d->track_dirty = false;
}

static void load_track(a2e_drive *d) {
    int t = d->halftrack >> 1;
    if (t == d->track)
        return;
    flush_track(d);
    d->track = t;
    if (d->image)
        nibblize(d);
    else
        memset(d->nib, 0, sizeof d->nib);
}

static void step(a2e_disk2 *c, int phase, bool on) {
    if (on)
        c->phases |= (uint8_t)(1u << phase);
    else
        c->phases &= (uint8_t)~(1u << phase);
    // The head moves a half track towards a magnet next to its position.
    a2e_drive *d = &c->drive[c->sel];
    int dir = 0;
    if (c->phases & (1u << ((d->halftrack + 1) & 3)))
        ++dir;
    if (c->phases & (1u << ((d->halftrack + 3) & 3)))
        --dir;
    int ht = d->halftrack + dir;
    if (ht < 0)
        ht = 0;
    if (ht > 69)
        ht = 69;
    d->halftrack = ht;
}

static uint8_t disk_io(void *ctx, uint8_t r, bool write, uint8_t v) {
    a2e_disk2 *c = ctx;
    switch (r) {
    case 0x8:
        if (c->motor)
            c->spin_until = *c->now + A2E_CPU_HZ;
        c->motor = false;
        break;
    case 0x9: c->motor = true; break;
    case 0xa: c->sel = 0; break;
    case 0xb: c->sel = 1; break;
    case 0xc: c->q6 = false; break;
    case 0xd: c->q6 = true; break;
    case 0xe: c->q7 = false; break;
    case 0xf: c->q7 = true; break;
    default: step(c, r >> 1, r & 1); break;
    }
    a2e_drive *d = &c->drive[c->sel];

    if (r == 0xc && d->image && a2e_disk2_spinning(c)) {
        // Q6L shifts a byte: the next nibble under the head is read into the
        // latch, or (in write mode) the latch is written there.
        load_track(d);
        if (c->q7) {
            if (!d->write_protect) {
                d->nib[d->pos] = c->latch;
                d->track_dirty = true;
            }
        } else if (c->shifting) {
            // Between nibbles the latch is filling with the next one's bits
            // (bit 7 clear), as on the real card. Without this, two reads in
            // a gap of $FFs look the same, and DOS takes the disk for stopped
            // and waits for the motor to come up to speed on every call.
            c->latch &= 0x7f;
            c->shifting = false;
            return c->latch;
        } else {
            c->latch = d->nib[d->pos];
            c->shifting = true;
        }
        if (++d->pos >= A2E_NIB_TRACK)
            d->pos = 0;
    } else if (c->q6 && !c->q7) {
        // Sense: bit 7 is the write protect switch.
        c->latch = d->write_protect ? 0x80 : 0;
    } else if (c->q6 && c->q7 && write) {
        // Load: a write with Q6 and Q7 high puts the byte in the latch.
        c->latch = v;
    }
    return (r & 1) ? 0xa0 : c->latch;
}

// ---- The interface -----------------------------------------------------------------

bool a2e_disk2_check_rom(uint8_t p5[256]) {
    static const uint8_t start[8] = {0xa2, 0x20, 0xa0, 0x00, 0xa2, 0x03, 0x86, 0x3c};
    if (!memcmp(p5, start, sizeof start))
        return true;
    // A dump read with data lines D4-D7 in reverse order and address lines
    // A5 and A7 swapped.
    uint8_t fixed[256];
    for (int i = 0; i < 256; ++i) {
        uint8_t b = p5[(i & 0x5f) | (i & 0x20) << 2 | (i & 0x80) >> 2];
        fixed[i] = (uint8_t)((b & 0x0f) | (b & 0x10) << 3 | (b & 0x20) << 1 | (b & 0x40) >> 1 |
                             (b & 0x80) >> 3);
    }
    if (memcmp(fixed, start, sizeof start) || fixed[0xf8] != 0x4c)  // ends JMP $0801
        return false;
    memcpy(p5, fixed, 256);
    return true;
}

void a2e_disk2_install(a2e *m, a2e_disk2 *c, int slot, const uint8_t p5[256]) {
    memset(c, 0, sizeof *c);
    c->drive[0].track = c->drive[1].track = -1;
    c->now = &m->cycles;
    memcpy(m->slot_rom[slot], p5, 256);
    m->slot_has_rom |= (uint8_t)(1u << slot);
    m->slot_io[slot] = disk_io;
    m->slot_ctx[slot] = c;
}

void a2e_disk2_insert(a2e_disk2 *c, int drive, uint8_t *image, bool prodos_order,
                      bool write_protect) {
    a2e_drive *d = &c->drive[drive];
    flush_track(d);
    d->image = image;
    d->prodos_order = prodos_order;
    d->write_protect = write_protect;
    d->dirty = false;
    d->track = -1;
}

void a2e_disk2_flush(a2e_disk2 *c, int drive) {
    flush_track(&c->drive[drive]);
}
