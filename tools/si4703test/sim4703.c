/**
 * @file sim4703.c
 * @brief Implementation of sim4703.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "sim4703.h"

#include <string.h>

bool sim_present = true;
int sim_resets;
int sim_bad_every;

static uint16_t R[16];               ///< the registers
static const sim_station_t *stations;  ///< what is on the air
static int nstations;                ///< how many
static int group_n;                  ///< RDS groups sent
static int stc_reads;                ///< reads before STC comes on

void sim_set_stations(const sim_station_t *s, int n) {
    stations = s;
    nstations = n;
}

const uint16_t *sim_regs(void) {
    return R;
}

/// The band's bottom and spacing, from SYSCONFIG2.
/// @param lo set to the bottom (kHz)
/// @param sp set to the spacing (kHz)
/// @param hi set to the top (kHz)
static void band(uint32_t *lo, uint32_t *sp, uint32_t *hi) {
    int b = (R[5] >> 6) & 3, s = (R[5] >> 4) & 3;
    *lo = b == 0 ? 87500 : 76000;
    *hi = b == 2 ? 90000 : 108000;
    *sp = s == 0 ? 200 : s == 1 ? 100 : 50;
}

/// The station at a frequency, or NULL.
/// @param khz the frequency
/// @return it
static const sim_station_t *at(uint32_t khz) {
    for (int i = 0; i < nstations; ++i)
        if (stations[i].khz == khz)
            return &stations[i];
    return NULL;
}

/// Sets STATUSRSSI for the channel tuned.
/// @param chan the channel
static void tuned_to(int chan) {
    uint32_t lo, sp, hi;
    band(&lo, &sp, &hi);
    R[11] = (uint16_t)(chan & 0x3ff);
    const sim_station_t *s = at(lo + chan * sp);
    R[10] = (uint16_t)((s ? s->rssi : 8) | (s && s->stereo ? 0x0100 : 0));
    stc_reads = 2;
    group_n = 0;
}

/// The next RDS group of the station tuned, into RDSA-D.
static void next_group(void) {
    uint32_t lo, sp, hi;
    band(&lo, &sp, &hi);
    const sim_station_t *s = at(lo + (R[11] & 0x3ff) * sp);
    if (!s || !s->ps || !(R[4] & 0x1000))
        return;
    int n = group_n++;
    uint16_t b = (uint16_t)(s->pty << 5);
    if (n % 2 == 0) {  // 0A: the name
        int seg = (n / 2) % 4;
        R[12] = s->pi;
        R[13] = (uint16_t)(b | seg);
        R[14] = 0;
        R[15] = (uint16_t)((uint8_t)s->ps[seg * 2] << 8 | (uint8_t)s->ps[seg * 2 + 1]);
    } else {  // 2A: the text, padded with spaces, ended by CR
        int seg = (n / 2) % 16;
        char t[65];
        memset(t, ' ', 64);
        int len = (int)strlen(s->rt);
        memcpy(t, s->rt, len);
        if (len < 64)
            t[len] = '\r';
        R[12] = s->pi;
        R[13] = (uint16_t)(0x2000 | b | seg);
        R[14] = (uint16_t)((uint8_t)t[seg * 4] << 8 | (uint8_t)t[seg * 4 + 1]);
        R[15] = (uint16_t)((uint8_t)t[seg * 4 + 2] << 8 | (uint8_t)t[seg * 4 + 3]);
    }
    R[10] |= 0x8000;  // RDSR
    R[11] &= 0x03ff;
    if (sim_bad_every && n % sim_bad_every == sim_bad_every - 1) {
        R[11] |= 3 << 10;  // BLERD uncorrectable
        R[15] ^= 0x4141;   // and the characters wrong
    }
}

/// The simulated read: registers 0x0A-0x0F then 0x00-0x09.
/// @param ctx unused
/// @param regs filled in
/// @return the chip answered
static bool sim_read(void *ctx, uint16_t regs[16]) {
    (void)ctx;
    if (!sim_present)
        return false;
    if (stc_reads && --stc_reads == 0 && ((R[3] & 0x8000) || (R[2] & 0x0100)))
        R[10] |= 0x4000;  // STC
    R[10] &= 0x7fff;
    if ((R[2] & 0x41) == 0x01 && !(R[3] & 0x8000) && !(R[2] & 0x0100))
        next_group();
    memcpy(regs, R, sizeof R);
    return true;
}

/// The simulated write: registers from 0x02.
/// @param ctx unused
/// @param from_02 the values
/// @param n how many
/// @return the chip answered
static bool sim_write(void *ctx, const uint16_t *from_02, int n) {
    (void)ctx;
    if (!sim_present)
        return false;
    uint16_t old2 = R[2], old3 = R[3];
    for (int i = 0; i < n && i < 6; ++i)  // 0x08 and 0x09 are read-only here
        R[2 + i] = from_02[i];
    uint32_t lo, sp, hi;
    band(&lo, &sp, &hi);
    if ((R[3] & 0x8000) && !(old3 & 0x8000))
        tuned_to(R[3] & 0x3ff);
    if (!(R[3] & 0x8000) && (old3 & 0x8000))
        R[10] &= (uint16_t)~0x4000;
    if ((R[2] & 0x0100) && !(old2 & 0x0100)) {  // seek
        bool up = R[2] & 0x0200;
        int chan = R[11] & 0x3ff, last = (int)((hi - lo) / sp);
        int th = R[5] >> 8;
        for (;;) {
            chan += up ? 1 : -1;
            if (chan < 0 || chan > last) {  // the band's end (SKMODE)
                chan = chan < 0 ? 0 : last;
                tuned_to(chan);
                R[10] |= 0x2000;
                break;
            }
            const sim_station_t *s = at(lo + chan * sp);
            if (s && s->rssi >= th) {
                tuned_to(chan);
                break;
            }
        }
    }
    if (!(R[2] & 0x0100) && (old2 & 0x0100))
        R[10] &= (uint16_t)~0x6000;
    return true;
}

/// The simulated reset: a powered-down chip, as after power-on.
/// @param ctx unused
static void sim_reset(void *ctx) {
    (void)ctx;
    ++sim_resets;
    memset(R, 0, sizeof R);
    R[0] = 0x1242;
    R[1] = 0x1253;
    R[7] = 0x0100;
}

/// The simulated delay: none.
/// @param ctx unused
/// @param ms how long
static void sim_delay(void *ctx, unsigned ms) {
    (void)ctx;
    (void)ms;
}

si4703_bus_t sim_bus(void) {
    return (si4703_bus_t){sim_read, sim_write, sim_reset, sim_delay, NULL};
}
