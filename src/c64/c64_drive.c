/**
 * @file c64_drive.c
 * @brief Implementation of c64_drive.h: a 1541 disk drive, run on its own ROM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_drive.h"

#include <string.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define FAST(f) __attribute__((section("app_ov_c64." #f))) f  ///< the apps' SRAM area (app_overlay.h)
#define TABLE   __attribute__((section("app_ov_c64.tables")))
#else
#define FAST(f) f
#define TABLE   const
#endif

/// The drive's clock against the C64's, 16.16: 1 MHz / 985248 Hz.
#define RATIO_Q16 ((uint32_t)(((uint64_t)C64_DRIVE_HZ << 16) / 985248))

// ---- GCR ---------------------------------------------------------------------------

static TABLE uint8_t to_gcr[16] = {
    0x0a, 0x0b, 0x12, 0x13, 0x0e, 0x0f, 0x16, 0x17, 0x09, 0x19, 0x1a, 0x1b, 0x0d, 0x1d, 0x1e, 0x15,
};

static int8_t from_gcr[32];  ///< -1: not a GCR code

static void gcr_tables(void) {
    memset(from_gcr, -1, sizeof from_gcr);
    for (int i = 0; i < 16; ++i)
        from_gcr[to_gcr[i]] = (int8_t)i;
}

/// 4 bytes as 5 GCR bytes.
static void encode4(const uint8_t *in, uint8_t *out) {
    uint64_t bits = 0;
    for (int i = 0; i < 4; ++i)
        bits = bits << 10 | (uint64_t)to_gcr[in[i] >> 4] << 5 | to_gcr[in[i] & 15];
    for (int i = 4; i >= 0; --i, bits >>= 8)
        out[i] = (uint8_t)bits;
}

/// 5 GCR bytes as 4 bytes; false if any code isn't GCR.
static bool decode5(const uint8_t *in, uint8_t *out) {
    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i)
        bits = bits << 8 | in[i];
    bool ok = true;
    for (int i = 3; i >= 0; --i, bits >>= 10) {
        int hi = from_gcr[(bits >> 5) & 31], lo = from_gcr[bits & 31];
        ok &= hi >= 0 && lo >= 0;
        out[i] = (uint8_t)((hi & 15) << 4 | (lo & 15));
    }
    return ok;
}

static int sectors(int track) {
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

static int zone_len(int track) {
    return track <= 17 ? 7692 : track <= 24 ? 7142 : track <= 30 ? 6666 : 6250;
}

static size_t sector_offset(int track, int sec) {
    size_t at = 0;
    for (int t = 1; t < track; ++t)
        at += (size_t)sectors(t);
    return (at + (size_t)sec) * 256;
}

static uint8_t *track_gcr(c64_drive *d, int track) {
    return d->gcr + (size_t)(track - 1) * C64_GCR_TRACK_MAX;
}

/// A track of the .d64 as the 1541 formats it: for each sector a sync, the
/// header (ID 8, checksum, sector, track, the disk's ID), a gap, a sync, the
/// data block (ID 7, 256 bytes, checksum) and a gap.
static void make_track(c64_drive *d, int track) {
    uint8_t *out = track_gcr(d, track), id1 = 0, id2 = 0;
    int len = zone_len(track), n = sectors(track), at = 0;
    if (d->d64 && sector_offset(18, 0) + 256 <= d->d64_size) {
        id1 = d->d64[sector_offset(18, 0) + 0xa2];
        id2 = d->d64[sector_offset(18, 0) + 0xa3];
    }
    int gap = (len - n * (5 + 10 + 9 + 5 + 325)) / n;
    memset(out, 0x55, (size_t)len);
    for (int s = 0; s < n; ++s) {
        memset(out + at, 0xff, 5);
        at += 5;
        uint8_t h[8] = {0x08, (uint8_t)(s ^ track ^ id2 ^ id1), (uint8_t)s, (uint8_t)track, id2, id1, 0x0f, 0x0f};
        encode4(h, out + at);
        encode4(h + 4, out + at + 5);
        at += 10 + 9;
        memset(out + at, 0xff, 5);
        at += 5;
        uint8_t block[260];
        block[0] = 0x07;
        size_t off = sector_offset(track, s);
        if (d->d64 && off + 256 <= d->d64_size)
            memcpy(block + 1, d->d64 + off, 256);
        else
            memset(block + 1, 0, 256);
        uint8_t sum = 0;
        for (int i = 1; i <= 256; ++i)
            sum ^= block[i];
        block[257] = sum;
        block[258] = block[259] = 0;
        for (int i = 0; i < 260; i += 4)
            encode4(block + i, out + at + i / 4 * 5);
        at += 325 + gap;
    }
    d->track_len[track - 1] = (uint16_t)len;
}

/// The sectors on a written track back into the .d64: each header found
/// after a sync, then the data block after the next sync.
static void read_back_track(c64_drive *d, int track) {
    const uint8_t *t = track_gcr(d, track);
    int len = d->track_len[track - 1];
    uint8_t raw[330], dec[264];
    for (int i = 0; i < len; ++i) {
        // The end of a sync: a byte after 0xFF 0xFF that isn't one.
        if (t[i] == 0xff || t[(i + len - 1) % len] != 0xff || t[(i + len - 2) % len] != 0xff)
            continue;
        for (int k = 0; k < 10; ++k)
            raw[k] = t[(i + k) % len];
        uint8_t h[8];
        if (!decode5(raw, h) || !decode5(raw + 5, h + 4) || h[0] != 0x08 || h[3] != track || h[2] >= sectors(track))
            continue;
        int sec = h[2];
        // The data block: after the next sync.
        int j = i + 10, end = i + 10 + 60;
        for (; j < end; ++j)
            if (t[j % len] != 0xff && t[(j + len - 1) % len] == 0xff && t[(j + len - 2) % len] == 0xff)
                break;
        if (j >= end)
            continue;
        for (int k = 0; k < 325; ++k)
            raw[k] = t[(j + k) % len];
        bool ok = true;
        for (int k = 0; k < 325; k += 5)
            ok &= decode5(raw + k, dec + k / 5 * 4);
        if (!ok || dec[0] != 0x07)
            continue;
        size_t off = sector_offset(track, sec);
        if (off + 256 <= d->d64_size)
            memcpy(d->d64 + off, dec + 1, 256);
    }
}

// ---- The drive ----------------------------------------------------------------------

static uint8_t open_read(void *ctx, uint16_t addr) {
    c64_drive *d = ctx;
    switch (addr & 0x1c00) {
    case 0x1800: return c64_via_read(&d->via1, addr & 15);
    case 0x1c00: return c64_via_read(&d->via2, addr & 15);
    default: return addr >> 8;  // nothing there
    }
}

static void open_write(void *ctx, uint16_t addr, uint8_t v) {
    c64_drive *d = ctx;
    switch (addr & 0x1c00) {
    case 0x1800: c64_via_write(&d->via1, addr & 15, v); break;
    case 0x1c00: c64_via_write(&d->via2, addr & 15, v); break;
    default:
        if (addr < 0x0800)
            d->ram[addr] = v;
        break;
    }
}

/// The serial bus: the C64 pulls a line low by writing 1 to its CIA 2 bit
/// (ATN 3, CLOCK 4, DATA 5); the drive with VIA 1 port B (DATA 1, CLOCK 3),
/// and DATA also while ATN and its acknowledge (bit 4) differ.
static inline void lines(const c64_drive *d, bool *atn_low, bool *clk_low, bool *data_low) {
    uint8_t c = c64_cia_out(d->cia2, 0), v = c64_via_out(&d->via1, 1);
    *atn_low = c & 0x08;
    *clk_low = (c & 0x10) || (v & 0x08);
    *data_low = (c & 0x20) || (v & 0x02) || (*atn_low != ((v & 0x10) != 0));
}

void c64_drive_bus(const c64_drive *d, bool *clk_low, bool *data_low) {
    uint8_t v = c64_via_out(&d->via1, 1);
    bool atn_low = c64_cia_out(d->cia2, 0) & 0x08;
    *clk_low = v & 0x08;
    *data_low = (v & 0x02) || (atn_low != ((v & 0x10) != 0));
}

static uint8_t via1_port(void *ctx, c64_via *via, int port) {
    c64_drive *d = ctx;
    if (!port)
        return 0xff;
    bool atn, clk, data;
    lines(d, &atn, &clk, &data);
    // Inputs (through inverters): DATA 0, CLOCK 2, ATN 7; 5-6 the device
    // number jumpers (both 0: drive 8).
    return (uint8_t)((c64_via_out(via, 1) & 0x1a) | (data ? 0x01 : 0) | (clk ? 0x04 : 0) | (atn ? 0x80 : 0));
}

static uint8_t via2_port(void *ctx, c64_via *via, int port) {
    c64_drive *d = ctx;
    if (!port)
        return d->last_byte;
    // Inputs: write protect 4 (0: protected), SYNC 7 (0: in a sync).
    return (uint8_t)((c64_via_out(via, 1) & 0x6f) | (d->write_protect ? 0 : 0x10) | (d->sync ? 0 : 0x80));
}

void c64_drive_init(c64_drive *d, const c64_cia *cia2) {
    gcr_tables();
    d->cia2 = cia2;
    d->cpu.read = open_read;
    d->cpu.write = open_write;
    d->cpu.ctx = d;
    d->cpu.rpage = d->rd;
    d->cpu.wpage = d->wr;
    for (int p = 0; p < 256; ++p) {
        d->rd[p] = NULL;
        d->wr[p] = NULL;
    }
    for (int p = 0; p < 0x18; ++p) {  // 2 KB of RAM, seen three times over
        d->rd[p] = d->ram + (p & 7) * 256;
        d->wr[p] = d->ram + (p & 7) * 256;
    }
    for (int p = 0x80; p < 0x100; ++p)  // the ROM, at $C000 and again at $8000
        d->rd[p] = d->rom + (p & 0x3f) * 256;
    d->via1.port_in = via1_port;
    d->via1.ctx = d;
    d->via2.port_in = via2_port;
    d->via2.ctx = d;
    d->half_track = 36;  // track 18
    d->phase_was = 0;
    c64_drive_reset(d);
}

void c64_drive_reset(c64_drive *d) {
    c64_via_reset(&d->via1);
    c64_via_reset(&d->via2);
    c64_cpu_reset(&d->cpu);
    d->atn_was = false;
}

void c64_drive_insert(c64_drive *d, uint8_t *d64, size_t size, bool write_protect) {
    d->d64 = d64;
    d->d64_size = size;
    d->write_protect = write_protect;
    d->changed = false;
    for (int t = 1; t <= C64_DRIVE_TRACKS; ++t) {
        d->track_dirty[t - 1] = false;
        if (d64)
            make_track(d, t);
        else
            d->track_len[t - 1] = 0;
    }
    d->head = 0;
}

bool c64_drive_flush(c64_drive *d) {
    bool any = false;
    for (int t = 1; t <= C64_DRIVE_TRACKS; ++t)
        if (d->track_dirty[t - 1]) {
            if (d->d64)
                read_back_track(d, t);
            d->track_dirty[t - 1] = false;
            any = true;
        }
    d->changed = false;
    return any;
}

int c64_drive_track(const c64_drive *d) {
    return (d->half_track & 1) ? 0 : d->half_track / 2;
}

bool c64_drive_led(const c64_drive *d) {
    return c64_via_out(&d->via2, 1) & 0x08;
}

bool c64_drive_motor(const c64_drive *d) {
    return c64_via_out(&d->via2, 1) & 0x04;
}

/// The disk turning under the head for n cycles: a byte every 26-32 cycles
/// (by the speed zone VIA 2 port B bits 5-6 choose). Each byte not in a sync
/// is ready for the CPU: latched in VIA 2 port A and, if CA2 (SOE) is high,
/// setting the 6502's overflow flag. In write mode (CB2 low) port A's
/// output is written instead.
static void FAST(spin)(c64_drive *d, unsigned n) {
    uint8_t pb = c64_via_out(&d->via2, 1);
    if (!(pb & 0x04))
        return;  // motor off
    int track = c64_drive_track(d);
    unsigned len = track ? d->track_len[track - 1] : 0;
    unsigned per_byte = 32 - ((pb >> 5) & 3) * 2;
    d->byte_clock += (uint16_t)n;
    while (d->byte_clock >= per_byte) {
        d->byte_clock -= (uint16_t)per_byte;
        if (!len) {
            d->sync = false;
            continue;
        }
        if (++d->head >= len)
            d->head = 0;
        uint8_t *t = track_gcr(d, track);
        uint8_t b;
        if (!c64_via_cb2(&d->via2)) {  // writing
            if (d->write_protect)
                continue;
            b = c64_via_out(&d->via2, 0);
            t[d->head] = b;
            d->track_dirty[track - 1] = true;
            d->changed = true;
            d->sync = false;
        } else {
            b = t[d->head];
            d->sync = b == 0xff && d->last_byte == 0xff;
            d->last_byte = b;
            if (d->sync)
                continue;
        }
        d->via2.ira = b;
        if (c64_via_ca2(&d->via2))
            d->cpu.p |= C64_V;  // BYTE READY on the 6502's SO pin
        c64_via_flag(&d->via2, VIA_CA1);
    }
}

/// The head stepper: VIA 2 port B bits 0-1 step through four phases; one
/// up moves the head in (a half-track), one down out.
static void step(c64_drive *d) {
    int phase = c64_via_out(&d->via2, 1) & 3;
    if (phase == ((d->phase_was + 1) & 3) && d->half_track < 2 * C64_DRIVE_TRACKS) {
        int was = c64_drive_track(d);
        ++d->half_track;
        int now = c64_drive_track(d);
        if (was && now)
            d->head = (uint16_t)(d->head * d->track_len[now - 1] / (d->track_len[was - 1] ? d->track_len[was - 1] : 1));
    } else if (phase == ((d->phase_was - 1) & 3) && d->half_track > 2) {
        --d->half_track;
    }
    d->phase_was = phase;
    int t = c64_drive_track(d);
    if (t && d->head >= d->track_len[t - 1])
        d->head = 0;
}

void c64_drive_sync(c64_drive *d, uint64_t c64_cycles) {
    d->cycles = d->via_at = (c64_cycles * RATIO_Q16) >> 16;
    d->wake_at = 0;
}

/// The VIAs' timers for a few cycles, the usual case (nothing underflows)
/// without a call.
static inline void via_tick(c64_via *v, unsigned n) {
    if (v->t1 > n && (v->t2 > n || (v->acr & 0x20))) {
        v->t1 -= (uint16_t)n;
        if (!(v->acr & 0x20))
            v->t2 -= (uint16_t)n;
    } else {
        c64_via_tick(v, n);
    }
}

/// The DOS's main loop, where it waits for the C64 or a job to do.
#define IDLE_FROM 0xebff
#define IDLE_TO   0xec9e

void FAST(c64_drive_run_to)(c64_drive *d, uint64_t c64_cycles) {
    uint64_t target = (c64_cycles * RATIO_Q16) >> 16;
    // Idle (in the main loop, the motor off, ATN not asserted): the 6502
    // would only go round the loop, so only time passes, and the VIAs'
    // timers are brought up to date when one is due to interrupt. Asserting
    // ATN or an interrupt wakes it.
    if (d->cpu.pc >= IDLE_FROM && d->cpu.pc < IDLE_TO && !(c64_via_out(&d->via2, 1) & 0x04) &&
        !(c64_cia_out(d->cia2, 0) & 0x08) && !d->atn_was) {
        if (target < d->wake_at) {
            d->cycles = target;
            return;
        }
        unsigned n = (unsigned)(target - d->via_at);
        c64_via_tick(&d->via1, n);
        c64_via_tick(&d->via2, n);
        d->cycles = d->via_at = target;
        d->cpu.irq = d->via1.irq || d->via2.irq;
        if (!d->cpu.irq || (d->cpu.p & C64_I)) {
            unsigned soon = 20000;  // look again at least this often
            for (int i = 0; i < 2; ++i) {
                const c64_via *v = i ? &d->via2 : &d->via1;
                if ((v->acr & 0x40) || v->t1_armed)
                    soon = v->t1 + 1u < soon ? v->t1 + 1u : soon;
                if (v->t2_armed && !(v->acr & 0x20))
                    soon = v->t2 + 1u < soon ? v->t2 + 1u : soon;
            }
            d->wake_at = target + soon;
            return;
        }
        d->cycles = target - 1;  // the interrupt is taken below
    } else if (d->via_at < d->cycles) {  // awake again: the VIAs catch up
        unsigned n = (unsigned)(d->cycles - d->via_at);
        c64_via_tick(&d->via1, n);
        c64_via_tick(&d->via2, n);
        d->via_at = d->cycles;
    }
    d->wake_at = 0;
    bool atn = c64_cia_out(d->cia2, 0) & 0x08;  // the C64 runs no further meanwhile
    if (atn != d->atn_was) {
        d->atn_was = atn;
        c64_via_set_ca1(&d->via1, atn);
    }
    while (d->cycles < target) {
        d->cpu.irq = d->via1.irq || d->via2.irq;
        unsigned n = (unsigned)c64_cpu_step(&d->cpu);
        d->cycles += n;
        via_tick(&d->via1, n);
        via_tick(&d->via2, n);
        d->via_at = d->cycles;
        uint8_t pb = c64_via_out(&d->via2, 1);
        if ((pb & 3) != d->phase_was)
            step(d);
        if (pb & 0x04)
            spin(d, n);
    }
}
