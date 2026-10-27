/**
 * @file genesis_host.c
 * @brief The Genesis on a PC, headless: loads a ROM, runs it for a number of frames as fast as it can, and saves the last frame as a PPM (and the sound as a WAV).
 *
 * With -p it also times the parts of the machine, to show where a frame's
 * time goes before the board has to run it.
 *
 *   genesis ROM [-n frames] [-o out.ppm] [-w out.wav] [-p] [-S n] [-h] [-q]
 *
 * -h runs the YM2612 at half rate (genesis_ym_half_rate).
 * -S n presses Start for 5 frames every n frames (to get past title
 * screens). -q prints only the numbers.
 *
 * The timing wraps the core's entry points (the Makefile links with
 * --wrap): the time in each call goes to its part, less the time in any
 * other part it calls (the 68000 writing to the YM2612 counts as YM2612).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 199309L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "genesis.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

// ---- Timing the parts ----

enum { P_OTHER, P_68K, P_Z80, P_VDP, P_YM, P_SN, P_COUNT };
static const char *const part_names[P_COUNT] = {
    "frame loop/other", "68000 (+bus, VDP ports)", "Z80", "VDP line render", "YM2612", "SN76489"};
static bool profiling;
static uint64_t part_ns[P_COUNT], part_calls[P_COUNT];
static int stack[16], depth;
static uint64_t mark;  ///< when the time last went to a part

static void enter(int p) {
    if (!profiling)
        return;
    uint64_t t = now_ns();
    part_ns[depth ? stack[depth - 1] : P_OTHER] += t - mark;
    mark = t;
    stack[depth++] = p;
    part_calls[p]++;
}

static void leave(void) {
    if (!profiling)
        return;
    uint64_t t = now_ns();
    part_ns[stack[--depth]] += t - mark;
    mark = t;
}

#define WRAP(ret, name, part, params, args)    \
    ret __real_##name params;                  \
    ret __wrap_##name params {                 \
        enter(part);                           \
        __real_##name args;                    \
        leave();                               \
    }
#define WRAP_RET(ret, name, part, params, args) \
    ret __real_##name params;                   \
    ret __wrap_##name params {                  \
        enter(part);                            \
        ret r = __real_##name args;             \
        leave();                                \
        return r;                               \
    }

WRAP(void, m68k_run, P_68K, (unsigned int cycles), (cycles))
WRAP(void, z80_run, P_Z80, (int target), (target))
WRAP(void, gwenesis_vdp_render_line, P_VDP, (int line), (line))
WRAP(void, ym2612_run, P_YM, (int target), (target))
WRAP(void, YM2612Write, P_YM, (unsigned int a, unsigned int v, int target), (a, v, target))
WRAP_RET(unsigned int, YM2612Read, P_YM, (int target), (target))
WRAP(void, gwenesis_SN76489_run, P_SN, (int target), (target))
WRAP(void, gwenesis_SN76489_Write, P_SN, (int data, int target), (data, target))

// ---- Picture and sound ----

static uint8_t frame[240][320];
static int16_t *wav;
static size_t wav_len, wav_cap;

static void put_line(void *ctx, int line, const uint8_t *pix, const uint16_t *palette) {
    memcpy(frame[line], pix, 320);
}

static bool write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    int w = 320, h = genesis_height();
    const uint16_t *pal = genesis_palette();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = pal[frame[y][x]];
            uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
            fwrite(rgb, 3, 1, f);
        }
    return fclose(f) == 0;
}

static void keep_sound(void) {
    const int16_t *ym, *sn;
    int n = genesis_audio(&ym, &sn);
    if (wav_len + n > wav_cap) {
        wav_cap = (wav_len + n) * 2;
        wav = realloc(wav, wav_cap * sizeof *wav);
    }
    for (int i = 0; i < n; i++) {
        int s = ym[i] + sn[i];
        wav[wav_len++] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    }
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    fwrite(b, 4, 1, f);
}

static void put16(FILE *f, uint16_t v) {
    uint8_t b[2] = {v, v >> 8};
    fwrite(b, 2, 1, f);
}

static bool write_wav(const char *path, unsigned rate) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint32_t bytes = wav_len * 2;
    fwrite("RIFF", 4, 1, f);
    put32(f, 36 + bytes);
    fwrite("WAVEfmt ", 8, 1, f);
    put32(f, 16);
    put16(f, 1);  // PCM
    put16(f, 1);  // mono
    put32(f, rate);
    put32(f, rate * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 4, 1, f);
    put32(f, bytes);
    for (size_t i = 0; i < wav_len; i++)
        put16(f, wav[i]);
    return fclose(f) == 0;
}

// ---- Running it ----

static void usage(void) {
    fprintf(stderr, "usage: genesis ROM [-n frames] [-o out.ppm] [-w out.wav] [-p] [-S n] [-h] [-q]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *ppm = NULL, *wav_path = NULL;
    long frames = 600, start_every = 0;
    bool quiet = false, half = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            ppm = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            wav_path = argv[++i];
        else if (!strcmp(argv[i], "-S") && i + 1 < argc)
            start_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "-p"))
            profiling = true;
        else if (!strcmp(argv[i], "-q"))
            quiet = true;
        else if (!strcmp(argv[i], "-h"))
            half = true;
        else if (argv[i][0] == '-' || rom_path)
            usage();
        else
            rom_path = argv[i];
    }
    if (!rom_path || frames < 1)
        usage();

    // The whole 8 MB the 68000 can read as ROM, zero past the file's end.
    uint8_t *rom = calloc(1, GENESIS_ROM_MAX);
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        perror(rom_path);
        return 1;
    }
    size_t size = fread(rom, 1, GENESIS_ROM_MAX, f);
    fclose(f);
    if (size < 0x200) {
        fprintf(stderr, "%s: too short for a Genesis ROM\n", rom_path);
        return 1;
    }
    size = (size + 1) & ~(size_t)1;

    genesis_init(malloc(GENESIS_MEM_SIZE), malloc(GENESIS_YM_FAST_SIZE), malloc(GENESIS_YM_SLOW_SIZE));
    const char *name = genesis_load(rom, size);
    genesis_ym_half_rate(half);
    if (!quiet)
        printf("%s: \"%s\", %zu KB\n", rom_path, name, size / 1024);

    mark = now_ns();
    uint64_t start = mark;
    for (long n = 0; n < frames; n++) {
        if (start_every)
            genesis_set_pad(0, n % start_every < 5 ? GENESIS_START : 0);
        genesis_run_frame(put_line, NULL);
        if (wav_path)
            keep_sound();
    }
    uint64_t took = now_ns() - start;
    if (profiling)
        part_ns[P_OTHER] += now_ns() - mark;

    double fps = frames * 1e9 / took, hz = genesis_pal() ? 50 : 60;
    printf("%ld frames in %.3f s: %.1f fps, %.1fx real time (%s, %dx%d)\n", frames, took / 1e9, fps,
           fps / hz, genesis_pal() ? "PAL" : "NTSC", genesis_width(), genesis_height());
    if (profiling) {
        uint64_t total = 0;
        for (int p = 0; p < P_COUNT; p++)
            total += part_ns[p];
        printf("per frame (timed, %.0f us in all; the timing itself adds to these):\n",
               total / 1e3 / frames);
        for (int p = 1; p <= P_COUNT; p++) {
            int q = p % P_COUNT;  // "other" last
            printf("  %-24s %8.1f us %5.1f%%  %8.0f calls\n", part_names[q], part_ns[q] / 1e3 / frames,
                   100.0 * part_ns[q] / total, (double)part_calls[q] / frames);
        }
    }
    if (ppm && !write_ppm(ppm)) {
        perror(ppm);
        return 1;
    }
    if (wav_path && !write_wav(wav_path, genesis_pal() ? GENESIS_AUDIO_RATE_PAL : GENESIS_AUDIO_RATE_NTSC)) {
        perror(wav_path);
        return 1;
    }
    return 0;
}
