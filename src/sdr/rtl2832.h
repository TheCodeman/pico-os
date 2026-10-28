/**
 * @file rtl2832.h
 * @brief An RTL2832U dongle's demodulator chip in its SDR mode and its Rafael Micro R820T/R820T2/R828D tuner: setting it up, the sample rate, tuning, gain and frequency correction.
 *
 * Every call here runs on core 0 and talks to the dongle with control
 * requests (rtl_usb_control), so rtl_usb_job must be running on core 1.
 * The RTL-SDR Blog V4 (an R828D with an HF upconverter and three inputs) is
 * recognised from its USB strings and handled: below 28.8 MHz it tunes
 * through the upconverter. Other R820T dongles (the Blog V3) tune below
 * 24 MHz by sampling the antenna input directly (the Q branch).
 *
 * The register settings follow the open-source rtl-sdr driver's (osmocom
 * librtlsdr and the RTL-SDR Blog fork), which documents how these chips
 * are set up; the code is written anew.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Tuner chips.
typedef enum {
    RTL_TUNER_NONE,   ///< none answered
    RTL_TUNER_R820T,  ///< Rafael Micro R820T or R820T2 (most dongles, the Blog V3)
    RTL_TUNER_R828D,  ///< Rafael Micro R828D (the Blog V4)
    RTL_TUNER_E4000,  ///< Elonics E4000 (not supported)
    RTL_TUNER_FC0012, ///< Fitipower FC0012 (not supported)
    RTL_TUNER_FC0013, ///< Fitipower FC0013 (not supported)
} rtl_tuner_t;

#define RTL_XTAL_HZ 28800000u  ///< the dongle's crystal
#define RTL_GAIN_AUTO (-1)     ///< rtl_set_gain: the tuner's own AGC

/// The R82xx tuner's state: its shadow copy of registers 5-31 and settings.
typedef struct {
    uint8_t i2c;          ///< its I2C address (8-bit form)
    bool r828d;           ///< an R828D (else R820T)
    uint32_t xtal;        ///< its crystal, after the correction (Hz)
    uint8_t regs[27];     ///< registers 5 to 31 as last written
    uint32_t if_hz;       ///< its IF (the low-pass filter's middle)
    int input;            ///< the input selected (Blog V4: band; R828D: air/cable)
    bool locked;          ///< the PLL locked on the last tune
} r82xx_t;

/// A dongle.
typedef struct {
    rtl_tuner_t tuner;      ///< its tuner
    bool blog_v4;           ///< an RTL-SDR Blog V4
    char maker[32];         ///< its USB manufacturer string
    char product[32];       ///< and product string
    uint32_t rate;          ///< the sample rate set (Hz, exact)
    uint32_t freq;          ///< the frequency tuned (Hz)
    int ppm;                ///< the crystal correction
    int gain;               ///< tenths of a dB, or RTL_GAIN_AUTO
    bool direct;            ///< sampling the antenna directly (below 24 MHz)
    bool bias_tee;          ///< the bias tee is on
    bool failed;            ///< a request failed since the last check
    r82xx_t r82;            ///< the tuner's state
} rtl_dev_t;

/// Finds the tuner and sets everything up (no stream yet).
/// @param d the dongle's state (filled in)
/// @param err set to why it failed
/// @param errlen its room
/// @return false if the dongle didn't answer or its tuner isn't supported
bool rtl_open(rtl_dev_t *d, char *err, int errlen);
/// Powers the demodulator and tuner down.
/// @param d the dongle
void rtl_close(rtl_dev_t *d);
/// The sample rate: 225001-300000 or 900001-3200000 Hz (only the first is
/// useful over full-speed USB).
/// @param d the dongle
/// @param hz the rate
/// @return false if it can't be set
bool rtl_set_rate(rtl_dev_t *d, uint32_t hz);
/// Tunes (the middle of the samples' band).
/// @param d the dongle
/// @param hz the frequency
/// @return false if the tuner wouldn't lock or a request failed
bool rtl_set_freq(rtl_dev_t *d, uint32_t hz);
/// The tuner's gain.
/// @param d the dongle
/// @param tenth_db tenths of a dB (the nearest step is used), or RTL_GAIN_AUTO
/// @return false if a request failed
bool rtl_set_gain(rtl_dev_t *d, int tenth_db);
/// The crystal's correction, in parts per million (-200..200).
/// @param d the dongle
/// @param ppm the correction
/// @return false if a request failed
bool rtl_set_ppm(rtl_dev_t *d, int ppm);
/// The bias tee (power for an LNA up the antenna cable), on GPIO 0.
/// @param d the dongle
/// @param on power on
/// @return false if a request failed
bool rtl_set_bias_tee(rtl_dev_t *d, bool on);
/// Empties the dongle's FIFO and starts it sending (call just before
/// rtl_usb_stream_start), or stops it.
/// @param d the dongle
/// @param on start (else stop)
/// @return false if a request failed
bool rtl_stream(rtl_dev_t *d, bool on);
/// The tuner's gain steps, in tenths of a dB.
/// @param n set to how many
/// @return the steps, lowest first
const int16_t *rtl_gains(int *n);
/// A tuner's name.
/// @param t the tuner
/// @return its name
const char *rtl_tuner_name(rtl_tuner_t t);
