// Runs src/mod/xm.c on a PC: renders an XM to a WAV file (to compare with
// libopenmpt, tools/modtest/compare.py), checks saving, and times the mixer.
//   xmtest IN.xm OUT.wav SECONDS
//   xmtest --convert IN.mod OUT.xm [SEPARATION]   (a MOD made into an XM, as the tracker does)
// It also saves each instrument as .XI and loads it back, checking it's the same.
// Build: cc -O2 -I../../src/mod xmtest.c ../../src/mod/xm.c ../../src/mod/mod.c ../../src/mod/mod2xm.c -lm -o xmtest
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "xm.h"

void *xm_alloc(size_t n) { return calloc(1, n); }
void xm_free(void *p) { free(p); }
void *mod_alloc(size_t n) { return calloc(1, n); }
void mod_free(void *p) { free(p); }
const char *xm_from_mod(xm_song_t *x, const mod_song_t *m, int speed, int bpm, int separation);

static unsigned char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return perror(path), NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    rewind(f);
    unsigned char *data = malloc(*len);
    if (fread(data, 1, *len, f) != (size_t)*len)
        return NULL;
    fclose(f);
    return data;
}

static int convert(const char *in, const char *out, int separation) {
    long len;
    unsigned char *data = read_file(in, &len);
    if (!data)
        return 1;
    static mod_song_t m;
    const char *err = mod_load(&m, data, len);
    if (err)
        return fprintf(stderr, "%s: %s\n", in, err), 1;
    static xm_song_t x;
    err = xm_from_mod(&x, &m, 6, 125, separation);
    if (err)
        return fprintf(stderr, "convert: %s\n", err), 1;
    size_t n = xm_save_size(&x);
    unsigned char *o = malloc(n);
    xm_save(&x, o);
    FILE *f = fopen(out, "wb");
    fwrite(o, 1, n, f);
    fclose(f);
    printf("%s: %d channels, %d samples -> %s (%zu bytes, %d instruments)\n", in, m.channels, m.nsamples, out, n,
           x.ninstruments);
    return 0;
}

// Each instrument through an .XI file and back: the same?
static void check_xi(const xm_song_t *s) {
    int bad = 0;
    for (int i = 0; i < s->ninstruments; ++i) {
        const xm_instrument_t *a = &s->instruments[i];
        size_t n = xm_xi_size(a);
        unsigned char *f = malloc(n);
        xm_xi_save(a, f);
        xm_instrument_t b;
        const char *err = xm_xi_load(&b, f, n);
        bool same = !err && !memcmp(a->name, b.name, 22) && !memcmp(a->keymap, b.keymap, 96) &&
                    !memcmp(&a->venv, &b.venv, sizeof a->venv) && !memcmp(&a->penv, &b.penv, sizeof a->penv) &&
                    a->fadeout == b.fadeout && a->vib_rate == b.vib_rate && a->nsamples == b.nsamples;
        for (int k = 0; same && k < a->nsamples; ++k) {
            const xm_sample_t *p = &a->samples[k], *q = &b.samples[k];
            same = p->length == q->length && p->loop_start == q->loop_start && p->loop_len == q->loop_len &&
                   p->loop == q->loop && p->volume == q->volume && p->finetune == q->finetune &&
                   p->pan == q->pan && p->relnote == q->relnote && p->bits16 == q->bits16 &&
                   !memcmp(p->data, q->data, p->length * 2);
        }
        if (!same)
            ++bad, printf("xi: instrument %d differs (%s)\n", i + 1, err ? err : "fields");
        free(f);
    }
    printf("xi: %d instruments saved and loaded back, %d differ\n", s->ninstruments, bad);
}

static void put32(FILE *f, unsigned v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--convert"))
        return convert(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0);
    if (argc < 4)
        return fprintf(stderr, "usage: xmtest IN.xm OUT.wav SECONDS\n"), 2;
    long len;
    unsigned char *data = read_file(argv[1], &len);
    if (!data)
        return 1;
    static xm_song_t song;
    const char *err = xm_load(&song, data, len);
    if (err)
        return fprintf(stderr, "load: %s\n", err), 1;
    printf("%s: \"%s\" (%s), %d channels, %d positions, %d patterns, %d instruments, %s\n", argv[1],
           song.title, song.tracker, song.channels, song.length, song.npatterns, song.ninstruments,
           song.linear ? "linear" : "Amiga");
    size_t n = xm_save_size(&song);
    unsigned char *back = malloc(n);
    xm_save(&song, back);
    if (n == (size_t)len && !memcmp(back, data, n)) {
        printf("save: identical to the file (%zu bytes)\n", n);
    } else {
        size_t i = 0;
        while (i < n && i < (size_t)len && back[i] == data[i])
            ++i;
        printf("save: %zu bytes (file %ld), first difference at %zu\n", n, len, i);
    }
    check_xi(&song);
    xm_play(&song, 0, 0);
    FILE *w = fopen(argv[2], "wb");
    long frames = (long)(atof(argv[3]) * XM_RATE);
    fwrite("RIFF", 1, 4, w); put32(w, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, w);
    put32(w, 16); put32(w, 1 | 2 << 16); put32(w, XM_RATE); put32(w, XM_RATE * 4); put32(w, 4 | 16 << 16);
    fwrite("data", 1, 4, w); put32(w, frames * 4);
    long clipped = 0;
    clock_t t0 = clock();
    double mix_s = 0;
    for (long i = 0; i < frames; i += 4) {
        int32_t buf[8] = {0};
        clock_t a = clock();
        xm_render(buf, 4);
        mix_s += (double)(clock() - a) / CLOCKS_PER_SEC;
        for (int k = 0; k < 8; ++k) {
            int32_t v = buf[k];
            if (v > 32767 || v < -32768)
                ++clipped;
            short s = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            fwrite(&s, 2, 1, w);
        }
    }
    fclose(w);
    (void)t0;
    xm_status_t st;
    xm_get_status(&st);
    printf("rendered %ld frames in %.2f s of CPU; now at order %d row %d; clipped %ld\n", frames, mix_s,
           st.order, st.row, clipped);
    return 0;
}
