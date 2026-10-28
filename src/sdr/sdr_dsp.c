/**
 * @file sdr_dsp.c
 * @brief The SDR's signal processing: see sdr_dsp.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "sdr_dsp.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

#define WFM_DEVIATION 75000.0f  ///< broadcast FM's peak deviation
#define NFM_DEVIATION 5000.0f   ///< narrow FM's (25 kHz channels; 12.5 kHz ones use 2.5 kHz)
#define WFM_AUDIO_HZ  15500.0f  ///< wide FM's audio filter: the mono audio, not the 19 kHz pilot
#define NFM_CHANNEL_HZ 7500.0f  ///< narrow FM's channel filter (each side)
#define AM_CHANNEL_HZ  5000.0f  ///< AM's
#define NFM_AUDIO_HZ   3000.0f  ///< narrow FM's audio low-pass (one pole)
#define SQ_HYST_DB     2.0f     ///< the squelch closes this far below where it opens

/// atan2 to within about 0.00001 radians, without the library's care.
/// @param y the imaginary part
/// @param x the real part
/// @return the angle, -pi..pi
static inline float fast_atan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx == 0.0f)
        return 0.0f;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = PI_F - r;
    return y < 0.0f ? -r : r;
}

/// A low-pass FIR filter: a Hamming-windowed sinc, summing to 1.
/// @param h the taps
/// @param n how many
/// @param cutoff the cutoff (Hz)
/// @param rate the sample rate
static void lowpass(float *h, int n, float cutoff, float rate) {
    float fc = cutoff / rate, sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = (float)i - (float)(n - 1) / 2.0f;
        float v = t == 0.0f ? 2.0f * fc : sinf(2.0f * PI_F * fc * t) / (PI_F * t);
        v *= 0.54f - 0.46f * cosf(2.0f * PI_F * (float)i / (float)(n - 1));
        h[i] = v;
        sum += v;
    }
    for (int i = 0; i < n; ++i)
        h[i] /= sum;
}

/// A filter's output: the taps against the last n samples.
/// @param h the taps
/// @param x the samples, oldest first
/// @param n how many
/// @return the sum
static inline float dot(const float *h, const float *x, int n) {
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < n; i += 2) {
        a += h[i] * x[i];
        b += h[i + 1] * x[i + 1];
    }
    return a + b;
}

/// The de-emphasis (and narrow FM's audio low-pass) coefficient.
/// @param s the state
/// @param us the time constant in microseconds (0: none)
static void set_de(dsp_t *s, float us) {
    s->de_a = us > 0.0f ? 1.0f - expf(-1.0f / ((float)DSP_AUDIO_RATE * us * 1e-6f)) : 1.0f;
}

void dsp_set_mode(dsp_t *s, int mode) {
    s->mode = mode;
    memset(s->hi, 0, sizeof s->hi);
    memset(s->hq, 0, sizeof s->hq);
    memset(s->ha, 0, sizeof s->ha);
    s->hpos = s->apos = s->phase = 0;
    s->pi = s->pq = 0.0f;
    s->de = 0.0f;
    s->env_avg = 0.0f;
    if (mode == DSP_WFM) {
        lowpass(s->af_taps, DSP_AF_TAPS, WFM_AUDIO_HZ, (float)s->rate);
        s->fm_scale = (float)s->rate / (2.0f * PI_F * WFM_DEVIATION);
        set_de(s, 75.0f);
    } else {
        lowpass(s->ch_taps, DSP_CH_TAPS, mode == DSP_NFM ? NFM_CHANNEL_HZ : AM_CHANNEL_HZ, (float)s->rate);
        s->fm_scale = (float)DSP_AUDIO_RATE / (2.0f * PI_F * NFM_DEVIATION);
        set_de(s, 1e6f / (2.0f * PI_F * NFM_AUDIO_HZ));
    }
}

void dsp_init(dsp_t *s, uint32_t rate, int mode) {
    memset(s, 0, sizeof *s);
    s->rate = rate;
    s->decim = (int)(rate / DSP_AUDIO_RATE);
    for (int i = 0; i < 256; ++i)
        s->lut[i] = ((float)i - 127.4f) / 128.0f;
    s->nco_re = 1.0f;
    s->rot_re = 1.0f;
    s->volume = 0.5f;
    s->squelch_db = -200.0f;
    s->power = 1e-6f;
    dsp_set_mode(s, mode);
}

void dsp_set_offset(dsp_t *s, int32_t hz) {
    s->offset_hz = hz;
    float w = -2.0f * PI_F * (float)hz / (float)s->rate;
    s->rot_re = cosf(w);
    s->rot_im = sinf(w);
}

void dsp_set_deemph(dsp_t *s, int us) {
    if (s->mode == DSP_WFM)
        set_de(s, (float)us);
}

float dsp_level_db(const dsp_t *s) {
    return 10.0f * log10f(s->power + 1e-12f);
}

/// The audio's last steps: de-emphasis, volume, squelch, to 16 bits.
/// @param s the state
/// @param y the demodulated sample (about -1..1)
/// @return the output sample
static inline int16_t finish(dsp_t *s, float y) {
    s->de += s->de_a * (y - s->de);
    s->sq_gain += ((s->sq_open ? 1.0f : 0.0f) - s->sq_gain) * 0.004f;
    float v = s->de * s->volume * s->sq_gain;
    if (v > 1.0f)
        v = 1.0f;
    else if (v < -1.0f)
        v = -1.0f;
    return (int16_t)(v * 32000.0f);
}

int dsp_process(dsp_t *s, const uint8_t *iq, int pairs, int16_t *out, int max) {
    int n = 0, pn = 0;
    float pw = 0.0f;
    const bool shift = s->offset_hz != 0;
    float nr = s->nco_re, ni = s->nco_im;
    const float rr = s->rot_re, ri = s->rot_im;
    for (int k = 0; k < pairs; ++k) {
        float i = s->lut[iq[2 * k]], q = s->lut[iq[2 * k + 1]];
        if (shift) {
            float a = i * nr - q * ni, b = i * ni + q * nr;
            i = a, q = b;
            float t = nr * rr - ni * ri;
            ni = nr * ri + ni * rr;
            nr = t;
        }
        if (s->mode == DSP_WFM) {
            pw += i * i + q * q;
            ++pn;
            float re = i * s->pi + q * s->pq, im = q * s->pi - i * s->pq;  // x * conj(x before)
            s->pi = i, s->pq = q;
            float d = fast_atan2(im, re) * s->fm_scale;
            s->ha[s->apos] = s->ha[s->apos + DSP_AF_TAPS] = d;
            if (++s->apos == DSP_AF_TAPS)
                s->apos = 0;
            if (++s->phase == s->decim) {
                s->phase = 0;
                if (n < max)
                    out[n++] = finish(s, dot(s->af_taps, s->ha + s->apos, DSP_AF_TAPS));
            }
            continue;
        }
        s->hi[s->hpos] = s->hi[s->hpos + DSP_CH_TAPS] = i;
        s->hq[s->hpos] = s->hq[s->hpos + DSP_CH_TAPS] = q;
        if (++s->hpos == DSP_CH_TAPS)
            s->hpos = 0;
        if (++s->phase < s->decim)
            continue;
        s->phase = 0;
        float ci = dot(s->ch_taps, s->hi + s->hpos, DSP_CH_TAPS);
        float cq = dot(s->ch_taps, s->hq + s->hpos, DSP_CH_TAPS);
        float p = ci * ci + cq * cq;
        pw += p;
        ++pn;
        float y;
        if (s->mode == DSP_NFM) {
            float re = ci * s->pi + cq * s->pq, im = cq * s->pi - ci * s->pq;
            s->pi = ci, s->pq = cq;
            y = fast_atan2(im, re) * s->fm_scale;
        } else {
            float env = sqrtf(p);
            s->env_avg += (env - s->env_avg) * 0.0005f;  // about 40 ms
            y = s->env_avg > 1e-7f ? 2.0f * (env / s->env_avg - 1.0f) : 0.0f;
        }
        if (n < max)
            out[n++] = finish(s, y);
    }
    if (shift) {
        // Keep the phasor on the unit circle.
        float m = 1.0f / sqrtf(nr * nr + ni * ni);
        s->nco_re = nr * m, s->nco_im = ni * m;
    }
    if (pn) {
        s->power += (pw / (float)pn - s->power) * 0.25f;
        float db = dsp_level_db(s);
        if (s->squelch_db <= -150.0f)
            s->sq_open = true;
        else if (s->sq_open)
            s->sq_open = db > s->squelch_db - SQ_HYST_DB;
        else
            s->sq_open = db > s->squelch_db;
    }
    return n;
}

// ---- The spectrum ------------------------------------------------------------------------------------

void dsp_spec_init(dsp_spec_t *sp) {
    memset(sp, 0, sizeof *sp);
    for (int i = 0; i < DSP_FFT_N; ++i) {
        sp->win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)DSP_FFT_N);
        sp->db[i] = -120.0f;
    }
}

/// An FFT in place (radix 2).
/// @param re the real parts
/// @param im the imaginary parts
/// @param n the size (a power of 2)
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i];
            im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2.0f * PI_F / (float)len, sr = cosf(a), si = sinf(a);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                int u = i + k, v = u + len / 2;
                float xr = re[v] * wr - im[v] * wi, xi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - xr, im[v] = im[u] - xi;
                re[u] += xr, im[u] += xi;
                float t = wr * sr - wi * si;
                wi = wr * si + wi * sr;
                wr = t;
            }
        }
    }
}

void dsp_spec_feed(dsp_spec_t *sp, const uint8_t *iq, int pairs) {
    if (sp->fill >= DSP_FFT_N)
        return;
    for (int k = 0; k < pairs && sp->fill < DSP_FFT_N; ++k, ++sp->fill) {
        float w = sp->win[sp->fill];
        sp->re[sp->fill] = ((float)iq[2 * k] - 127.4f) * w;
        sp->im[sp->fill] = ((float)iq[2 * k + 1] - 127.4f) * w;
    }
    if (sp->fill < DSP_FFT_N)
        return;
    fft(sp->re, sp->im, DSP_FFT_N);
    // A full-scale tone (128 counts) reads about 0 dB: the window sums to N/2.
    const float ref = 20.0f * log10f(128.0f * DSP_FFT_N / 2);
    for (int k = 0; k < DSP_FFT_N; ++k) {
        float p = sp->re[k] * sp->re[k] + sp->im[k] * sp->im[k];
        float db = 10.0f * log10f(p + 1e-9f) - ref;
        float *o = &sp->db[(k + DSP_FFT_N / 2) % DSP_FFT_N];
        *o += (db - *o) * 0.5f;
    }
    sp->fresh = true;
}

void dsp_spec_again(dsp_spec_t *sp) {
    sp->fill = 0;
}
