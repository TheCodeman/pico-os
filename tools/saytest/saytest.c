/**
 * @file saytest.c
 * @brief PC test of the text-to-speech engine: spelling rules against known words, the levels of each kind of sound, and WAV files to listen to.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "../../src/say/tts.c"

/// Words and what the rules should make of them (stress marks included).
static const char *const words[][2] = {
    {"hello world", "HH AX L OW1 / W ER1 L D / ."},
    {"cat", "K AE1 T / ."},
    {"the dog", "DH AX / D AA1 G / ."},
    {"speak", "S P IY1 K / ."},
    {"nation", "N EY1 SH AX N / ."},
    {"thinking", "TH IH1 NG K IH NG / ."},
    {"phone", "F OW1 N / ."},
    {"knight", "N AY1 T / ."},
    {"make", "M EY1 K / ."},
    {"boat", "B OW1 T / ."},
    {"rain", "R EY1 N / ."},
    {"church", "CH ER1 CH / ."},
    {"judge", "JH AH1 JH / ."},
    {"wished", "W IH1 SH T / ."},
    {"played", "P L EY1 D / ."},
    {"wanted", "W AA1 N T IH D / ."},
    {"USB drive", "Y UW1 / EH1 S / B IY1 / D R AY1 V / ."},
    {"1984", "W AH1 N / TH AW1 Z AE N D / N AY1 N / HH AH1 N D R AX D / EY1 T IY / F AO1 R / ."},
    {"3.5", "TH R IY1 / P OY1 N T / F AY1 V / ."},
    {"is it on?", "IH Z / IH T / AA N / ?"},
    {"yes, no.", "Y EH1 S / , N OW1 / ."},
};

/// Writes 16 kHz mono samples as a WAV file.
static void write_wav(const char *name, const int16_t *d, size_t n) {
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    uint32_t bytes = (uint32_t)(n * 2), rate = TTS_RATE, br = TTS_RATE * 2, v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f);
    v = 36 + bytes;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    s = 1;
    fwrite(&s, 2, 1, f);
    fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f);
    s = 2;
    fwrite(&s, 2, 1, f);
    s = 16;
    fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(d, 2, n, f);
    fclose(f);
}

/// Speaks text into a WAV file, and checks the length and the peak.
static int speak(const char *text, const tts_voice_t *v, const char *name, int verbose) {
    char ph[4096];
    if (tts_phonemes(text, ph, sizeof ph) < 0) {
        printf("FAIL: phonemes of \"%s\" didn't fit\n", text);
        return 1;
    }
    const char *bad;
    tts_t *t = tts_start(ph, v, &bad);
    if (!t || bad) {
        printf("FAIL: \"%s\": %s\n", text, t ? bad : "no memory");
        return 1;
    }
    size_t len = tts_length(t);
    int16_t *d = malloc(len * 2);
    size_t got = 0, step = 333;  // in odd pieces, as the player asks
    while (got < len) {
        size_t k = tts_render(t, d + got, len - got < step ? len - got : step);
        if (!k)
            break;
        got += k;
    }
    int fails = 0;
    if (got != len || tts_render(t, d, 1) != 0) {
        printf("FAIL: \"%s\": made %zu of %zu samples\n", text, got, len);
        fails++;
    }
    int peak = 0;
    double sum = 0;
    for (size_t i = 0; i < got; ++i) {
        int a = abs(d[i]);
        peak = a > peak ? a : peak;
        sum += (double)d[i] * d[i];
    }
    if (verbose)
        printf("  %-40.40s %5.2f s  peak %5d  rms %5.0f\n", text, got / (double)TTS_RATE, peak,
               sqrt(sum / (got ? got : 1)));
    if (peak < 6000 || peak > 32767) {
        printf("FAIL: \"%s\": peak %d\n", text, peak);
        fails++;
    }
    if (name)
        write_wav(name, d, got);
    free(d);
    tts_end(t);
    return fails;
}

/// The RMS level of each phoneme said on its own between two AA vowels, as
/// a fraction of the vowels' level: frication and bursts should sit well
/// below the vowels, as in speech.
static void levels(void) {
    printf("Levels (dB against the vowel):\n");
    for (int i = 0; i < NPH; ++i) {
        char ph[64];
        snprintf(ph, sizeof ph, "AA1 %s AA1", phons[i].name);
        tts_t *t = tts_start(ph, NULL, NULL);
        size_t len = tts_length(t);
        int16_t *d = malloc(len * 2);
        tts_render(t, d, len);
        // The middle segment(s) of the phoneme, and the first vowel's middle.
        seg_t *v = &t->seg[1];
        double vs = 0, cs = 0;
        size_t vn = 0, cn = 0;
        for (uint32_t s = v->start + v->len / 4; s < v->start + v->len * 3 / 4; ++s, ++vn)
            vs += (double)d[s] * d[s];
        uint32_t a = t->seg[2].start, e = t->seg[t->nseg - 3].start + t->seg[t->nseg - 3].len;
        for (uint32_t s = a; s < e; ++s, ++cn)
            cs += (double)d[s] * d[s];
        printf("  %-3s %6.1f\n", phons[i].name, 10 * log10((cs / cn + 1) / (vs / vn + 1)));
        free(d);
        tts_end(t);
    }
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    int fails = 0;
    char ph[1024];
    printf("Spelling rules:\n");
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) {
        tts_phonemes(words[i][0], ph, sizeof ph);
        // Compare without the trailing space.
        size_t n = strlen(ph);
        while (n && ph[n - 1] == ' ')
            ph[--n] = 0;
        bool ok = !strcmp(ph, words[i][1]);
        printf("  %s %-14s %s\n", ok ? "ok  " : "FAIL", words[i][0], ph);
        fails += !ok;
    }
    // Too little room: whole words only, and -1.
    char small[20];
    if (tts_phonemes("one two three four five six", small, sizeof small) != -1 || strlen(small) >= 20) {
        printf("FAIL: overflow\n");
        fails++;
    }
    levels();

    printf("Speech:\n");
    static const char *const lines[] = {
        "Hello world. This is the Pico 2 speaking through HDMI.",
        "The quick brown fox jumps over the lazy dog.",
        "Is the USB keyboard plugged in?",
        "I can count: one, two, three, four, five, 1024, and 3.14.",
        "Sixty-six shiny sheep, chewing cheese in the sun.",
    };
    char name[512];
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
        snprintf(name, sizeof name, "%s/say_%zu.wav", out, i + 1);
        fails += speak(lines[i], NULL, name, 1);
    }
    tts_voice_t hi = {180, 130}, lo = {70, 70};
    snprintf(name, sizeof name, "%s/say_high_fast.wav", out);
    fails += speak("A higher voice, a little faster.", &hi, name, 1);
    snprintf(name, sizeof name, "%s/say_low_slow.wav", out);
    fails += speak("A low voice, slowly.", &lo, name, 1);
    fails += speak("[HH EH1 L OW / DH EH1 R] .", NULL, NULL, 1);
    // A long text: every letter, digit and sign, much longer than a line.
    static char longtext[3000];
    static const char chars[] =
        " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,;:?!'\"&+%@=$*<>()-#~ e ";
    for (int i = 0; i < 2900; ++i)
        longtext[i] = chars[(i * 37 + i / 7) % (sizeof chars - 1)];
    char *big = malloc(40000);
    int r = tts_phonemes(longtext, big, 40000);
    const char *bad = NULL;
    tts_t *t = tts_start(big, NULL, &bad);
    if (r < 0 || !t || bad) {
        printf("FAIL: long text (%d, %s)\n", r, bad ? bad : "");
        fails++;
    } else {
        int16_t buf[1000];
        size_t total = 0, k;
        while ((k = tts_render(t, buf, 1000)) > 0)
            total += k;
        printf("  long text: %d phoneme chars, %.1f s\n", r, total / (double)TTS_RATE);
    }
    tts_end(t);
    free(big);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
