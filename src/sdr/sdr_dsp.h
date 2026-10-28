/**
 * @file sdr_dsp.h
 * @brief The SDR's signal processing: the dongle's 8-bit IQ samples to 48 kHz audio (wide FM, narrow FM or AM), the channel's level and squelch, and the spectrum.
 *
 * The input rate is a whole multiple of 48 kHz (240 or 288 kS/s). Wide FM
 * is demodulated at the input rate (the whole band is the channel), then
 * low-passed to 15 kHz and decimated to 48 kHz and de-emphasised. Narrow FM
 * and AM shift the channel to the middle, low-pass and decimate it to 48 kHz,
 * then demodulate: AM's envelope divided by its own average is both the AGC
 * and the DC block.
 *
 * Plain C with floats, so the PC tests run the same code.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DSP_AUDIO_RATE 48000  ///< the audio's rate
#define DSP_CH_TAPS    64     ///< the narrow channel filter's taps
#define DSP_AF_TAPS    128    ///< wide FM's audio filter's taps
#define DSP_FFT_N      1024   ///< the spectrum's points

/// Demodulators.
enum { DSP_WFM, DSP_NFM, DSP_AM, DSP_MODES };

/// The state (about 3 KB; SRAM for speed).
typedef struct {
    int mode;                ///< DSP_*
    uint32_t rate;           ///< the input rate
    int decim;               ///< rate / 48 kHz
    int phase;               ///< input samples since the last output
    float lut[256];          ///< a sample byte to -1..1
    float ch_taps[DSP_CH_TAPS];      ///< the channel filter
    float af_taps[DSP_AF_TAPS];      ///< the wide FM audio filter
    float hi[2 * DSP_CH_TAPS];       ///< the channel filter's history: I
    float hq[2 * DSP_CH_TAPS];       ///< and Q
    float ha[2 * DSP_AF_TAPS];       ///< the audio filter's history
    int hpos;                ///< where the next sample goes in hi/hq
    int apos;                ///< and in ha
    float nco_re, nco_im;    ///< the channel shift's phasor
    float rot_re, rot_im;    ///< its step per sample
    int32_t offset_hz;       ///< the channel's distance from the middle
    float pi, pq;            ///< the last sample (FM's discriminator)
    float de;                ///< the de-emphasis filter's state
    float de_a;              ///< and its coefficient
    float env_avg;           ///< AM: the envelope's average
    float power;             ///< the channel's power, averaged
    float volume;            ///< 0..1
    float squelch_db;        ///< the squelch level (dBFS); -200: off
    bool sq_open;            ///< the squelch is open
    float sq_gain;           ///< the audio's gain, ramped as it opens and closes
    float fm_scale;          ///< the discriminator's radians to audio
} dsp_t;

/// Sets it up.
/// @param s the state
/// @param rate the input rate (a multiple of 48000)
/// @param mode DSP_*
void dsp_init(dsp_t *s, uint32_t rate, int mode);
/// Changes the demodulator (its filters are made again).
/// @param s the state
/// @param mode DSP_*
void dsp_set_mode(dsp_t *s, int mode);
/// The channel's distance from the middle of the samples' band.
/// @param s the state
/// @param hz the offset (+: above)
void dsp_set_offset(dsp_t *s, int32_t hz);
/// The de-emphasis time constant for wide FM.
/// @param s the state
/// @param us 50 (Europe) or 75 (the Americas); 0: none
void dsp_set_deemph(dsp_t *s, int us);
/// Demodulates a buffer of IQ pairs.
/// @param s the state
/// @param iq the bytes: I, Q, I, Q... (unsigned, 127.5 is zero)
/// @param pairs how many pairs
/// @param out the audio (one 48 kHz sample for every `decim` pairs)
/// @param max out's room
/// @return the audio samples made
int dsp_process(dsp_t *s, const uint8_t *iq, int pairs, int16_t *out, int max);
/// The channel's level now, in dB below full scale.
/// @param s the state
/// @return dBFS
float dsp_level_db(const dsp_t *s);

/// The spectrum: DSP_FFT_N points of a Hann-windowed FFT of the samples.
typedef struct {
    float re[DSP_FFT_N], im[DSP_FFT_N];  ///< the samples, then the FFT
    float win[DSP_FFT_N];                ///< the window
    float db[DSP_FFT_N];                 ///< the result, smoothed: index 0 is -rate/2
    int fill;                            ///< samples gathered so far
    bool fresh;                          ///< db is new since the last look
} dsp_spec_t;

/// Sets the spectrum up.
/// @param sp it
void dsp_spec_init(dsp_spec_t *sp);
/// Takes samples until it has DSP_FFT_N, then computes the spectrum (and
/// takes no more until dsp_spec_again).
/// @param sp it
/// @param iq the bytes
/// @param pairs how many pairs
void dsp_spec_feed(dsp_spec_t *sp, const uint8_t *iq, int pairs);
/// Starts gathering the next one.
/// @param sp it
void dsp_spec_again(dsp_spec_t *sp);
