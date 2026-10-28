/**
 * @file uitest.c
 * @brief The WAV editor on a PC: loads WAV files through the file box, selects with the mouse, checks cut, copy, paste, delete, trim, the effects, undo and redo, format changes, saving, playing and the PSRAM checksums, and writes screenshots.
 *
 *     ./uitest.sh  (builds this, writes wav_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#define PSRAM_UNCACHED_OFFSET 0  ///< no second window on a PC
#include "../../src/wav/wav_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock
static bool (*source)(int32_t *, int32_t *);  ///< the audio source

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return KEY_NONE; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }
void audio_set_source(bool (*fn)(int32_t *, int32_t *)) { source = fn; }
bool (*audio_get_source(void))(int32_t *, int32_t *) { return source; }

size_t pc_psram_limit = 8u << 20;  ///< the PSRAM "free"
static size_t psram_used;          ///< and used

void *psram_malloc(size_t n) {
    if (psram_used + n > pc_psram_limit)
        return NULL;
    size_t *p = malloc(n + 16);
    if (!p)
        return NULL;
    p[0] = n;
    psram_used += n;
    return (uint8_t *)p + 16;
}
void psram_free(void *p) {
    if (!p)
        return;
    size_t *b = (size_t *)((uint8_t *)p - 16);
    psram_used -= b[0];
    free(b);
}
void *psram_realloc(void *p, size_t n) {
    size_t *b = (size_t *)((uint8_t *)p - 16), was = b[0];
    if (psram_used - was + n > pc_psram_limit)
        return NULL;
    b = realloc(b, n + 16);
    if (!b)
        return NULL;
    b[0] = n;
    psram_used = psram_used - was + n;
    return (uint8_t *)b + 16;
}
size_t psram_free_total(void) { return pc_psram_limit - psram_used; }

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param path the path
/// @return the local one
static const char *local(const char *path) {
    static char buf[300];
    snprintf(buf, sizeof buf, ".%s%s", path[0] == '/' ? "" : "/", path);
    return buf;
}
FRESULT f_open(FIL *f, const char *path, int mode) {
    f->f = fopen(local(path), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }
FRESULT f_lseek(FIL *f, FSIZE_t ofs) { return fseek(f->f, (long)ofs, SEEK_SET) ? 1 : FR_OK; }
FSIZE_t f_size(FIL *f) {
    long at = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long n = ftell(f->f);
    fseek(f->f, at, SEEK_SET);
    return (FSIZE_t)n;
}
void *pc_opendir(const char *path);                          ///< dirstub.c
void pc_readdir(void *d, char *name, int n, int *is_dir);    ///< dirstub.c
void pc_closedir(void *d);                                   ///< dirstub.c
FRESULT f_opendir(DIR *d, const char *path) { d->d = pc_opendir(local(path)); return d->d ? FR_OK : 1; }
FRESULT f_readdir(DIR *d, FILINFO *fi) {
    int is_dir = 0;
    pc_readdir(d->d, fi->fname, sizeof fi->fname, &is_dir);
    fi->fattrib = is_dir ? AM_DIR : 0;
    return FR_OK;
}
FRESULT f_closedir(DIR *d) { pc_closedir(d->d); return FR_OK; }
FRESULT f_stat(const char *path, FILINFO *fi) {
    struct stat st;
    if (stat(local(path), &st))
        return 1;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fi->fsize = (unsigned long)st.st_size;
    return FR_OK;
}
FRESULT f_getcwd(char *buf, UINT n) { snprintf(buf, n, "/"); return FR_OK; }

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            ++failures;                                 \
            printf("FAIL uitest.c:%d: ", __LINE__);     \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/// What the editor's loop does after events: draws again if it needs to.
static void settle(void) {
    if (F->dirty && !F->drag) {
        F->dirty = false;
        draw();
    }
}

/// A mouse event.
/// @param type MOUSE_*
/// @param btn the button
/// @param x where
/// @param y where
static void ev(int type, int btn, int x, int y) {
    mouse_event_t e = {(uint8_t)type, (uint8_t)btn, (uint8_t)(type == MOUSE_UP ? 0 : btn), 0, (int16_t)x,
                       (int16_t)y, false};
    mouse(&e);
    settle();
}

/// A click.
/// @param x where
/// @param y where
/// @param btn the button
static void click(int x, int y, int btn) {
    ev(MOUSE_DOWN, btn, x, y);
    ev(MOUSE_UP, btn, x, y);
}

/// A key.
/// @param k the key
static void press(int k) {
    key(k);
    settle();
}

/// Types text.
/// @param s it
static void type(const char *s) {
    while (*s)
        press((uint8_t)*s++);
}

/// The screen x of a frame (within the view).
/// @param p the frame
/// @return the x
static int fx(uint32_t p) { return WV_X + (int)(((uint64_t)(p - F->view0) * WV_W + F->vlen / 2) / F->vlen); }

/// Drags across the waveform from one frame to another.
/// @param a from
/// @param b to
/// @param btn the button
static void drag(uint32_t a, uint32_t b, int btn) {
    int y = WV_Y + 40;
    ev(MOUSE_DOWN, btn, fx(a), y);
    for (int i = 1; i <= 4; ++i)
        ev(MOUSE_MOVE, btn, fx(a) + (fx(b) - fx(a)) * i / 4, y);
    ev(MOUSE_UP, btn, fx(b), y);
}

/// Clicks a file button.
/// @param b FB_*
static void file_btn(int b) {
    click(4 + b * 63 + 30, FILE_Y + 12, MOUSE_LEFT);
}

/// Clicks one of the buttons above them.
/// @param b TB_*
static void tool_btn(int b) {
    int x = 72;
    for (int i = 0; i < b; ++i)
        if (tools[i].row == tools[b].row)
            x += tools[i].w + 4;
    click(x + tools[b].w / 2, TOOL_Y + 6 + tools[b].row * 30 + 12, MOUSE_LEFT);
}

/// Saves the picture as raw RGB332, with an arrow where the mouse is.
/// @param name the file
/// @param ax the arrow's x (-1: none)
/// @param ay its y
static void snap(const char *name, int ax, int ay) {
    static const char *const a[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
        "X....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
    };
    settle();
    draw();
    if (ax >= 0)
        for (int r = 0; r < 16; ++r)
            for (int c = 0; a[r][c]; ++c)
                if (a[r][c] != ' ')
                    F->g[(ay + r) * W + ax + c] = a[r][c] == 'X' ? C_BG : C_TEXT;
    FILE *f = fopen(name, "wb");
    fwrite(F->g, 1, W * H, f);
    fclose(f);
}

/// Writes a 16-bit WAV file.
/// @param name the file
/// @param rate its rate
/// @param ch its channels
/// @param d the frames
/// @param n how many
static void write_wav(const char *name, uint32_t rate, int ch, const int16_t *d, uint32_t n) {
    uint8_t h[44] = {0};
    uint32_t data = n * ch * 2;
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1;
    h[22] = (uint8_t)ch;
    put32(h + 24, rate);
    put32(h + 28, rate * ch * 2);
    h[32] = (uint8_t)(ch * 2);
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    FILE *f = fopen(name, "wb");
    fwrite(h, 1, 44, f);
    fwrite(d, 2, (size_t)n * ch, f);
    fclose(f);
}

/// The test tone: 2 s at 22050 Hz, 440 Hz at half on the left, 220 Hz at a quarter on the right.
/// @param n set to its frames
/// @return its frames (malloc)
static int16_t *tone(uint32_t *n) {
    *n = 44100;
    int16_t *d = malloc(*n * 4);
    for (uint32_t i = 0; i < *n; ++i) {
        d[i * 2] = (int16_t)lrint(16384 * sin(2 * M_PI * 440 * i / 22050.0));
        d[i * 2 + 1] = (int16_t)lrint(8192 * sin(2 * M_PI * 220 * i / 22050.0));
    }
    return d;
}

/// The screenshots' sound: plucked notes going up, stereo, at 22050 Hz.
static void demo_wav(void) {
    uint32_t n = 22050 * 3;
    int16_t *d = calloc(n * 2, 2);
    static const double notes[6] = {261.6, 329.6, 392.0, 523.3, 392.0, 659.3};
    for (int k = 0; k < 6; ++k) {
        uint32_t at = (uint32_t)(k * 0.45 * 22050) + 1000;
        for (uint32_t i = 0; at + i < n && i < 22050; ++i) {
            double env = exp(-(double)i / 4000.0) * (i < 60 ? i / 60.0 : 1);
            double s = sin(2 * M_PI * notes[k] * i / 22050) + 0.4 * sin(4 * M_PI * notes[k] * i / 22050);
            double pan = k & 1 ? 0.6 : 1.0;
            d[(at + i) * 2] = (int16_t)(d[(at + i) * 2] + 15000 * env * s * pan);
            d[(at + i) * 2 + 1] = (int16_t)(d[(at + i) * 2 + 1] + 15000 * env * s * (1.6 - pan));
        }
    }
    write_wav("DEMO.WAV", 22050, 2, d, n);
    free(d);
}

/// The loudest sample of a part of the sound.
/// @param a the first frame
/// @param b the one after the last
/// @param c the channel
/// @return its size
static int peak(uint32_t a, uint32_t b, int c) {
    int p = 0;
    for (uint32_t i = a; i < b; ++i)
        p = abs(F->d[i * F->ch + c]) > p ? abs(F->d[i * F->ch + c]) : p;
    return p;
}

int main(void) {
    uint32_t n;
    int16_t *orig = tone(&n);
    write_wav("TONE.WAV", 22050, 2, orig, n);
    demo_wav();
    mkdir("SOUNDS", 0755);
    remove("OUT8.WAV");
    remove("OUT16.WAV");
    start();
    F->g = psram_malloc(W * H);
    draw();
    CHECK(F->len == 0 && F->rate == 44100 && F->ch == 1, "new: %u frames %u Hz %d ch", F->len, F->rate, F->ch);

    // Load through the file box: the folder and both files are listed.
    file_btn(FB_LOAD);
    CHECK(F->dlg == DLG_FILE && F->fd_n == 3, "the load box lists %d", F->fd_n);
    type("TONE.WAV");
    press('\r');
    CHECK(F->dlg == DLG_NONE && F->len == n && F->ch == 2 && F->rate == 22050 && F->bits == 16,
          "loaded %u frames %d ch %u Hz %d-bit", F->len, F->ch, F->rate, F->bits);
    CHECK(!memcmp(F->d, orig, n * 4), "the samples differ from the file");
    CHECK(!F->modified && !strcmp(F->path, "/TONE.WAV"), "path '%s' modified %d", F->path, F->modified);
    CHECK(F->view0 == 0 && F->vlen == n, "view %u+%u", F->view0, F->vlen);

    // A selection with the mouse, a quarter to a half of the way.
    drag(n / 4, n / 2, MOUSE_LEFT);
    CHECK(F->sel0 > n / 4 - 100 && F->sel0 < n / 4 + 100 && F->sel1 > n / 2 - 100 && F->sel1 < n / 2 + 100,
          "selected %u to %u", F->sel0, F->sel1);
    uint32_t s0 = F->sel0, s1 = F->sel1, sn = s1 - s0;
    // The right button moves the nearer end.
    click(fx(n * 3 / 5), WV_Y + 40, MOUSE_RIGHT);
    CHECK(F->sel0 == s0 && F->sel1 > n * 3 / 5 - 100, "right button: %u to %u", F->sel0, F->sel1);
    drag(n / 4, n / 2, MOUSE_LEFT);
    CHECK(F->sel0 == s0 && F->sel1 == s1, "the same drag again: %u to %u", F->sel0, F->sel1);

    // Cut, undo, redo, undo.
    tool_btn(TB_CUT);
    CHECK(F->len == n - sn && F->clip_len == sn && F->sel0 == F->sel1 && F->sel0 == s0, "cut: %u frames, clip %u",
          F->len, F->clip_len);
    CHECK(!memcmp(F->d + s0 * 2, orig + s1 * 2, (n - s1) * 4), "cut: what was after it moved wrong");
    CHECK(!memcmp(F->clip, orig + s0 * 2, sn * 4), "cut: the clipboard differs");
    CHECK(F->modified, "not modified after the cut");
    file_btn(FB_UNDO);
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo of the cut");
    CHECK(F->sel0 == s0 && F->sel1 == s1, "undo of the cut: the selection %u to %u", F->sel0, F->sel1);
    file_btn(FB_REDO);
    CHECK(F->len == n - sn, "redo of the cut");
    press('u');
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo again");

    // Paste at the start, then over a selection.
    click(fx(0), WV_Y + 40, MOUSE_LEFT);
    CHECK(F->sel0 == 0 && F->sel1 == 0, "the cursor at %u", F->sel0);
    press(CTRL('v'));
    CHECK(F->len == n + sn && !memcmp(F->d, orig + s0 * 2, sn * 4) && !memcmp(F->d + sn * 2, orig, n * 4),
          "paste at the start");
    CHECK(F->sel0 == 0 && F->sel1 == sn, "pasted, selected %u to %u", F->sel0, F->sel1);
    press(CTRL('z'));
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo of the paste");
    F->sel0 = 1000, F->sel1 = 1100;
    press(CTRL('v'));
    CHECK(F->len == n - 100 + sn && !memcmp(F->d + 1000 * 2, orig + s0 * 2, sn * 4), "paste over 100 frames");
    press(CTRL('z'));
    CHECK(!memcmp(F->d, orig, n * 4) && F->sel0 == 1000 && F->sel1 == 1100, "undo of the paste over");

    // The effects on the selection, each undone.
    F->sel0 = s0, F->sel1 = s1;
    tool_btn(TB_REVERSE);
    CHECK(F->d[s0 * 2] == orig[(s1 - 1) * 2] && F->d[(s1 - 1) * 2 + 1] == orig[s0 * 2 + 1], "reverse");
    CHECK(!memcmp(F->d, orig, s0 * 4) && !memcmp(F->d + s1 * 2, orig + s1 * 2, (n - s1) * 4), "reverse spilled");
    press('r');
    CHECK(!memcmp(F->d, orig, n * 4), "reversed twice isn't the same");
    press('u'), press('u');
    CHECK(!memcmp(F->d, orig, n * 4) && F->nredo == 2, "undo of the reverses");
    tool_btn(TB_GAIN);
    CHECK(F->dlg == DLG_GAIN, "Gain opened %d", F->dlg);
    for (int i = 0; i < 6; ++i)
        press('+');
    CHECK(F->gain_db10 == 60, "the gain is %d", F->gain_db10);
    snap("shot_gain.raw", W / 2 - 230 + 168 + 5 * 46 + 20, 150 + 50);
    press('\r');
    CHECK(peak(s0, s1, 0) > 32000 && peak(s0, s1, 1) > 16000 && peak(s0, s1, 1) < 16500, "+6 dB: peaks %d %d",
          peak(s0, s1, 0), peak(s0, s1, 1));
    press('u');
    tool_btn(TB_NORMAL);
    CHECK(peak(s0, s1, 0) == 32767 && abs(peak(s0, s1, 1) - 16383) < 3, "normalize: peaks %d %d", peak(s0, s1, 0),
          peak(s0, s1, 1));
    CHECK(peak(0, s0, 0) == 16384, "normalize spilled: %d", peak(0, s0, 0));
    press('u');
    tool_btn(TB_FADEIN);
    CHECK(F->d[s0 * 2] == 0 && F->d[s0 * 2 + 1] == 0 && F->d[(s1 - 1) * 2] == orig[(s1 - 1) * 2], "fade in");
    press('u');
    tool_btn(TB_FADEOUT);
    CHECK(F->d[(s1 - 1) * 2] == 0 && F->d[s0 * 2 + 1] == orig[s0 * 2 + 1], "fade out");
    press('u');
    tool_btn(TB_SILENCE);
    CHECK(peak(s0, s1, 0) == 0 && peak(s0, s1, 1) == 0 && peak(s1, n, 0) > 0, "silence");
    press('u');
    CHECK(!memcmp(F->d, orig, n * 4), "the effects didn't all undo");
    // Silence with no selection puts in a second.
    F->sel0 = F->sel1 = 500;
    press('s');
    CHECK(F->len == n + 22050 && peak(500, 500 + 22050, 0) == 0 && !memcmp(F->d + (500 + 22050) * 2, orig + 1000, 4),
          "a second of silence put in");
    press('u');
    // Delete and trim.
    F->sel0 = s0, F->sel1 = s1;
    press(KEY_DEL);
    CHECK(F->len == n - sn, "delete");
    press('u');
    F->sel0 = s0, F->sel1 = s1;
    tool_btn(TB_TRIM);
    CHECK(F->len == sn && !memcmp(F->d, orig + s0 * 2, sn * 4) && F->sel0 == 0 && F->sel1 == sn, "trim");
    press('u');
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo of the trim");
    CHECK(F->sel0 == s0 && F->sel1 == s1, "the selection after undoing the trim: %u to %u", F->sel0, F->sel1);

    // Zoom: to the selection, in around it, and all.
    tool_btn(TB_ZSEL);
    CHECK(F->view0 == s0 && F->vlen == sn, "zoom to the selection: %u+%u", F->view0, F->vlen);
    for (int i = 0; i < 20; ++i)
        press('+');
    CHECK(F->vlen == WV_W / 16, "zoomed right in: %u", F->vlen);
    draw();  // the line between the frames
    press('a');
    CHECK(F->view0 == 0 && F->vlen == n, "zoom all: %u+%u", F->view0, F->vlen);
    // The overview moves the view.
    tool_btn(TB_ZIN);
    tool_btn(TB_ZIN);
    click(OV_X + OV_W - 2, OV_Y + 5, MOUSE_LEFT);
    CHECK(F->view0 + F->vlen == n && F->vlen == n / 4, "the overview: %u+%u", F->view0, F->vlen);
    press(KEY_HOME);
    CHECK(F->view0 == 0 && F->sel0 == 0, "Home");

    // Playing: 22050 Hz at 48 kHz, the selection, once then looping.
    F->sel0 = 22050, F->sel1 = 22050 + 100;
    press(' ');
    CHECK(F->pl_on && source == play_src && F->pl_step == (22050u << 16) / 48000, "play: on %d step %u", F->pl_on,
          F->pl_step);
    int32_t l, r;
    play_src(&l, &r);
    CHECK(l == orig[22050 * 2] && r == orig[22050 * 2 + 1], "the first frame %d %d", l, r);
    play_src(&l, &r);
    int want = orig[22050 * 2] + (int)(((int64_t)(orig[22051 * 2] - orig[22050 * 2]) * F->pl_step) >> 16);
    CHECK(abs(l - want) <= 1, "between frames: %d, %d wanted", l, want);
    int k = 2;
    while (play_src(&l, &r))
        ++k;
    CHECK(!F->pl_on && k > 100 * 48000 / 22050 - 3 && k < 100 * 48000 / 22050 + 3, "played %d frames", k);
    press('l');
    press(' ');
    for (int i = 0; i < 1000; ++i)
        play_src(&l, &r);
    CHECK(F->pl_on && F->pl_pos >= 22050 && F->pl_pos < 22150, "looping: at %u", F->pl_pos);
    press(KEY_ESC);
    CHECK(!F->pl_on && !F->quit, "Esc stops playing first");
    press('l');

    // Format: mono at 44100, resampled; undone exactly.
    file_btn(FB_FORMAT);
    CHECK(F->dlg == DLG_FORMAT && F->f_rate == 22050 && F->f_ch == 2, "the format box");
    click(W / 2 - 280 + 80 + 5 * 66 + 30, 130 + 40 + 10, MOUSE_LEFT);  // 44100
    click(W / 2 - 280 + 80 + 30, 130 + 70 + 10, MOUSE_LEFT);           // mono
    snap("shot_format.raw", W / 2 - 280 + 80 + 30, 130 + 70 + 10);
    press('\r');
    CHECK(F->ch == 1 && F->rate == 44100 && F->len == n * 2, "format: %d ch %u Hz %u frames", F->ch, F->rate, F->len);
    CHECK(abs(F->d[2000] - (orig[2000] + orig[2001]) / 2) <= 1, "mono at 44100: %d", F->d[2000]);
    CHECK(F->sel0 == 44100 && F->sel1 == 44100 + 200, "the selection moved to %u to %u", F->sel0, F->sel1);
    press('u');
    CHECK(F->ch == 2 && F->rate == 22050 && F->len == n && !memcmp(F->d, orig, n * 4), "undo of the format");
    press(CTRL('y'));
    CHECK(F->ch == 1 && F->len == n * 2, "redo of the format");
    press('u');
    // Down to 11025: averaged.
    set_format(11025, 2, 16, true);
    CHECK(F->len == n / 2 && F->d[200] == (orig[400] + orig[402]) / 2, "11025: %u frames, %d", F->len, F->d[200]);
    press('u');
    // Relabelled: the same samples, faster.
    set_format(44100, 2, 16, false);
    CHECK(F->rate == 44100 && F->len == n && !memcmp(F->d, orig, n * 4), "relabel");
    press('u');
    CHECK(F->rate == 22050, "undo of the relabel");

    // Save as 16 and 8 bits, and read them back.
    F->modified = true;
    file_btn(FB_SAVEAS);
    CHECK(F->dlg == DLG_FILE && F->fd_mode == FD_SAVE && !strcmp(F->fd_name, "TONE.WAV"), "Save as: '%s'",
          F->fd_name);
    for (int i = 0; i < 8; ++i)
        press(8);
    type("OUT16");
    press('\r');
    CHECK(!F->modified && !strcmp(F->path, "/OUT16.WAV"), "saved as '%s'", F->path);
    set_format(22050, 2, 8, true);
    CHECK(F->bits == 8 && F->len == n, "8-bit");
    file_btn(FB_SAVEAS);
    for (int i = 0; i < 9; ++i)
        press(8);
    type("OUT8.WAV");
    press('\r');
    CHECK(!strcmp(F->path, "/OUT8.WAV"), "saved as '%s'", F->path);
    CHECK(load("/OUT16.WAV") && F->len == n && F->bits == 16 && !memcmp(F->d, orig, n * 4), "OUT16.WAV read back");
    CHECK(load("/OUT8.WAV") && F->len == n && F->bits == 8, "OUT8.WAV read back");
    int worst = 0;
    for (uint32_t i = 0; i < n * 2; ++i)
        worst = abs(F->d[i] - orig[i]) > worst ? abs(F->d[i] - orig[i]) : worst;
    CHECK(worst <= 128, "8-bit: off by %d", worst);
    FILINFO fi;
    CHECK(f_stat("/OUT8.WAV", &fi) == FR_OK && fi.fsize == 44 + n * 2, "OUT8.WAV is %lu bytes", fi.fsize);

    // The checksums: all well, then a bit that changed by itself.
    CHECK(load("/TONE.WAV"), "TONE.WAV again");
    refresh(-1);
    refresh(-1);
    CHECK(!F->damaged, "damaged with nothing wrong");
    F->d[12345] ^= 0x40;  // not through the editor
    refresh(-1);
    CHECK(F->damaged, "the changed bit wasn't found");
    F->modified = true;
    file_btn(FB_SAVE);
    CHECK(F->dlg == DLG_FILE, "Save over the file when damaged: dlg %d", F->dlg);
    press(KEY_ESC);

    // Running out of memory: undo steps make way, then a paste is refused.
    CHECK(load("/TONE.WAV") && !F->damaged, "TONE.WAV once more");
    for (int i = 0; i < 4; ++i) {
        F->sel0 = 0, F->sel1 = n;
        press('r');
    }
    CHECK(F->nundo == 4, "%d undo steps", F->nundo);
    gfree(F->clip);
    F->clip = NULL;
    F->clip_len = 0;
    pc_psram_limit = psram_used + n * 4 - 1000;  // not quite room for a copy
    F->sel0 = 0, F->sel1 = n;
    press(CTRL('c'));
    CHECK(F->clip_len == n && F->nundo == 3, "copy made room: clip %u, %d undo steps", F->clip_len, F->nundo);
    F->sel0 = F->sel1 = 0;
    press(CTRL('v'));
    CHECK(F->len == 2 * n && F->nundo <= 3, "paste with the undo steps making way: %u, %d steps", F->len, F->nundo);
    pc_psram_limit = psram_used + 1000;
    press(CTRL('v'));
    CHECK(F->len == 2 * n, "a paste with no room changed it: %u", F->len);
    press('u');
    CHECK(F->len == 2 * n && F->nundo, "undo without the memory to keep the redo step");
    pc_psram_limit = 8u << 20;
    press('u');
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo after running out");

    // Effects. Echo of a click: 10 ms (220 frames), half fed back, at full level.
    CHECK(load("/TONE.WAV"), "TONE.WAV for the effects");
    press(CTRL('a'));
    press('s');
    F->d[1000 * 2] = 16384;
    gtouch(F->d, n * 4);
    F->sel0 = 0, F->sel1 = 2000;
    tool_btn(TB_EFFECTS);
    CHECK(F->dlg == DLG_EFFECTS && F->fx == E_ECHO, "Effects opened %d on %d", F->dlg, F->fx);
    for (int i = 0; i < 40; ++i)
        press(KEY_LEFT);  // the delay down to 10 ms
    press('\t');
    click(FXB_X + 140 + 270 + 15, FXB_Y + 64 + 30 + 10, MOUSE_LEFT);  // feedback + 5: 45
    press(KEY_RIGHT);                                                   // 50
    press('\t');
    for (int i = 0; i < 10; ++i)
        press(KEY_RIGHT);  // level 100
    CHECK(F->fxp[E_ECHO][0] == 10 && F->fxp[E_ECHO][1] == 50 && F->fxp[E_ECHO][2] == 100, "echo settings %d %d %d",
          F->fxp[E_ECHO][0], F->fxp[E_ECHO][1], F->fxp[E_ECHO][2]);
    press('\r');
    CHECK(F->len == n && F->d[1000 * 2] == 16384 && F->d[1220 * 2] == 16384 && F->d[1440 * 2] == 8192 &&
              F->d[1660 * 2] == 4096 && F->d[2100 * 2] == 1024 && F->d[1220 * 2 + 1] == 0,
          "echo: %d %d %d %d %d", F->d[1000 * 2], F->d[1220 * 2], F->d[1440 * 2], F->d[1660 * 2], F->d[2100 * 2]);
    CHECK(F->d[1221 * 2] == 0 && F->d[4300 * 2] == 0, "echo: something between the repeats");
    press('u');
    CHECK(F->d[1220 * 2] == 0 && F->d[1000 * 2] == 16384, "undo of the echo");
    // Reverb on all of the tone: the tail makes it longer.
    CHECK(load("/TONE.WAV"), "TONE.WAV again");
    press('e');
    press(KEY_DOWN);
    CHECK(F->fx == E_REVERB, "Down chose %d", F->fx);
    press('\r');
    uint32_t rtail = (uint32_t)((0.5f + 2.5f * 0.7f) * 22050);
    CHECK(F->len == n + rtail && peak(n, n + 2000, 0) > 500 && peak(n + rtail - 100, n + rtail, 0) < 200,
          "reverb: %u frames, tail %d then %d", F->len, peak(n, n + 2000, 0), peak(n + rtail - 100, n + rtail, 0));
    press('u');
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "undo of the reverb");
    // The filters: high-pass at 3 kHz all but takes out 440 and 220 Hz; low-pass at 5 kHz leaves them.
    press('e');
    F->fx = E_HIGHPASS;
    F->fxp[E_HIGHPASS][0] = 3000;
    press('\r');
    CHECK(peak(2000, n, 0) < 1200 && peak(2000, n, 1) < 200, "high-pass: %d %d", peak(2000, n, 0), peak(2000, n, 1));
    press('u');
    press('e');
    F->fx = E_LOWPASS;
    F->fxp[E_LOWPASS][0] = 5000;
    press('\r');
    CHECK(abs(peak(2000, n, 0) - 16384) < 400 && abs(peak(2000, n, 1) - 8192) < 200, "low-pass: %d %d",
          peak(2000, n, 0), peak(2000, n, 1));
    press('u');
    // Distortion makes the quiet side louder, never past full scale; the rest just run.
    press('e');
    F->fx = E_DISTORT;
    press('\r');
    CHECK(peak(0, n, 1) > 20000 && peak(0, n, 0) <= 32767, "distortion: %d", peak(0, n, 1));
    press('u');
    static const int others[3] = {E_CHORUS, E_FLANGER, E_TREMOLO};
    for (int i = 0; i < 3; ++i) {
        press('e');
        F->fx = others[i];
        press('\r');
        CHECK(F->len == n + (others[i] == E_TREMOLO ? 0 : 22050 / 20) && peak(0, n, 0) > 4000, "%s: %u frames",
              fx_defs[others[i]].name, F->len);
        press('u');
    }
    CHECK(F->len == n && !memcmp(F->d, orig, n * 4), "the effects didn't all undo");

    // The screenshots: the demo, part selected, playing.
    CHECK(load("/DEMO.WAV"), "DEMO.WAV");
    F->modified = false;
    zoom_to(0, 22050 * 2);
    drag(22050 * 45 / 100 + 900, 22050 * 90 / 100 + 900, MOUSE_LEFT);
    F->pl_on = true;
    F->pl_pos = 22050 * 70 / 100;
    F->ph_x = frame_x(F->pl_pos);
    press(CTRL('c'));
    ev(MOUSE_MOVE, 0, fx(22050 * 140 / 100), WV_Y + 60);
    snap("shot_main.raw", fx(22050 * 140 / 100), WV_Y + 60);
    F->pl_on = false;
    F->ph_x = -1;
    file_btn(FB_LOAD);
    press(KEY_DOWN), press(KEY_DOWN);
    snap("shot_load.raw", 300, 150);
    press(KEY_ESC);
    press(KEY_DEL);
    press(KEY_ESC);
    CHECK(F->dlg == DLG_ASK && !F->quit, "Esc with changes didn't ask");
    snap("shot_ask.raw", 330, 248);
    press(KEY_ESC);
    press('e');
    click(FXB_X + 16 + 50, FXB_Y + 34 + E_REVERB * 22 + 10, MOUSE_LEFT);
    snap("shot_effects.raw", FXB_X + 16 + 50, FXB_Y + 34 + E_REVERB * 22 + 10);
    press(KEY_ESC);
    press(KEY_F1);
    snap("shot_help.raw", -1, -1);
    press(' ');
    CHECK(F->dlg == DLG_NONE && !F->pl_on, "a key closes the help without playing");

    // wav info.
    info("/TONE.WAV");
    stop();
    CHECK(psram_used == 0, "%zu bytes of PSRAM left after stopping", psram_used);
    free(orig);
    printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}
