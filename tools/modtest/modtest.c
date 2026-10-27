// Runs src/mod on a PC: renders a MOD to a WAV file (to compare with
// libopenmpt, tools/modtest/compare.py) and checks that saving gives back
// the same file.
//   modtest IN.mod OUT.wav SECONDS [separation%]
// Build: cc -O2 -I../../src/mod modtest.c ../../src/mod/mod.c -o modtest
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

void *mod_alloc(size_t n) { return malloc(n); }
void mod_free(void *p) { free(p); }

static void put32(FILE *f, unsigned v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv) {
    if (argc < 4)
        return fprintf(stderr, "usage: modtest IN.mod OUT.wav SECONDS [separation]\n"), 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    unsigned char *data = malloc(len);
    fread(data, 1, len, f);
    fclose(f);
    static mod_song_t song;
    const char *err = mod_load(&song, data, len);
    if (err)
        return fprintf(stderr, "load: %s\n", err), 1;
    printf("%s: \"%.20s\" %s, %d channels, %d positions, %d patterns\n", argv[1], song.title,
           song.sig, song.channels, song.length, song.npatterns);
    size_t n = mod_save_size(&song);
    unsigned char *back = malloc(n);
    mod_save(&song, back);
    if (n == (size_t)len && !memcmp(back, data, n))
        printf("save: identical to the file (%zu bytes)\n", n);
    else {
        size_t i = 0;
        while (i < n && i < (size_t)len && back[i] == data[i])
            ++i;
        printf("save: %zu bytes (file %ld), first difference at %zu\n", n, len, i);
    }
    if (argc > 4)
        mod_set_separation(atoi(argv[4]));
    mod_play(&song, 0, 0, false);
    FILE *w = fopen(argv[2], "wb");
    long frames = (long)(atof(argv[3]) * MOD_RATE);
    fwrite("RIFF", 1, 4, w); put32(w, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, w);
    put32(w, 16); put32(w, 1 | 2 << 16); put32(w, MOD_RATE); put32(w, MOD_RATE * 4); put32(w, 4 | 16 << 16);
    fwrite("data", 1, 4, w); put32(w, frames * 4);
    long clipped = 0;
    for (long i = 0; i < frames; ++i) {
        int32_t l, r;
        mod_render(&l, &r);
        short s[2];
        int32_t v[2] = {l, r};
        for (int k = 0; k < 2; ++k) {
            if (v[k] > 32767 || v[k] < -32768)
                ++clipped;
            s[k] = v[k] > 32767 ? 32767 : v[k] < -32768 ? -32768 : v[k];
        }
        fwrite(s, 2, 2, w);
    }
    fclose(w);
    mod_status_t st;
    mod_get_status(&st);
    printf("rendered %ld frames; now at order %d row %d; clipped samples %ld\n", frames, st.order, st.row, clipped);
    return 0;
}
