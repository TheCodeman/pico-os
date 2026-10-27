/**
 * @file si4703.h
 * @brief A Si4703 FM receiver: power-up, tuning, seeking, a band scan and RDS (station name and radio text).
 *
 * Plain C, so it can be tested on a PC against a simulated chip: the chip is
 * reached through callbacks (si4703_bus_t), which si4703_pico.c gives the
 * board's I2C and reset pin.
 *
 * The chip (Silicon Labs Si4702/03, AN230): sixteen 16-bit registers. A read
 * always starts at register 0x0A and wraps round through 0x0F to 0x00; a
 * write always starts at 0x02. Its I2C ("2-wire") mode is chosen as RST
 * rises with SDIO held low, so the reset pin is part of the bus.
 *
 * Frequencies are in kHz. Bands: 87.5-108 MHz (the US and Europe), 76-108
 * MHz (Japan wide band) and 76-90 MHz (Japan); channel spacing 200, 100 or
 * 50 kHz. The audio comes out of the module's own headphone socket (whose
 * cable is the aerial): it doesn't pass through the board.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SI4703_ADDR   0x10  ///< its I2C address
#define SI4703_VOLUME_MAX 30  ///< volume steps: 1-15, and 16-30 with VOLEXT's quieter range below (0 mute)

/// The bands.
enum {
    SI4703_BAND_EU,    ///< 87.5-108 MHz (the US and Europe)
    SI4703_BAND_WIDE,  ///< 76-108 MHz (Japan wide band)
    SI4703_BAND_JP,    ///< 76-90 MHz (Japan)
};

/// Seek sensitivity (AN230's settings).
enum {
    SI4703_SEEK_FEW,     ///< strong stations only
    SI4703_SEEK_NORMAL,  ///< AN230's recommended settings
    SI4703_SEEK_MANY,    ///< weak stations too
};

/// The receiver's settings.
typedef struct {
    uint8_t band;       ///< SI4703_BAND_*
    uint8_t spacing;    ///< channel spacing in kHz: 200, 100 or 50
    bool de50;          ///< 50 us de-emphasis (Europe, Japan), else 75 us (the US)
    uint8_t volume;     ///< 0 (silent) to SI4703_VOLUME_MAX
    bool mute;          ///< muted
    bool mono;          ///< forced mono
    uint8_t seek;       ///< SI4703_SEEK_*
} si4703_config_t;

/// What a status read found.
typedef struct {
    uint32_t khz;       ///< the frequency tuned
    uint8_t rssi;       ///< received signal strength, dBuV (0-75)
    bool stereo;        ///< stereo pilot found
    bool tuned;         ///< the last tune or seek finished (STC)
    bool band_limit;    ///< the last seek failed or hit the band's end (SF/BL)
} si4703_status_t;

/// RDS: what the station sends about itself.
typedef struct {
    uint16_t pi;        ///< programme identification, 0 none yet
    uint8_t pty;        ///< programme type (0-31)
    char ps[9];         ///< programme service name: 8 characters
    char rt[65];        ///< radio text: up to 64 characters
    char ps_new[8];     ///< the name being received
    uint8_t ps_seen;    ///< bit n: segment n of ps_new arrived twice the same
    char rt_new[64];    ///< the text being received
    uint16_t rt_seen;   ///< bit n: segment n of rt_new arrived
    int8_t rt_ab;       ///< the text A/B flag (a change clears it), -1 none yet
    uint32_t groups;    ///< good groups received
    uint16_t last[4];   ///< the last group read (the ready flag stays up for 40 ms)
} si4703_rds_t;

/// How the driver reaches the chip.
typedef struct {
    /// Reads all 16 registers (the chip sends 0x0A..0x0F, then 0x00..0x09).
    bool (*read)(void *ctx, uint16_t regs[16]);
    /// Writes registers 0x02 up to 0x02 + n - 1.
    bool (*write)(void *ctx, const uint16_t *from_02, int n);
    /// Resets the chip into 2-wire (I2C) mode: SDIO low while RST rises.
    void (*reset)(void *ctx);
    /// Waits ms milliseconds.
    void (*delay_ms)(void *ctx, unsigned ms);
    void *ctx;  ///< passed to all of them
} si4703_bus_t;

/// A chip.
typedef struct {
    si4703_bus_t bus;   ///< how to reach it
    uint16_t regs[16];  ///< its registers as last read or written
    si4703_config_t cfg;  ///< the settings it has
    bool on;            ///< powered up
    bool seeking;       ///< a seek is under way (si4703_seek_poll)
} si4703_dev_t;

/// The settings at first: 87.5-108 MHz, 100 kHz steps, 75 us, volume 20, normal seek.
/// @param cfg filled in
void si4703_defaults(si4703_config_t *cfg);

/// A band's lowest frequency.
/// @param band SI4703_BAND_*
/// @return kHz
uint32_t si4703_band_bottom(uint8_t band);

/// A band's highest frequency.
/// @param band SI4703_BAND_*
/// @return kHz
uint32_t si4703_band_top(uint8_t band);

/// The channel number of a frequency (rounded to the spacing, kept in the band).
/// @param cfg the settings
/// @param khz the frequency
/// @return the channel (0-1023)
uint16_t si4703_channel(const si4703_config_t *cfg, uint32_t khz);

/// A channel's frequency.
/// @param cfg the settings
/// @param chan the channel
/// @return kHz
uint32_t si4703_channel_khz(const si4703_config_t *cfg, uint16_t chan);

/// Starts a chip: resets it into I2C mode and checks that it is a Si4702/03.
/// @param dev the chip
/// @param bus how to reach it
/// @return a Si4702/03 answered
bool si4703_init(si4703_dev_t *dev, const si4703_bus_t *bus);

/// Whether the chip still answers, and already powered up (after the panel
/// opens again with the radio still playing).
/// @param dev the chip
/// @param bus how to reach it
/// @return it answers and is powered up
bool si4703_attach(si4703_dev_t *dev, const si4703_bus_t *bus);

/// Powers the chip up (crystal, then the receiver) with the settings.
/// @param dev the chip
/// @param cfg the settings
/// @return the chip answered
bool si4703_power_up(si4703_dev_t *dev, const si4703_config_t *cfg);

/// Powers the receiver down (the chip keeps its registers).
/// @param dev the chip
/// @return the chip answered
bool si4703_power_down(si4703_dev_t *dev);

/// Changes the settings (band, spacing, volume and the like) without
/// retuning; after a band or spacing change the caller tunes again.
/// @param dev the chip
/// @param cfg the settings
/// @return the chip answered
bool si4703_configure(si4703_dev_t *dev, const si4703_config_t *cfg);

/// Tunes to a frequency and waits for it (about 60 ms).
/// @param dev the chip
/// @param khz the frequency
/// @return the chip answered and the tune finished
bool si4703_tune(si4703_dev_t *dev, uint32_t khz);

/// Starts a seek to the next station up or down, stopping at the band's end.
/// @param dev the chip
/// @param up upwards
/// @return the chip answered
bool si4703_seek_start(si4703_dev_t *dev, bool up);

/// Checks on a seek. When it is over, *done is set and *found says whether
/// it stopped on a station (else at the band's end); the frequency reached
/// is in the status.
/// @param dev the chip
/// @param done set when the seek is over
/// @param found set to it having found a station
/// @return the chip answered
bool si4703_seek_poll(si4703_dev_t *dev, bool *done, bool *found);

/// Stops a seek where it is.
/// @param dev the chip
/// @return the chip answered
bool si4703_seek_stop(si4703_dev_t *dev);

/// Reads the status: frequency, signal, stereo.
/// @param dev the chip
/// @param st filled in
/// @return the chip answered
bool si4703_status(si4703_dev_t *dev, si4703_status_t *st);

/// Clears what RDS has received (after tuning).
/// @param rds the RDS state
void si4703_rds_clear(si4703_rds_t *rds);

/// Feeds one RDS group (blocks A-D) into the RDS state.
/// @param rds the RDS state
/// @param blocks the four blocks
void si4703_rds_group(si4703_rds_t *rds, const uint16_t blocks[4]);

/// Reads the chip and, if an RDS group is ready (and without errors in the
/// blocks that matter), feeds it in.
/// @param dev the chip
/// @param rds the RDS state
/// @param st set to the status read, or NULL
/// @return the chip answered
bool si4703_rds_poll(si4703_dev_t *dev, si4703_rds_t *rds, si4703_status_t *st);

/// A programme type's name: RDS's (Europe) or RBDS's (North America).
/// @param pty the type (0-31)
/// @param rbds North America's names
/// @return its name
const char *si4703_pty_name(uint8_t pty, bool rbds);

/// A frequency as "101.10" (MHz, two decimals).
/// @param khz the frequency
/// @param out the text
/// @param size room in out
/// @return out
char *si4703_freq_text(uint32_t khz, char *out, int size);

/// Parses a frequency in MHz ("101.1", "101.10") or kHz ("101100").
/// @param s the text
/// @param khz set to the frequency
/// @return it parsed
bool si4703_parse_freq(const char *s, uint32_t *khz);
