/**
 * @file dsptest.c
 * @brief The SDR's signal processing on a PC: makes 8-bit IQ samples of wide FM, narrow FM and AM signals carrying a 1 kHz tone (with noise and a station beside them), demodulates them, and checks the tone comes out clean, the squelch, the level and the spectrum.
 *
 *     cc -O2 -I../../src/sdr dsptest.c ../../src/sdr/sdr_dsp.c -lm && ./a.out
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdr_dsp.h"

#define RATE 240000  ///< the input rate tested first
#define N    (RATE / 2)  ///< half a second of samples

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            ++failures;                             \
            printf("FAIL dsptest.c:%d: ", __LINE__); \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

/// A uniform random number, -1..1.
/// @return it
static double rnd(void) {
    return 2.0 * rand() / RAND_MAX - 1.0;
}

/// Makes the samples: a signal at `off` Hz from the middle, modulated by a
/// 1 kHz tone, a second station at `off2`, and noise.
/// @param iq the bytes (2 * n)
/// @param n the pairs
/// @param rate the rate
/// @param mode DSP_*
/// @param off where the signal is
/// @param amp its amplitude (1 is full scale)
/// @param off2 where the other station is
static void make(uint8_t *iq, int n, int rate, int mode, double off, double amp, double off2) {
    double ph = 0, ph2 = 0, dev = mode == DSP_WFM ? 75000 * 0.5 : 5000 * 0.5;
    for (int k = 0; k < n; ++k) {
        double t = (double)k / rate, tone = sin(2 * M_PI * 1000 * t);
        double i, q;
        if (mode == DSP_AM) {
            double e = amp * (1 + 0.5 * tone);
            i = e * cos(2 * M_PI * off * t) * 0.6;
            q = e * sin(2 * M_PI * off * t) * 0.6;
        } else {
            ph += 2 * M_PI * (off + dev * tone) / rate;
            i = amp * cos(ph), q = amp * sin(ph);
        }
        ph2 += 2 * M_PI * (off2 + 3000 * sin(2 * M_PI * 400 * t)) / rate;
        i += 0.2 * cos(ph2) + 0.02 * rnd();
        q += 0.2 * sin(ph2) + 0.02 * rnd();
        int a = (int)lrint(127.4 + 127 * i * 0.7), b = (int)lrint(127.4 + 127 * q * 0.7);
        iq[2 * k] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
        iq[2 * k + 1] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
}

/// How much of the audio is the 1 kHz tone: its power over the rest's, in
/// dB (from the last half of the audio, after the filters settle).
/// @param a the audio
/// @param n its samples
/// @param rms set to its RMS (of 32768)
/// @return the ratio in dB
static double tone_snr(const int16_t *a, int n, double *rms) {
    int from = n / 2, m = n - from;
    double c = 0, s = 0, tot = 0, mean = 0;
    for (int k = from; k < n; ++k)
        mean += a[k];
    mean /= m;
    for (int k = from; k < n; ++k) {
        double v = a[k] - mean, w = 2 * M_PI * 1000 * k / 48000.0;
        c += v * cos(w), s += v * sin(w), tot += v * v;
    }
    double tone = 2 * (c * c + s * s) / m;  // the tone's energy
    *rms = sqrt(tot / m) / 32768;
    double rest = tot - tone;
    return 10 * log10(tone / (rest > 1e-9 ? rest : 1e-9));
}

/// Demodulates one case and checks it.
/// @param name what it is
/// @param rate the rate
/// @param mode DSP_*
/// @param off where the signal is from the middle
/// @param off2 the other station
static void run(const char *name, int rate, int mode, double off, double off2) {
    int n = rate / 2;
    uint8_t *iq = malloc(2 * n);
    int16_t *au = malloc(sizeof(int16_t) * n);
    make(iq, n, rate, mode, off, 1.0, off2);
    static dsp_t s;
    dsp_init(&s, rate, mode);
    s.volume = 1.0f;
    dsp_set_offset(&s, (int32_t)off);
    int got = 0;
    for (int k = 0; k < n; k += 1024) {  // the dongle's buffers
        int m = n - k < 1024 ? n - k : 1024;
        got += dsp_process(&s, iq + 2 * k, m, au + got, n - got);
    }
    double rms, snr = tone_snr(au, got, &rms);
    printf("%-34s %5d samples, tone %5.1f dB over the rest, level %5.3f, channel %5.1f dBFS\n", name, got, snr, rms,
           dsp_level_db(&s));
    CHECK(got == n / s.decim, "%s: %d samples, wanted %d", name, got, n / s.decim);
    CHECK(snr > 20, "%s: the tone is only %.1f dB over the rest", name, snr);
    CHECK(rms > 0.05 && rms < 0.9, "%s: level %.3f", name, rms);
    free(iq);
    free(au);
}

/// The squelch: shut on noise alone, open on a signal.
static void squelch(void) {
    int n = RATE / 4;
    uint8_t *iq = malloc(2 * n);
    int16_t *au = malloc(sizeof(int16_t) * n);
    static dsp_t s;
    dsp_init(&s, RATE, DSP_NFM);
    s.squelch_db = -30;
    for (int k = 0; k < n; ++k) {
        iq[2 * k] = (uint8_t)(127 + (int)lrint(3 * rnd()));
        iq[2 * k + 1] = (uint8_t)(127 + (int)lrint(3 * rnd()));
    }
    int got = dsp_process(&s, iq, n, au, n);
    double e = 0;
    for (int k = got / 2; k < got; ++k)
        e += (double)au[k] * au[k];
    printf("squelch on noise: %s at %.1f dBFS, audio energy %.0f\n", s.sq_open ? "open" : "shut", dsp_level_db(&s), e);
    CHECK(!s.sq_open && e < 1e3, "the squelch should be shut on noise");
    make(iq, n, RATE, DSP_NFM, 0, 1.0, 100000);
    got = dsp_process(&s, iq, n, au, n);
    printf("squelch on a signal: %s at %.1f dBFS\n", s.sq_open ? "open" : "shut", dsp_level_db(&s));
    CHECK(s.sq_open, "the squelch should open on a signal");
    free(iq);
    free(au);
}

/// The spectrum: a tone at +30 kHz is the biggest bin, in the right place.
static void spectrum(void) {
    uint8_t iq[2 * DSP_FFT_N];
    for (int k = 0; k < DSP_FFT_N; ++k) {
        double w = 2 * M_PI * 30000.0 * k / RATE;
        iq[2 * k] = (uint8_t)lrint(127.4 + 100 * cos(w));
        iq[2 * k + 1] = (uint8_t)lrint(127.4 + 100 * sin(w));
    }
    static dsp_spec_t sp;
    dsp_spec_init(&sp);
    for (int r = 0; r < 8; ++r) {
        dsp_spec_again(&sp);
        dsp_spec_feed(&sp, iq, DSP_FFT_N);
    }
    int best = 0;
    for (int k = 1; k < DSP_FFT_N; ++k)
        if (sp.db[k] > sp.db[best])
            best = k;
    int want = DSP_FFT_N / 2 + (int)lrint(30000.0 * DSP_FFT_N / RATE);
    printf("spectrum: peak at bin %d (%.1f dB), wanted %d; floor %.1f dB\n", best, sp.db[best], want, sp.db[10]);
    CHECK(abs(best - want) <= 1, "the peak is at bin %d, wanted %d", best, want);
    CHECK(sp.db[best] > -6 && sp.db[best] < 3, "the peak reads %.1f dB", sp.db[best]);
    CHECK(sp.db[10] < sp.db[best] - 40, "the floor is too high");
}

int main(void) {
    srand(1);
    run("wide FM, middle", RATE, DSP_WFM, 0, 1e9);
    run("wide FM, 288 kS/s", 288000, DSP_WFM, 0, 1e9);
    run("narrow FM, +40 kHz, other at -20k", RATE, DSP_NFM, 40000, -20000);
    run("narrow FM, -70 kHz, other at -45k", RATE, DSP_NFM, -70000, -45000);
    run("narrow FM, 288 kS/s, +100 kHz", 288000, DSP_NFM, 100000, 0);
    run("AM, +25 kHz, other at 0", RATE, DSP_AM, 25000, 0);
    run("AM, middle, other at 30k", RATE, DSP_AM, 0, 30000);
    squelch();
    spectrum();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
