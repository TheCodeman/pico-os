/**
 * @file uitest.c
 * @brief The SDR receiver on a PC, with a pretend dongle whose samples carry FM and AM stations: tunes by keys, the mouse, the digits and typing, checks the sound, squelch, favourites (and /SDR.INI), the scan, and writes screenshots.
 *
 *     ./uitest.sh  (builds this, writes sdr_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdarg.h>

#include "../../src/sdr/sdr_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock
static bool (*source)(int32_t *, int32_t *);  ///< the audio source
static void (*job)(void);                     ///< core 1's job
static uint8_t overlay[25 * 1024];            ///< the apps' SRAM area

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
uint32_t time_us_32(void) { return (uint32_t)now_us; }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }
void sleep_us(uint64_t us) { now_us += us; }
void audio_set_source(bool (*fn)(int32_t *, int32_t *)) { source = fn; }
bool (*audio_get_source(void))(int32_t *, int32_t *) { return source; }
void core1_job_start(void (*fn)(void)) { job = fn; }
void core1_job_stop(void) { job = NULL; }
void (*core1_job_now(void))(void) { return job; }
void *app_overlay_claim(int app, size_t *size, void (*release)(void)) {
    (void)app, (void)release;
    *size = sizeof overlay;
    return overlay;
}

size_t pc_psram_limit = 8u << 20;  ///< the PSRAM "free"
static size_t psram_used;          ///< and used
void *psram_malloc(size_t n) {
    if (psram_used + n > pc_psram_limit)
        return NULL;
    size_t *p = malloc(n + 16);
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

FRESULT f_open(FIL *f, const char *path, int mode) {
    char buf[300];
    snprintf(buf, sizeof buf, ".%s", path);
    f->f = fopen(buf, mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }
char *f_gets(char *buf, int n, FIL *f) { return fgets(buf, n, f->f); }
int f_printf(FIL *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f->f, fmt, ap);
    va_end(ap);
    return r;
}

// ---- The pretend dongle -----------------------------------------------------------------------------

/// The stations on the air.
static const struct {
    uint32_t hz;
    int mode;
    double amp;
} stations[] = {
    {99900000, DSP_WFM, 0.10},   {100000000, DSP_WFM, 0.5}, {100100000, DSP_WFM, 0.08},
    {145450000, DSP_NFM, 0.05},  {145525000, DSP_NFM, 0.4}, {145575000, DSP_NFM, 0.1},
    {145600000, DSP_NFM, 0.02},  {1215000, DSP_AM, 0.4},
};
#define NSTATIONS (int)(sizeof stations / sizeof *stations)  ///< how many

static bool plugged = true;       ///< the dongle is there
static uint32_t tuned, rate_set;  ///< what it was told
static uint64_t pairs_sent;       ///< samples it has sent
static double phase[NSTATIONS];   ///< each station's carrier phase
static double mod_t;              ///< the modulating tone's time
static uint8_t buf[RTL_SLOT_BYTES];  ///< the buffer handed out
static bool buf_out;              ///< and not yet given back
static int tunes;                 ///< times rtl_set_freq was called

bool rtl_usb_present(void) { return plugged; }
void rtl_usb_ids(uint16_t *vid, uint16_t *pid) { *vid = 0x0bda, *pid = 0x2838; }
uint16_t rtl_usb_packet_size(void) { return 64; }
void rtl_usb_job(void) {}
int rtl_usb_control(uint8_t t, uint8_t r, uint16_t v, uint16_t i, void *d, uint16_t l) { (void)t, (void)r, (void)v, (void)i, (void)d; return l; }
void rtl_usb_hold(bool on) { (void)on; }
bool rtl_usb_stream_start(uint8_t *ring) { (void)ring; pairs_sent = now_us * rate_set / 1000000; return plugged; }
void rtl_usb_stream_stop(void) {}
uint32_t rtl_usb_overruns(void) { return 0; }
uint32_t rtl_usb_bytes(void) { return (uint32_t)(pairs_sent * 2); }

const uint8_t *rtl_usb_read(uint32_t *len) {
    if (!plugged || !rate_set)
        return NULL;
    uint64_t due = now_us * rate_set / 1000000;
    if (pairs_sent + RTL_SLOT_BYTES / 2 > due)
        return NULL;
    // A buffer: each station near the tuned frequency, modulated by 1 kHz, and noise.
    int n = RTL_SLOT_BYTES / 2;
    for (int k = 0; k < n; ++k) {
        double t = mod_t + (double)k / rate_set, tone = sin(2 * M_PI * 1000 * t);
        double i = 0, q = 0;
        for (int s = 0; s < NSTATIONS; ++s) {
            double off = (double)stations[s].hz - tuned;
            if (fabs(off) > rate_set)
                continue;
            if (stations[s].mode == DSP_AM) {
                double e = stations[s].amp * (1 + 0.5 * tone);
                phase[s] += 2 * M_PI * off / rate_set;
                i += e * cos(phase[s]), q += e * sin(phase[s]);
            } else {
                double dev = stations[s].mode == DSP_WFM ? 40000 : 2500;
                phase[s] += 2 * M_PI * (off + dev * tone) / rate_set;
                i += stations[s].amp * cos(phase[s]), q += stations[s].amp * sin(phase[s]);
            }
        }
        i += 0.01 * (2.0 * rand() / RAND_MAX - 1), q += 0.01 * (2.0 * rand() / RAND_MAX - 1);
        int a = (int)lrint(127.4 + 127 * i), b = (int)lrint(127.4 + 127 * q);
        buf[2 * k] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
        buf[2 * k + 1] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
    mod_t += (double)n / rate_set;
    pairs_sent += n;
    buf_out = true;
    *len = RTL_SLOT_BYTES;
    return buf;
}
void rtl_usb_done(void) { buf_out = false; }

bool rtl_open(rtl_dev_t *d, char *err, int errlen) {
    memset(d, 0, sizeof *d);
    if (!plugged) {
        snprintf(err, errlen, "no RTL-SDR dongle plugged in");
        return false;
    }
    d->tuner = RTL_TUNER_R820T;
    snprintf(d->maker, sizeof d->maker, "Realtek");
    snprintf(d->product, sizeof d->product, "RTL2838UHIDIR");
    d->gain = RTL_GAIN_AUTO;
    return true;
}
void rtl_close(rtl_dev_t *d) { (void)d; }
bool rtl_set_rate(rtl_dev_t *d, uint32_t hz) { d->rate = rate_set = hz; return true; }
bool rtl_set_freq(rtl_dev_t *d, uint32_t hz) {
    d->freq = tuned = hz;
    d->direct = hz < 24000000;
    ++tunes;
    return true;
}
bool rtl_set_gain(rtl_dev_t *d, int g) { d->gain = g; return true; }
bool rtl_set_ppm(rtl_dev_t *d, int ppm) { d->ppm = ppm; return true; }
bool rtl_set_bias_tee(rtl_dev_t *d, bool on) { d->bias_tee = on; return true; }
bool rtl_stream(rtl_dev_t *d, bool on) { (void)d, (void)on; return true; }
const int16_t *rtl_gains(int *n) {
    static const int16_t g[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
    *n = (int)(sizeof g / sizeof *g);
    return g;
}
const char *rtl_tuner_name(rtl_tuner_t t) { return t == RTL_TUNER_R820T ? "R820T" : "R828D"; }

// ---- The test ---------------------------------------------------------------------------------------

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

/// What the receiver's loop does: draws again if it needs to.
static void settle(void) {
    if (S->dirty) {
        S->dirty = false;
        draw();
    }
}

/// Lets `ms` of samples come and go: demodulating, the spectrum, the sound
/// played (as core 1 would), and the scan.
/// @param ms how long
static void run(int ms) {
    for (int t = 0; t < ms; t += 5) {
        sleep_ms(5);
        pump();
        for (int i = 0; i < 240 && source; ++i) {  // 5 ms of 48 kHz
            int32_t l, r;
            source(&l, &r);
        }
        scan_step();
        if ((int32_t)(now_ms() - S->next_spec) >= 0 && spectrum_step())
            S->next_spec = now_ms() + SPEC_EVERY_MS;
    }
    settle();
}

/// A mouse event.
/// @param type MOUSE_*
/// @param btn the button
/// @param x where
/// @param y where
/// @param wheel the wheel's steps
static void ev(int type, int btn, int x, int y, int wheel) {
    mouse_event_t e = {(uint8_t)type, (uint8_t)btn, (uint8_t)(type == MOUSE_UP ? 0 : btn), (int8_t)wheel,
                       (int16_t)x, (int16_t)y, false};
    mouse(&e);
    settle();
}

/// A click.
/// @param x where
/// @param y where
static void click(int x, int y) {
    ev(MOUSE_DOWN, MOUSE_LEFT, x, y, 0);
    ev(MOUSE_UP, MOUSE_LEFT, x, y, 0);
}

/// A key.
/// @param k it
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

/// Clicks a bottom button.
/// @param b BB_*
static void bottom(int b) {
    click(4 + b * 63 + 30, FILE_Y + 12);
}

/// How loud the sound in the ring is, of the last `n` samples (RMS, of 1).
/// @param n how many
/// @return the level
static double sound_level(int n) {
    double e = 0;
    for (int i = 1; i <= n; ++i) {
        double v = aring[(a_in - i) % AUD_N] / 32768.0;
        e += v * v;
    }
    return sqrt(e / n);
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
    draw();
    if (ax >= 0)
        for (int r = 0; r < 16; ++r)
            for (int c = 0; a[r][c]; ++c)
                if (a[r][c] != ' ')
                    S->g[(ay + r) * W + ax + c] = a[r][c] == 'X' ? C_BG : C_TEXT;
    FILE *f = fopen(name, "wb");
    fwrite(S->g, 1, W * H, f);
    fclose(f);
}

int main(void) {
    srand(2);
    remove("SDR.INI");
    // No dongle: the box says so.
    plugged = false;
    CHECK(start(), "start");
    S->next_try = 0;
    CHECK(!start_dongle(), "started without a dongle");
    S->dirty = true;
    settle();
    snap("shot_nodongle.raw", -1, -1);
    plugged = true;
    CHECK(start_dongle(), "start_dongle: %s", S->err);
    CHECK(S->running && source == sdr_src, "it should be playing");
    CHECK(S->freq == 100000000 && S->mode == DSP_WFM, "the defaults: %lu, mode %d", (unsigned long)S->freq, S->mode);

    // Wide FM at 100 MHz: the tone comes out.
    run(7000);
    double lv = sound_level(4800);
    printf("WFM 100.0: level %.3f, %.1f dBFS, CPU %d.%d%% (PC)\n", lv, (double)dsp_level_db(S->dsp), S->load / 10, S->load % 10);
    CHECK(lv > 0.05, "no sound on the WFM station: %.3f", lv);
    snap("shot_main.raw", 330, 150);

    // Keys: a step up (100 kHz) and back.
    press(KEY_RIGHT);
    CHECK(S->freq == 100100000 && S->centre == 100100000, "Right: %lu", (unsigned long)S->freq);
    press(KEY_LEFT);
    CHECK(S->freq == 100000000, "Left: %lu", (unsigned long)S->freq);
    // The wheel over the 1 MHz digit (place 6: the fourth character).
    ev(MOUSE_WHEEL, 0, DIG_X + 3 * DIG_W + 5, DIG_Y + 10, 1);
    CHECK(S->freq == 101000000, "wheel on the MHz digit: %lu", (unsigned long)S->freq);
    ev(MOUSE_WHEEL, 0, DIG_X + 3 * DIG_W + 5, DIG_Y + 10, -1);
    CHECK(S->freq == 100000000, "wheel back: %lu", (unsigned long)S->freq);

    // Typing a frequency, and NFM.
    type("145.5");
    CHECK(S->typing, "typing");
    press('\r');
    CHECK(S->freq == 145500000, "typed 145.5: %lu", (unsigned long)S->freq);
    click(10 + 54 + 25, ROW_A + 10);  // NFM
    CHECK(S->mode == DSP_NFM, "the NFM button");
    for (int i = 0; i < NSTEPS; ++i)
        if (steps[i] == 12500)
            S->step = i;
    S->dirty = true;
    run(500);
    // A click on the spectrum where the strong station is (145.525).
    int x = freq_x(145525000);
    int before = tunes;
    click(x, SP_Y + 50);
    CHECK(S->freq == 145525000, "the spectrum click: %lu", (unsigned long)S->freq);
    CHECK(tunes == before, "a click within the band shouldn't retune the dongle");
    CHECK(S->dsp->offset_hz == 25000, "the channel's offset: %ld", (long)S->dsp->offset_hz);
    run(3000);
    lv = sound_level(4800);
    printf("NFM 145.525: level %.3f, %.1f dBFS\n", lv, (double)dsp_level_db(S->dsp));
    CHECK(lv > 0.05, "no sound on the NFM station: %.3f", lv);
    // The squelch: open on the station, shut on 145.500 (nothing there).
    ev(MOUSE_DOWN, MOUSE_LEFT, SQ_X + 5 + (int)((SL_W - 10) * 0.55f), ROW_B + 10, 0);  // -45 dB
    ev(MOUSE_UP, MOUSE_LEFT, SQ_X + 5 + (int)((SL_W - 10) * 0.55f), ROW_B + 10, 0);
    CHECK(S->squelch == -45, "the squelch slider: %d", S->squelch);
    run(500);
    CHECK(S->dsp->sq_open, "the squelch should be open on the station (%.1f dBFS)", (double)dsp_level_db(S->dsp));
    // Waterfall time, then the screenshot.
    run(5000);
    snap("shot_nfm.raw", x + 40, WF_Y + 40);
    press(KEY_LEFT), press(KEY_LEFT);
    CHECK(S->freq == 145500000, "two steps down: %lu", (unsigned long)S->freq);
    run(800);
    printf("NFM 145.500 (empty): %.1f dBFS, squelch %s, level %.4f\n", (double)dsp_level_db(S->dsp),
           S->dsp->sq_open ? "open" : "shut", sound_level(2400));
    CHECK(!S->dsp->sq_open, "the squelch should be shut on an empty channel");
    CHECK(sound_level(2400) < 0.002, "sound through a shut squelch: %.4f", sound_level(2400));

    // Favourites: three, one renamed, one deleted; kept in the INI.
    bottom(BB_ADD);
    CHECK(S->dlg == DLG_NAME, "Add fav asks for a name");
    type("Empty");
    press('\r');
    tune(145525000);
    press('f');
    type("Repeater");
    press('\r');
    tune(145575000);
    press('f');
    type("Simplex");
    press('\r');
    CHECK(S->nfavs == 3, "favourites: %d", S->nfavs);
    bottom(BB_FAVS);
    CHECK(S->dlg == DLG_FAVS && S->fav_sel == 2, "the box on the one tuned: %d", S->fav_sel);
    snap("shot_favs.raw", FB_X + 100, FB_Y + 60);
    click(FB_X + 8 + FV_RENAME * 77 + 30, FB_Y + FB_H - 20);
    press(8), press(8), press(8), press(8), press(8), press(8), press(8);
    type("Calling");
    press('\r');
    CHECK(!strcmp(S->favs[2].name, "Calling"), "renamed: %s", S->favs[2].name);
    click(FB_X + 30, FB_Y + 28 + 2 + 0 * 18 + 5);  // the first row
    click(FB_X + 8 + FV_DELETE * 77 + 30, FB_Y + FB_H - 20);
    CHECK(S->nfavs == 2 && S->favs[0].hz == 145525000, "deleted: %d, %lu", S->nfavs, (unsigned long)S->favs[0].hz);
    press(KEY_ESC);
    CHECK(S->dlg == DLG_NONE, "Esc closes the box");

    // Scan: from an empty channel to the station.
    tune(145500000);
    run(300);
    bottom(BB_SCAN);
    CHECK(S->scanning, "scanning");
    run(1500);
    CHECK(S->freq == 145525000 && S->dsp->sq_open, "the scan should stop on the repeater: %lu", (unsigned long)S->freq);
    bottom(BB_SCAN);
    CHECK(!S->scanning, "scan off");

    // AM below 24 MHz (direct sampling on an R820T dongle).
    type("1.215");
    press('\r');
    press('m');  // NFM -> AM
    CHECK(S->mode == DSP_AM && S->dev.direct, "AM, direct: mode %d, direct %d", S->mode, S->dev.direct);
    for (int i = 0; i < NSTEPS; ++i)
        if (steps[i] == 9000)
            S->step = i;
    run(3000);
    lv = sound_level(4800);
    printf("AM 1.215: level %.3f, %.1f dBFS\n", lv, (double)dsp_level_db(S->dsp));
    CHECK(lv > 0.05, "no sound on the AM station: %.3f", lv);

    // Help.
    bottom(BB_HELP);
    snap("shot_help.raw", -1, -1);
    press('x');
    CHECK(S->dlg == DLG_NONE, "a key closes the help");

    // Unplugged: it stops, and says so.
    tune(100000000);
    set_mode(DSP_WFM);
    run(300);
    plugged = false;
    stop_dongle();
    CHECK(!S->running && source == NULL, "unplugged: still running");

    // The INI keeps the settings and favourites.
    save_ini();
    stop();
    CHECK(start(), "start again");
    CHECK(S->nfavs == 2 && !strcmp(S->favs[1].name, "Calling") && S->freq == 100000000 && S->squelch == -45 &&
              S->mode == DSP_WFM,
          "the INI: %d favs, %lu, squelch %d, mode %d", S->nfavs, (unsigned long)S->freq, S->squelch, S->mode);
    stop();

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
