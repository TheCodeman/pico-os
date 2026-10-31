/**
 * @file tonetest.c
 * @brief The radio's bass and treble on a PC: the shelves' gains against RBJ's formulas, the noise they add, and loud music through them.
 *
 *     ./tonetest.sh [song.mp3]  (the song also goes through the Helix MP3 decoder and the radio's resampler)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/radio/radio_tone.c"
#include "mp3dec.h"

static int failures;  ///< checks failed

/// A check.
/// @param ok it passed
/// @param what what it was
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

/// The gain the shelves should have at hz, from their transfer function in doubles.
/// @param hz the frequency
/// @return the gain in dB
static double expected_db(double hz) {
    double g = 0;
    for (int k = 0; k < 2; ++k) {
        const shelf_t *f = &tone[tone_at][k];
        double w = 2 * M_PI * hz / RATE, s = 1 << TONE_Q;
        double nr = f->b0 + f->b1 * cos(w) + f->b2 * cos(2 * w), ni = -f->b1 * sin(w) - f->b2 * sin(2 * w);
        double dr = s + f->a1 * cos(w) + f->a2 * cos(2 * w), di = -f->a1 * sin(w) - f->a2 * sin(2 * w);
        g += 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return g;
}

/// A sine through the shelves: the gain measured after they settle.
/// @param hz its frequency
/// @param amp its level
/// @param noise_db set to the error's level against an ideal sine, in dB below the signal
/// @return the gain in dB
static double measure_db(double hz, double amp, double *noise_db) {
    memset(tone_z, 0, sizeof tone_z);
    int n = 96000, from = 48000;
    double in = 0, out = 0, cr = 0, ci = 0;
    static double y[96000];
    for (int i = 0; i < n; ++i) {
        int l = (int)lrint(amp * sin(2 * M_PI * hz * i / RATE)), r = l;
        radio_tone_frame(&l, &r);
        y[i] = l;
        if (i >= from) {
            double x = amp * sin(2 * M_PI * hz * i / RATE);
            in += x * x;
            out += (double)l * l;
            cr += l * sin(2 * M_PI * hz * i / RATE);
            ci += l * cos(2 * M_PI * hz * i / RATE);
        }
    }
    // The error: what is left once the sine (fitted) is taken away.
    double m = n - from, a = 2 * cr / m, b = 2 * ci / m, e = 0;
    for (int i = from; i < n; ++i) {
        double d = y[i] - a * sin(2 * M_PI * hz * i / RATE) - b * cos(2 * M_PI * hz * i / RATE);
        e += d * d;
    }
    *noise_db = 10 * log10(out / (e + 1e-9));
    return 10 * log10(out / in);
}

/// Bass and treble at a setting: the gains at several frequencies.
/// @param bass the bass, dB
/// @param treble the treble
static void gains(int bass, int treble) {
    radio_set_tone(bass, treble);
    static const double hz[] = {40, 100, 150, 300, 1000, 2000, 4000, 8000, 15000};
    int ok = 1;
    printf("bass %+d treble %+d:", bass, treble);
    for (unsigned i = 0; i < sizeof hz / sizeof *hz; ++i) {
        double nd, g = measure_db(hz[i], 3000, &nd), want = expected_db(hz[i]);
        printf(" %g Hz %+.1f", hz[i], g);
        ok &= fabs(g - want) < 0.2;
    }
    printf("\n");
    char what[80];
    snprintf(what, sizeof what, "bass %+d treble %+d: gains as the coefficients say", bass, treble);
    check(ok, what);
}

/// The Helix decoder's buffers: from malloc on a PC.
/// @param n the size
/// @return the memory
void *radio_helix_alloc(size_t n) { return malloc(n); }
/// Frees them.
/// @param p the memory
void radio_helix_free(void *p) { free(p); }

/// A song: decoded, resampled as radio_source does, through the shelves;
/// checks nothing goes past what the mixer takes and the bass came up.
/// @param path the MP3
static void song(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no %s)\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *buf = malloc(n), *p = buf;
    if (fread(buf, 1, n, f) != (size_t)n)
        n = 0;
    fclose(f);
    int left = (int)n, rate = 44100, frames = 0;
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * 1152 * (n / 100 + 1));
    HMP3Decoder d = MP3InitDecoder();
    while (left > 0) {
        int off = MP3FindSyncWord(p, left);
        if (off < 0)
            break;
        p += off;
        left -= off;
        int16_t *o = pcm + 2 * frames;
        int r = MP3Decode(d, &p, &left, o, 0);
        if (r == ERR_MP3_INDATA_UNDERFLOW)
            break;
        if (r) {
            ++p, --left;
            continue;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d, &fi);
        rate = fi.samprate;
        int fr = fi.nChans == 2 ? fi.outputSamps / 2 : fi.outputSamps;
        if (fi.nChans == 1)
            for (int i = fr - 1; i >= 0; --i)
                o[2 * i] = o[2 * i + 1] = o[i];
        frames += fr;
    }
    MP3FreeDecoder(d);
    radio_set_tone(RADIO_BASS_DEFAULT, 0);
    radio_tone_reset();
    uint32_t step = (uint32_t)(((int64_t)rate << 16) / 48000), pos = 0, out = 0;
    int peak_in = 0, peak = 0;
    double low_in = 0, low_out = 0, zl = 0, zo = 0;
    FILE *w = fopen("radio_tone.raw", "wb");
    while (out + (pos >> 16) + 1 < (uint32_t)frames) {
        uint32_t a = out + (pos >> 16), b = a + 1;
        int fq = pos & 0xffff;
        int al = pcm[2 * a], ar = pcm[2 * a + 1], bl = pcm[2 * b], br = pcm[2 * b + 1];
        int vl = al + (((bl - al) * (fq >> 1)) >> 15), vr = ar + (((br - ar) * (fq >> 1)) >> 15);
        pos += step;
        out += pos >> 16;
        pos &= 0xffff;
        int x = vl;
        peak_in = abs(vl) > peak_in ? abs(vl) : peak_in;
        radio_tone_frame(&vl, &vr);
        peak = abs(vl) > peak ? abs(vl) : peak;
        zl += (x - zl) * 0.0065;  // one pole at 50 Hz: the low end's level
        zo += (vl - zo) * 0.0065;
        low_in += zl * zl;
        low_out += zo * zo;
        int16_t s[2] = {(int16_t)(vl > 32767 ? 32767 : vl < -32768 ? -32768 : vl),
                        (int16_t)(vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr)};
        if (w)
            fwrite(s, sizeof s, 1, w);
    }
    if (w)
        fclose(w);
    double lift = 10 * log10(low_out / low_in);
    printf("%s: %d frames at %d Hz, peak %d -> %d, below 50 Hz %+.1f dB (radio_tone.raw: 48 kHz stereo)\n", path,
           frames, rate, peak_in, peak, lift);
    check(peak < 32767 * 4, "a loud song stays within what the mixer takes");
    check(lift > 2 && lift < 4, "the default lifts the bass about 3 dB (6 less half of it)");
    free(pcm);
    free(buf);
}

/// Runs the checks.
/// @param argc the argument count
/// @param argv a song (MP3), optional
/// @return the failures
int main(int argc, char **argv) {
    gains(0, 0);
    gains(RADIO_BASS_DEFAULT, 0);
    gains(12, 0);
    gains(-12, 0);
    gains(0, 12);
    gains(0, -12);
    gains(12, 12);
    radio_set_tone(12, 0);
    double gl = expected_db(20), gm = expected_db(1000), gh = expected_db(20000);
    check(fabs(gl - 6) < 0.3 && fabs(gm + 6) < 0.3 && fabs(gh + 6) < 0.3,
          "+12 bass: +6 at the bottom, -6 elsewhere (the headroom)");
    radio_set_tone(0, 12);
    gl = expected_db(20), gh = expected_db(20000);
    check(fabs(gl + 6) < 0.3 && fabs(gh - 6) < 0.6, "+12 treble: -6 at the bottom, +6 at the top");
    // Noise: a quiet 60 Hz tone through the full bass boost.
    radio_set_tone(12, 0);
    double nd;
    measure_db(60, 200, &nd);
    printf("a 60 Hz sine at 200 through +12 bass: error %.1f dB below it\n", nd);
    check(nd > 45, "the bass shelf adds little noise to a quiet low tone");
    radio_set_tone(-12, -12);
    measure_db(1000, 32000, &nd);
    check(nd > 70, "a loud tone through both cuts stays clean");
    radio_set_tone(30, -30);
    check(radio_bass() == RADIO_TONE_MAX && radio_treble() == -RADIO_TONE_MAX, "settings kept within +-12 dB");
    radio_set_tone(0, 0);
    int l = 1234, r = -4321;
    radio_tone_frame(&l, &r);
    check(l == 1234 && r == -4321, "flat: the sound untouched");
    if (argc > 1)
        song(argv[1]);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
