/**
 * @file say.c
 * @brief The `say` command: text to speech over HDMI, a line at a time, from a file, or into a WAV file.
 *
 * The speech is made on core 0 a piece at a time (tts_render, 16 kHz) into a
 * small ring, and core 1's audio source plays it from there at 48 kHz, each
 * sample three times as long with straight lines between them. So speech of
 * any length starts at once and needs only the ring (8 KB) and the
 * synthesizer's list of sounds, all freed when the command ends: nothing of
 * it stays in SRAM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "say.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "ff.h"
#include "hardware/sync.h"
#include "mc.h"
#include "pico/stdlib.h"
#include "shell.h"
#include "tts.h"

#define RING 4096       ///< samples in the ring (a power of two)
#define TEXT_MAX 1024   ///< the longest text spoken in one go
#define PHON_MAX 8192   ///< room for its phonemes

/// What core 0 and core 1 share while speaking.
typedef struct {
    int16_t ring[RING];      ///< the speech, 16 kHz
    volatile uint32_t w, r;  ///< samples written (core 0) and read (core 1)
    volatile bool end;       ///< all of it is in the ring
    volatile bool done;      ///< and all of it has been played
    int sub;                 ///< which of the three 48 kHz frames of a sample
    int32_t s0, s1;          ///< the samples played between
} player_t;

static player_t *P;  ///< the player while speaking

/// The audio source (core 1): the next 48 kHz frame of the speech.
/// @param left the left sample
/// @param right the right
/// @return there was speech to play
static bool play_src(int32_t *left, int32_t *right) {
    player_t *p = P;
    *left = *right = 0;
    if (!p)
        return false;
    if (p->sub == 0) {
        if (p->r == p->w) {  // nothing new: the end, or core 0 behind
            if (p->end)
                p->done = true;
            p->s0 = p->s1 = 0;
            return false;
        }
        p->s0 = p->s1;
        p->s1 = p->ring[p->r % RING];
        p->r = p->r + 1;
    }
    int32_t v = p->s0 + (p->s1 - p->s0) * p->sub / 3;
    p->sub = p->sub == 2 ? 0 : p->sub + 1;
    *left = *right = v;
    return true;
}

/// Turns text into phonemes, shown if asked.
/// @param text the text
/// @param show print them
/// @return the phonemes (malloc'd), or NULL (said why)
static char *phonemes(const char *text, bool show) {
    char *ph = malloc(PHON_MAX);
    if (!ph) {
        printf("say: not enough memory\n");
        return NULL;
    }
    if (tts_phonemes(text, ph, PHON_MAX) < 0)
        printf("say: too long; saying the start of it\n");
    if (show)
        printf("%s\n", ph);
    return ph;
}

/// Speaks a piece of text over HDMI until it ends or Esc is pressed.
/// @param text the text
/// @param v the voice
/// @param show print the phonemes
/// @return false if Esc stopped it
static bool speak_piece(const char *text, const tts_voice_t *v, bool show) {
    char *ph = phonemes(text, show);
    if (!ph)
        return true;
    const char *bad;
    tts_t *t = tts_start(ph, v, &bad);
    if (bad) {
        int n = 0;
        while (bad[n] && bad[n] != ' ')
            ++n;
        printf("say: no phoneme %.*s\n", n, bad);
    }
    free(ph);
    player_t *p = calloc(1, sizeof *p);
    if (!t || !p) {
        printf("say: not enough memory\n");
        tts_end(t);
        free(p);
        return true;
    }
    P = p;
    bool started = false, stopped = false;
    while (!p->done) {
        // Top the ring up, a contiguous piece at a time.
        while (!p->end) {
            uint32_t room = RING - (p->w - p->r), at = p->w % RING;
            if (room < 256)
                break;
            if (room > RING - at)
                room = RING - at;
            size_t got = tts_render(t, p->ring + at, room);
            __dmb();  // the samples are there before core 1 sees w move
            p->w = p->w + (uint32_t)got;
            if (got < room)
                p->end = true;
        }
        if (!started) {
            audio_set_source(play_src);
            started = true;
        }
        if (key_stop()) {
            stopped = true;
            break;
        }
        if (audio_get_source() != play_src)
            break;  // something else took the sound
        sleep_ms(2);
    }
    if (audio_get_source() == play_src)
        audio_set_source(NULL);
    sleep_ms(2);  // core 1 may be in the middle of a frame
    P = NULL;
    free(p);
    tts_end(t);
    return !stopped;
}

/// Puts a 16-bit little-endian number into a header.
static void le16(uint8_t *d, unsigned v) {
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
}

/// Puts a 32-bit little-endian number into a header.
static void le32(uint8_t *d, uint32_t v) {
    le16(d, v & 0xffff);
    le16(d + 2, v >> 16);
}

/// A WAV file being written.
typedef struct {
    FIL f;           ///< the file
    FRESULT r;       ///< the first error
    uint32_t bytes;  ///< the samples' bytes so far
    int16_t buf[512];  ///< a piece of speech
} wav_t;

/// Writes (or rewrites) the WAV header: 16 kHz, mono, 16 bits.
/// @param w the file
static void wav_header(wav_t *w) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + w->bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);  // PCM
    le16(h + 22, 1);  // mono
    le32(h + 24, TTS_RATE);
    le32(h + 28, TTS_RATE * 2);
    le16(h + 32, 2);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, w->bytes);
    UINT wr;
    if (w->r == FR_OK && (w->r = f_lseek(&w->f, 0)) == FR_OK &&
        (w->r = f_write(&w->f, h, sizeof h, &wr)) == FR_OK && wr != sizeof h)
        w->r = FR_DENIED;
}

/// Adds the speech of a piece of text to a WAV file.
/// @param text the text
/// @param v the voice
/// @param show print the phonemes
/// @param w the file
/// @return false after an error
static bool save_piece(const char *text, const tts_voice_t *v, bool show, wav_t *w) {
    char *ph = phonemes(text, show);
    if (!ph)
        return false;
    tts_t *t = tts_start(ph, v, NULL);
    free(ph);
    if (!t) {
        printf("say: not enough memory\n");
        return false;
    }
    size_t n;
    UINT wr;
    while (w->r == FR_OK && (n = tts_render(t, w->buf, 512)) > 0) {
        // (Little-endian samples, as the file wants them.)
        if ((w->r = f_write(&w->f, w->buf, n * 2, &wr)) == FR_OK && wr != n * 2)
            w->r = FR_DENIED;
        w->bytes += (uint32_t)n * 2;
    }
    tts_end(t);
    return w->r == FR_OK;
}

/// What is done with each piece of the text: spoken, or saved.
typedef struct {
    const tts_voice_t *v;  ///< the voice
    bool show;             ///< print the phonemes
    wav_t *wav;            ///< the WAV file (NULL: speak)
} job_t;

/// Speaks or saves text a sentence at a time (at most about 240 characters),
/// so the synthesizer's list of sounds stays small.
/// @param text the text
/// @param j what to do with it
/// @return false if stopped (Esc) or an error
static bool say_text(const char *text, const job_t *j) {
    char piece[256];
    while (*text) {
        while (*text == ' ')
            ++text;
        size_t n = 0, cut = 0;
        for (; text[n] && n < sizeof piece - 16; ++n) {
            if (text[n] == ' ')
                cut = n;
            if (strchr(".!?;", text[n]) && (text[n + 1] == ' ' || !text[n + 1])) {
                cut = ++n;
                break;
            }
        }
        if (!text[n] || !cut)
            cut = n;
        memcpy(piece, text, cut);
        piece[cut] = 0;
        text += cut;
        if (!piece[0])
            continue;
        if (!(j->wav ? save_piece(piece, j->v, j->show, j->wav) : speak_piece(piece, j->v, j->show)))
            return false;
    }
    return true;
}

/// Reads a line typed at the keyboard: Backspace rubs out, Up brings back
/// the line before.
/// @param buf the line
/// @param size its room
/// @param last the line before ("" if none)
/// @return false for Esc or Ctrl-C
static bool read_line(char *buf, int size, const char *last) {
    int n = 0;
    buf[0] = 0;
    printf("say> ");
    fflush(stdout);
    for (;;) {
        int k = key_get();
        if (k == KEY_ESC || k == CTRL('c')) {
            printf("\n");
            return false;
        }
        if (k == '\r' || k == '\n') {
            printf("\n");
            return true;
        }
        if ((k == 8 || k == 127) && n > 0) {
            buf[--n] = 0;
            printf("\b \b");
        } else if (k == KEY_UP && last[0]) {
            while (n > 0) {
                buf[--n] = 0;
                printf("\b \b");
            }
            snprintf(buf, size, "%s", last);
            n = (int)strlen(buf);
            printf("%s", buf);
        } else if (k >= ' ' && k < 127 && n < size - 1) {
            buf[n++] = (char)k;
            buf[n] = 0;
            putchar(k);
        }
        fflush(stdout);
    }
}

/// How say is used.
static void usage(void) {
    printf("say [options] text      speaks the text over HDMI (Esc stops)\n"
           "say [options]           type lines to hear them; Up repeats one,\n"
           "                        an empty line or Esc ends\n"
           "say [options] -f file   reads a text file aloud\n"
           "  -p Hz    the pitch (50-300, normally 110)\n"
           "  -s %%     the speed (40-250, normally 100)\n"
           "  -w file  saves the speech as a WAV file instead\n"
           "  -x       shows the phonemes\n"
           "  Text in [brackets] is phonemes, e.g. say [HH AX L OW1]\n");
}

void say_command(int argc, char **argv) {
    tts_voice_t v = {0, 0};
    const char *wav = NULL, *file = NULL;
    bool show = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        char o = argv[i][1];
        if (o == 'x') {
            show = true;
        } else if ((o == 'p' || o == 's' || o == 'w' || o == 'f') && i + 1 < argc) {
            const char *a = argv[++i];
            if (o == 'p')
                v.pitch = atoi(a);
            else if (o == 's')
                v.speed = atoi(a);
            else if (o == 'w')
                wav = a;
            else
                file = a;
        } else {
            usage();
            return;
        }
    }
    if (i < argc && (!strcmp(argv[i], "help") || !strcmp(argv[i], "-h"))) {
        usage();
        return;
    }
    char *text = malloc(TEXT_MAX);
    if (!text) {
        printf("say: not enough memory\n");
        return;
    }
    text[0] = 0;
    for (size_t len = 0; i < argc; ++i) {  // the words, as one line
        int n = snprintf(text + len, TEXT_MAX - len, "%s%s", len ? " " : "", argv[i]);
        if (n < 0 || len + (size_t)n >= TEXT_MAX)
            break;
        len += (size_t)n;
    }

    job_t job = {&v, show, NULL};
    if (wav) {
        job.wav = calloc(1, sizeof *job.wav);
        if (!job.wav) {
            printf("say: not enough memory\n");
            free(text);
            return;
        }
        if ((job.wav->r = f_open(&job.wav->f, wav, FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK)
            wav_header(job.wav);
    }
    if (job.wav && job.wav->r != FR_OK) {
        // (said below)
    } else if (file) {  // a file, a line at a time
        FIL f;
        FRESULT r = f_open(&f, file, FA_READ);
        if (r != FR_OK) {
            printf("say: %s: %s\n", file, fresult_name(r));
        } else {
            while (f_gets(text, TEXT_MAX, &f))
                if (!say_text(text, &job))
                    break;
            f_close(&f);
        }
    } else if (text[0]) {
        say_text(text, &job);
    } else if (shell_batch) {
        usage();
    } else {  // lines typed
        char *last = calloc(1, TEXT_MAX);
        if (last) {
            printf("Type a line and Enter to hear it; Esc or an empty line ends.\n");
            while (read_line(text, TEXT_MAX, last) && text[0]) {
                strcpy(last, text);
                say_text(text, &job);
                if (job.wav)
                    break;
            }
            free(last);
        }
    }
    if (job.wav) {
        wav_t *w = job.wav;
        if (w->r == FR_OK)
            wav_header(w);  // now with the length
        FRESULT rc = f_close(&w->f);
        if (w->r == FR_OK)
            w->r = rc;
        if (w->r != FR_OK)
            printf("say: %s: %s\n", wav, fresult_name(w->r));
        else
            printf("%s: %.1f s of speech\n", wav, w->bytes / 2 / (double)TTS_RATE);
        free(w);
    }
    free(text);
}
