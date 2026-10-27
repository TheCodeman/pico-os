/**
 * @file si4703.c
 * @brief Implementation of si4703.h: the Si4703's registers, tuning, seeking and RDS decoding.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "si4703.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Registers.
enum {
    DEVICEID = 0x00,    ///< part number and manufacturer: 0x1242
    CHIPID = 0x01,      ///< revision and device
    POWERCFG = 0x02,    ///< mute, mono, seek, enable
    CHANNEL = 0x03,     ///< tune, channel
    SYSCONFIG1 = 0x04,  ///< RDS, de-emphasis
    SYSCONFIG2 = 0x05,  ///< seek threshold, band, spacing, volume
    SYSCONFIG3 = 0x06,  ///< volume range, seek SNR and count
    TEST1 = 0x07,       ///< crystal oscillator enable
    STATUSRSSI = 0x0A,  ///< RDS ready, tune/seek complete, stereo, RSSI
    READCHAN = 0x0B,    ///< RDS block errors, the channel tuned
    RDSA = 0x0C,        ///< RDS block A (D at 0x0F)
};

/// Register bits.
enum {
    PWR_DSMUTE = 0x8000,   ///< soft mute off
    PWR_DMUTE = 0x4000,    ///< mute off
    PWR_MONO = 0x2000,     ///< forced mono
    PWR_SKMODE = 0x0400,   ///< seeks stop at the band's end
    PWR_SEEKUP = 0x0200,   ///< seek upwards
    PWR_SEEK = 0x0100,     ///< seek
    PWR_DISABLE = 0x0040,  ///< power down
    PWR_ENABLE = 0x0001,   ///< power up
    CHAN_TUNE = 0x8000,    ///< tune
    SC1_RDS = 0x1000,      ///< RDS on
    SC1_DE = 0x0800,       ///< 50 us de-emphasis
    SC3_VOLEXT = 0x0100,   ///< volume 30 dB lower
    TEST1_XOSCEN = 0x8000, ///< crystal oscillator on
    ST_RDSR = 0x8000,      ///< an RDS group is ready
    ST_STC = 0x4000,       ///< tune or seek complete
    ST_SFBL = 0x2000,      ///< seek failed / band limit
    ST_STEREO = 0x0100,    ///< stereo
};

void si4703_defaults(si4703_config_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->band = SI4703_BAND_EU;
    cfg->spacing = 100;
    cfg->de50 = false;
    cfg->volume = 20;
    cfg->seek = SI4703_SEEK_NORMAL;
}

uint32_t si4703_band_bottom(uint8_t band) {
    return band == SI4703_BAND_EU ? 87500 : 76000;
}

uint32_t si4703_band_top(uint8_t band) {
    return band == SI4703_BAND_JP ? 90000 : 108000;
}

uint16_t si4703_channel(const si4703_config_t *cfg, uint32_t khz) {
    uint32_t lo = si4703_band_bottom(cfg->band), hi = si4703_band_top(cfg->band);
    if (khz < lo)
        khz = lo;
    if (khz > hi)
        khz = hi;
    uint32_t chan = (khz - lo + cfg->spacing / 2) / cfg->spacing, last = (hi - lo) / cfg->spacing;
    return (uint16_t)(chan > last ? last : chan);
}

uint32_t si4703_channel_khz(const si4703_config_t *cfg, uint16_t chan) {
    return si4703_band_bottom(cfg->band) + (uint32_t)chan * cfg->spacing;
}

/// Reads all the registers into dev->regs.
/// @param dev the chip
/// @return it answered
static bool read_regs(si4703_dev_t *dev) {
    return dev->bus.read(dev->bus.ctx, dev->regs);
}

/// Writes registers 0x02 to last from dev->regs.
/// @param dev the chip
/// @param last the last register written (0x02-0x07)
/// @return it answered
static bool write_regs(si4703_dev_t *dev, int last) {
    return dev->bus.write(dev->bus.ctx, dev->regs + POWERCFG, last - POWERCFG + 1);
}

/// Puts the settings into the register copies (not written yet).
/// @param dev the chip
/// @param cfg the settings
static void set_config_regs(si4703_dev_t *dev, const si4703_config_t *cfg) {
    static const uint8_t seekth[3] = {0x19, 0x19, 0x0c}, sksnr[3] = {7, 4, 4}, skcnt[3] = {15, 8, 8};
    uint8_t s = cfg->seek > SI4703_SEEK_MANY ? SI4703_SEEK_NORMAL : cfg->seek;
    uint16_t *r = dev->regs;
    r[POWERCFG] &= (uint16_t)~(PWR_DMUTE | PWR_MONO | PWR_DSMUTE);
    r[POWERCFG] |= PWR_DSMUTE | PWR_SKMODE;
    if (!cfg->mute && cfg->volume)
        r[POWERCFG] |= PWR_DMUTE;
    if (cfg->mono)
        r[POWERCFG] |= PWR_MONO;
    r[SYSCONFIG1] = (uint16_t)((r[SYSCONFIG1] & ~(SC1_RDS | SC1_DE)) | SC1_RDS | (cfg->de50 ? SC1_DE : 0));
    uint8_t space = cfg->spacing == 200 ? 0 : cfg->spacing == 50 ? 2 : 1;
    uint8_t vol = cfg->volume > SI4703_VOLUME_MAX ? SI4703_VOLUME_MAX : cfg->volume;
    bool volext = vol >= 1 && vol <= 15;  // the quiet range: VOLEXT on, 1-15
    uint8_t v4 = vol == 0 ? 0 : volext ? vol : vol - 15;
    r[SYSCONFIG2] = (uint16_t)(seekth[s] << 8 | (cfg->band & 3) << 6 | space << 4 | v4);
    r[SYSCONFIG3] = (uint16_t)((r[SYSCONFIG3] & 0xfe00) | (volext ? SC3_VOLEXT : 0) | sksnr[s] << 4 | skcnt[s]);
    dev->cfg = *cfg;
}

/// Whether the registers read are a Si4702/03's.
/// @param dev the chip
/// @return they are
static bool is_si470x(const si4703_dev_t *dev) {
    return dev->regs[DEVICEID] == 0x1242;
}

bool si4703_init(si4703_dev_t *dev, const si4703_bus_t *bus) {
    memset(dev, 0, sizeof *dev);
    dev->bus = *bus;
    si4703_defaults(&dev->cfg);
    if (dev->bus.reset)
        dev->bus.reset(dev->bus.ctx);
    return read_regs(dev) && is_si470x(dev);
}

bool si4703_attach(si4703_dev_t *dev, const si4703_bus_t *bus) {
    memset(dev, 0, sizeof *dev);
    dev->bus = *bus;
    si4703_defaults(&dev->cfg);
    if (!read_regs(dev) || !is_si470x(dev))
        return false;
    dev->on = (dev->regs[POWERCFG] & (PWR_ENABLE | PWR_DISABLE)) == PWR_ENABLE &&
              (dev->regs[TEST1] & TEST1_XOSCEN);
    return dev->on;
}

bool si4703_power_up(si4703_dev_t *dev, const si4703_config_t *cfg) {
    // AN230: the crystal first (500 ms to settle), then the receiver (110 ms).
    dev->regs[TEST1] = 0x8100;
    if (!write_regs(dev, TEST1))
        return false;
    dev->bus.delay_ms(dev->bus.ctx, 500);
    set_config_regs(dev, cfg);
    dev->regs[POWERCFG] = (uint16_t)((dev->regs[POWERCFG] & ~PWR_DISABLE) | PWR_ENABLE);
    if (!write_regs(dev, SYSCONFIG3))
        return false;
    dev->bus.delay_ms(dev->bus.ctx, 110);
    dev->on = read_regs(dev);
    return dev->on;
}

bool si4703_power_down(si4703_dev_t *dev) {
    dev->regs[SYSCONFIG1] &= (uint16_t)~SC1_RDS;
    dev->regs[POWERCFG] = (uint16_t)((dev->regs[POWERCFG] & ~PWR_SEEK) | PWR_ENABLE | PWR_DISABLE);
    dev->on = false;
    return write_regs(dev, SYSCONFIG1);
}

bool si4703_configure(si4703_dev_t *dev, const si4703_config_t *cfg) {
    set_config_regs(dev, cfg);
    return write_regs(dev, SYSCONFIG3);
}

/// Waits for STC to be set (or cleared).
/// @param dev the chip
/// @param set wait for it set (else cleared)
/// @param tries reads to try, a millisecond apart
/// @return it happened
static bool wait_stc(si4703_dev_t *dev, bool set, int tries) {
    for (int i = 0; i < tries; ++i) {
        if (!read_regs(dev))
            return false;
        if (!!(dev->regs[STATUSRSSI] & ST_STC) == set)
            return true;
        dev->bus.delay_ms(dev->bus.ctx, 1);
    }
    return false;
}

bool si4703_tune(si4703_dev_t *dev, uint32_t khz) {
    if (dev->seeking)
        si4703_seek_stop(dev);
    dev->regs[CHANNEL] = (uint16_t)(CHAN_TUNE | si4703_channel(&dev->cfg, khz));
    if (!write_regs(dev, CHANNEL))
        return false;
    bool ok = wait_stc(dev, true, 250);
    dev->regs[CHANNEL] &= (uint16_t)~CHAN_TUNE;
    if (!write_regs(dev, CHANNEL))
        return false;
    return wait_stc(dev, false, 50) && ok;
}

bool si4703_seek_start(si4703_dev_t *dev, bool up) {
    uint16_t p = dev->regs[POWERCFG] & (uint16_t)~(PWR_SEEK | PWR_SEEKUP);
    dev->regs[POWERCFG] = (uint16_t)(p | PWR_SEEK | PWR_SKMODE | (up ? PWR_SEEKUP : 0));
    dev->seeking = true;
    return write_regs(dev, POWERCFG);
}

bool si4703_seek_poll(si4703_dev_t *dev, bool *done, bool *found) {
    *done = false;
    *found = false;
    if (!dev->seeking) {
        *done = true;
        return true;
    }
    if (!read_regs(dev))
        return false;
    if (!(dev->regs[STATUSRSSI] & ST_STC))
        return true;
    *done = true;
    *found = !(dev->regs[STATUSRSSI] & ST_SFBL);
    dev->seeking = false;
    dev->regs[POWERCFG] &= (uint16_t)~PWR_SEEK;
    if (!write_regs(dev, POWERCFG))
        return false;
    return wait_stc(dev, false, 50);
}

bool si4703_seek_stop(si4703_dev_t *dev) {
    dev->seeking = false;
    dev->regs[POWERCFG] &= (uint16_t)~PWR_SEEK;
    return write_regs(dev, POWERCFG) && wait_stc(dev, false, 50);
}

/// The status from the registers last read.
/// @param dev the chip
/// @param st filled in
static void status_of(const si4703_dev_t *dev, si4703_status_t *st) {
    uint16_t s = dev->regs[STATUSRSSI];
    st->khz = si4703_channel_khz(&dev->cfg, dev->regs[READCHAN] & 0x3ff);
    st->rssi = s & 0xff;
    st->stereo = s & ST_STEREO;
    st->tuned = s & ST_STC;
    st->band_limit = s & ST_SFBL;
}

bool si4703_status(si4703_dev_t *dev, si4703_status_t *st) {
    if (!read_regs(dev))
        return false;
    status_of(dev, st);
    return true;
}

void si4703_rds_clear(si4703_rds_t *rds) {
    memset(rds, 0, sizeof *rds);
    memset(rds->ps_new, ' ', sizeof rds->ps_new);
    memset(rds->rt_new, ' ', sizeof rds->rt_new);
    rds->rt_ab = -1;
}

/// RDS characters made printable (the RDS set's accented letters as their
/// plain ones would need a table: they show as '?').
/// @param c the character
/// @return it, or '?'
static char rds_char(uint8_t c) {
    return c >= 0x20 && c < 0x7f ? (char)c : c == 0x0d ? '\r' : '?';
}

/// Copies the radio text received so far into rds->rt once its segments up
/// to the end (a carriage return, or all of them) have arrived.
/// @param rds the RDS state
/// @param segs segments in a full text (16 for 2A, 16 of 2 characters for 2B)
/// @param per characters a segment
static void rt_update(si4703_rds_t *rds, int segs, int per) {
    int n = 0;
    while (n < segs && (rds->rt_seen & (1u << n)))
        ++n;
    int len = n * per;
    char *cr = memchr(rds->rt_new, '\r', len);
    if (!cr && n < segs)
        return;  // not complete yet
    if (cr)
        len = (int)(cr - rds->rt_new);
    while (len && rds->rt_new[len - 1] == ' ')
        --len;
    memcpy(rds->rt, rds->rt_new, len);
    rds->rt[len] = 0;
}

void si4703_rds_group(si4703_rds_t *rds, const uint16_t blocks[4]) {
    uint16_t b = blocks[1], c = blocks[2], d = blocks[3];
    int type = b >> 12;
    bool version_b = b & 0x0800;
    rds->groups++;
    rds->pi = blocks[0];
    rds->pty = (b >> 5) & 0x1f;
    if (type == 0) {
        // The station's name, two characters a group, each pair taken once
        // it has come the same twice.
        int seg = b & 3;
        char x = rds_char(d >> 8), y = rds_char(d & 0xff);
        char *at = rds->ps_new + seg * 2;
        if (at[0] == x && at[1] == y) {
            rds->ps_seen |= (uint8_t)(1 << seg);
        } else {
            at[0] = x;
            at[1] = y;
            rds->ps_seen &= (uint8_t)~(1 << seg);
        }
        if (rds->ps_seen == 0x0f) {
            memcpy(rds->ps, rds->ps_new, 8);
            rds->ps[8] = 0;
        }
    } else if (type == 2) {
        int ab = (b >> 4) & 1, seg = b & 15;
        if (rds->rt_ab != ab) {  // a new text
            rds->rt_ab = (int8_t)ab;
            memset(rds->rt_new, ' ', sizeof rds->rt_new);
            rds->rt_seen = 0;
        }
        if (!version_b) {
            char *at = rds->rt_new + seg * 4;
            at[0] = rds_char(c >> 8);
            at[1] = rds_char(c & 0xff);
            at[2] = rds_char(d >> 8);
            at[3] = rds_char(d & 0xff);
            rds->rt_seen |= (uint16_t)(1u << seg);
            rt_update(rds, 16, 4);
        } else {
            char *at = rds->rt_new + seg * 2;
            at[0] = rds_char(d >> 8);
            at[1] = rds_char(d & 0xff);
            rds->rt_seen |= (uint16_t)(1u << seg);
            rt_update(rds, 16, 2);
        }
    }
}

bool si4703_rds_poll(si4703_dev_t *dev, si4703_rds_t *rds, si4703_status_t *st) {
    if (!read_regs(dev))
        return false;
    if (st)
        status_of(dev, st);
    uint16_t s = dev->regs[STATUSRSSI], r = dev->regs[READCHAN];
    if (!(s & ST_RDSR))
        return true;
    // Block errors 0-2 were corrected; 3 couldn't be. B decides what the
    // group is and D carries the characters, so both must be good.
    int blera = (s >> 9) & 3, blerb = (r >> 14) & 3, blerc = (r >> 12) & 3, blerd = (r >> 10) & 3;
    uint16_t blocks[4] = {dev->regs[RDSA], dev->regs[RDSA + 1], dev->regs[RDSA + 2], dev->regs[RDSA + 3]};
    // The ready flag stays up about 40 ms: a group read twice is the same
    // four blocks, so it counts once.
    if (!memcmp(rds->last, blocks, sizeof rds->last))
        return true;
    memcpy(rds->last, blocks, sizeof rds->last);
    if (blerb > 2 || blerd > 2)
        return true;
    if ((blocks[1] >> 12) == 2 && !(blocks[1] & 0x0800) && blerc > 2)
        return true;
    if (blera > 2)
        blocks[0] = rds->pi;  // keep the PI we had
    si4703_rds_group(rds, blocks);
    return true;
}

const char *si4703_pty_name(uint8_t pty, bool rbds) {
    static const char *const rds_names[32] = {
        "",           "News",          "Current Affairs", "Information",   "Sport",       "Education",
        "Drama",      "Culture",       "Science",         "Varied",        "Pop Music",   "Rock Music",
        "Easy Listening", "Light Classical", "Serious Classical", "Other Music", "Weather", "Finance",
        "Children's", "Social Affairs", "Religion",       "Phone In",      "Travel",      "Leisure",
        "Jazz Music", "Country Music", "National Music",  "Oldies Music",  "Folk Music",  "Documentary",
        "Alarm Test", "Alarm",
    };
    static const char *const rbds_names[32] = {
        "",           "News",          "Information",     "Sports",        "Talk",        "Rock",
        "Classic Rock", "Adult Hits",  "Soft Rock",       "Top 40",        "Country",     "Oldies",
        "Soft",       "Nostalgia",     "Jazz",            "Classical",     "R&B",         "Soft R&B",
        "Language",   "Religious Music", "Religious Talk", "Personality",  "Public",      "College",
        "",           "",              "",                "",              "",            "Weather",
        "Emergency Test", "Emergency",
    };
    return (rbds ? rbds_names : rds_names)[pty & 31];
}

char *si4703_freq_text(uint32_t khz, char *out, int size) {
    snprintf(out, size, "%lu.%02lu", (unsigned long)(khz / 1000), (unsigned long)(khz % 1000 / 10));
    return out;
}

bool si4703_parse_freq(const char *s, uint32_t *khz) {
    while (*s == ' ')
        ++s;
    char *end;
    double v = strtod(s, &end);
    if (end == s)
        return false;
    while (*end == ' ')
        ++end;
    if ((end[0] == 'M' || end[0] == 'm') && (end[1] == 'H' || end[1] == 'h'))
        end += 3;  // "MHz"
    while (*end == ' ')
        ++end;
    if (*end)
        return false;
    if (v >= 60 && v <= 120)
        v *= 1000;  // MHz
    if (v < 60000 || v > 120000)
        return false;
    *khz = (uint32_t)(v + 0.5);
    return true;
}
