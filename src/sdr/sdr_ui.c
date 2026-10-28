/**
 * @file sdr_ui.c
 * @brief The `sdr` command: a receiver for an RTL-SDR dongle (RTL2832U with an R820T or R828D tuner) on the USB hub, in the tracker's style (bevelled panels drawn off the screen, the scanout mouse arrow), playing through HDMI audio.
 *
 * The screen: the frequency in big digits (the wheel over a digit changes
 * it), the mode, step, signal meter and squelch; the spectrum of the
 * 240 kHz the dongle sends (click to tune) and a waterfall of it; the
 * mode, step, gain, volume, squelch, tuning, rate, correction and
 * de-emphasis controls; and along the bottom, as in the sprite editor, the
 * favourites, scan, help and quit buttons.
 *
 * Over the RP2350's full-speed USB the dongle runs at 240 or 288 kS/s (2
 * bytes a sample: 480 or 576 KB/s of the bus's 1.2 MB/s), enough for one
 * broadcast FM station (mono), narrow FM or AM. The samples come into a
 * ring of buffers in the apps' SRAM area (app_overlay.h) on core 1; the
 * demodulating is on core 0 between the screen's work (sdr_dsp.c); the audio
 * goes to core 1's HDMI sound through a ring in PSRAM.
 *
 * Memory: the state, the picture, the waterfall and the audio ring are in
 * PSRAM while it runs; the sample ring and the demodulator's state are in
 * the apps' SRAM area and the spectrum in the SRAM heap (or PSRAM), freed on
 * leaving. The settings and favourites are kept in /SDR.INI.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_overlay.h"
#include "audio.h"
#include "console.h"
#include "core1_job.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "rtl2832.h"
#include "rtl_usb.h"
#include "sdr.h"
#include "sdr_dsp.h"

#define W DVI_WIDTH              ///< the screen's width
#define H DVI_HEIGHT             ///< its height
#define INI_PATH "/SDR.INI"      ///< the settings file
#define MAX_FAVS 40              ///< favourites kept
#define NAME_LEN 20              ///< a favourite's name
#define AUD_N    8192            ///< the audio ring's samples (170 ms)
#define AUD_START 2400           ///< playing starts (again) with this much in it (50 ms)
#define AUD_HIGH 6000            ///< above this a sample is dropped now and then (the clocks differ)
#define FREQ_MIN 500000u         ///< the lowest frequency tuned
#define FREQ_MAX 1766000000u     ///< the highest
#define SPEC_EVERY_MS 50         ///< a spectrum line this often
#define SCAN_DWELL_MS 150        ///< scan: listening on each favourite
#define SCAN_HANG_MS 2000        ///< and staying this long after a signal goes

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< the frequency, titles
#define C_CYAN   0x1f  ///< typed text, the spectrum's line
#define C_GREEN  0x3c  ///< a signal, squelch open
#define C_ORANGE 0xf4  ///< warnings
#define C_RED    0xe0  ///< the channel's line, errors
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the selected row
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip
#define C_GRID   0x25  ///< the spectrum's grid
#define C_SPEC   0x0d  ///< the spectrum's body
#define C_CHAN   0x06  ///< the channel's band behind the spectrum
#define C_UNLIT  0x24  ///< unlit digits

/// Layout.
#define TOP_H   24     ///< the title strip
#define DISP_Y  27     ///< the frequency panel
#define DISP_H  68     ///< its height
#define DIG_X   16     ///< the big digits' left
#define DIG_Y   37     ///< their top
#define DIG_W   24     ///< a big character's width (the font at 3x)
#define DIG_H   48     ///< and height
#define DIG_N   12     ///< characters: dddd.ddd.ddd
#define INFO_X  372    ///< the panel's right part
#define SP_X    8      ///< the spectrum's left
#define SP_W    624    ///< its width (a column a pixel)
#define SP_Y    100    ///< its top
#define SP_H    100    ///< its height
#define SCALE_Y 201    ///< the frequency scale under it
#define WF_Y    222    ///< the waterfall's top
#define WF_H    120    ///< its rows
#define CTL_Y   346    ///< the controls' panel
#define ROW_A   351    ///< its rows
#define ROW_B   380
#define ROW_C   409
#define FILE_Y  444    ///< the buttons along the bottom
#define VOL_X   72     ///< the volume slider
#define SQ_X    384    ///< the squelch slider
#define SL_W    176    ///< a slider's width
#define SPAN_DB 60     ///< the spectrum's height in dB

/// What a click hits.
enum {
    HIT_DIGIT = 1, HIT_SPEC, HIT_MODE, HIT_STEP, HIT_GAIN, HIT_BIAS, HIT_VOL, HIT_MUTE, HIT_SQ, HIT_TUNE, HIT_RATE,
    HIT_PPM, HIT_DEEMPH, HIT_BTN, HIT_FAVROW, HIT_FAVBTN, HIT_SWALLOW,
};

/// The bottom row's buttons.
enum { BB_FAVS, BB_ADD, BB_PREV, BB_NEXT, BB_SCAN, BB_FREQ, BB_HELP, BB_QUIT, BB_COUNT };
/// Their labels.
static const char *const bb_names[BB_COUNT] = {"Favs", "Add fav", "< Fav", "Fav >", "Scan", "Freq", "Help", "Quit"};

/// The favourites box's buttons.
enum { FV_TUNE, FV_RENAME, FV_DELETE, FV_UP, FV_DOWN, FV_CLOSE, FV_COUNT };
/// Their labels.
static const char *const fv_names[FV_COUNT] = {"Tune", "Rename", "Delete", "Up", "Down", "Close"};

/// Dialogs.
enum { DLG_NONE, DLG_FAVS, DLG_NAME, DLG_HELP };

/// The modes' names.
static const char *const mode_names[DSP_MODES] = {"WFM", "NFM", "AM"};

/// Tuning steps.
static const uint32_t steps[] = {100, 1000, 5000, 6250, 9000, 10000, 12500, 25000, 50000, 100000, 200000, 1000000};
#define NSTEPS (int)(sizeof steps / sizeof *steps)  ///< how many

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A favourite.
typedef struct {
    uint32_t hz;          ///< its frequency
    uint8_t mode;         ///< DSP_*
    char name[NAME_LEN];  ///< its name, or ""
} fav_t;

/// Everything the receiver keeps (PSRAM, while it runs).
typedef struct {
    rtl_dev_t dev;          ///< the dongle
    bool running;           ///< it is set up and sending
    char err[64];           ///< why it isn't
    uint32_t next_try;      ///< when to look for it again
    uint8_t *ring;          ///< the sample ring (the apps' SRAM area)
    dsp_t *dsp;             ///< the demodulator (the apps' SRAM area)
    dsp_spec_t *spec;       ///< the spectrum (SRAM heap, or PSRAM)
    bool spec_psram;        ///< it is in PSRAM
    // Settings (kept in the INI file).
    uint32_t freq;          ///< the channel's frequency
    uint32_t centre;        ///< the frequency the dongle is tuned to
    int mode;               ///< DSP_*
    int step;               ///< index into steps
    int gain;               ///< index into the gain steps, -1 for auto
    int squelch;            ///< dBFS, -100: off
    int volume;             ///< 0-100
    bool muted;             ///< no sound
    uint32_t rate;          ///< 240000 or 288000
    int ppm;                ///< the crystal correction
    int deemph;             ///< 50 or 75 us
    bool bias;              ///< the bias tee
    fav_t favs[MAX_FAVS];   ///< favourites
    int nfavs;              ///< their count
    // The display.
    uint8_t *g;             ///< the off-screen picture (PSRAM)
    uint8_t *t;             ///< where drawing goes (g, or the screen)
    uint8_t *wf;            ///< the waterfall's rows (PSRAM), newest at wf_top
    int wf_top;             ///< the newest row
    int8_t col[SP_W];       ///< the spectrum's columns (dB)
    float floor_db;         ///< the noise floor, for the scale
    bool have_spec;         ///< col has something
    uint32_t next_spec;     ///< when the next spectrum line is due
    uint32_t next_meter;    ///< and the meter
    hit_t hits[96];         ///< places to click
    int nhits;              ///< their count
    bool collect;           ///< hits are being collected
    bool dirty;             ///< draw everything again
    bool quit;              ///< leave
    int dlg;                ///< DLG_*
    int fav_sel, fav_top;   ///< the favourites box's selection and first row shown
    char typed[16];         ///< a frequency being typed (MHz)
    bool typing;            ///< it is
    char naming[NAME_LEN];  ///< a favourite's name being typed
    int naming_fav;         ///< which (-1: a new one)
    int drag;               ///< HIT_VOL or HIT_SQ while dragging a slider
    char msg[96];           ///< a message for the title strip
    uint32_t msg_until;     ///< until when
    bool scanning;          ///< going round the favourites
    int scan_at;            ///< the favourite listened to
    uint32_t scan_next;     ///< when to move on
    uint32_t busy_us;       ///< time demodulating since busy_since
    uint32_t busy_since;    ///< when that started
    int load;               ///< the demodulating's share of core 0 (tenths of a %)
    uint8_t wf_pal[64];     ///< the waterfall's colours: dark blue to cyan, yellow, red and white
    int16_t abuf[512];      ///< one buffer's audio
} sdr_t;

static sdr_t *S;  ///< the state, while the receiver runs

/// The audio ring (PSRAM), written by core 0 and played by core 1.
static int16_t *aring;
/// Samples ever put in and taken out.
///@{
static volatile uint32_t a_in, a_out;
///@}
static volatile bool a_playing;    ///< the ring is being played
static volatile uint32_t a_underruns;  ///< times it ran dry

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pump(void);

// ---- Drawing --------------------------------------------------------------------------------------

/// Fills a rectangle (clipped to the screen).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int r = 0; r < h; ++r)
        memset(S->t + (y + r) * W + x, c, w);
}

/// Draws a character of the 8x16 font, `scale` times as big.
/// @param x left
/// @param y top
/// @param c the character
/// @param fg its colour
/// @param bg the background
/// @param scale 1, 2, 3...
static void chs(int x, int y, uint8_t c, uint8_t fg, uint8_t bg, int scale) {
    if (x < 0 || y < 0 || x > W - 8 * scale || y > H - 16 * scale)
        return;
    const uint8_t *gl = font8x16[c];
    for (int r = 0; r < 16 * scale; ++r) {
        uint8_t *p = S->t + (y + r) * W + x, bits = gl[r / scale];
        for (int i = 0; i < 8 * scale; ++i)
            p[i] = bits & (0x80 >> (i / scale)) ? fg : bg;
    }
}

/// Draws text.
/// @param x left
/// @param y top
/// @param s the text
/// @param fg its colour
/// @param bg the background
/// @return the x after it
static int text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; ++s, x += 8)
        chs(x, y, (uint8_t)*s, fg, bg, 1);
    return x;
}

/// Draws formatted text.
/// @param x left
/// @param y top
/// @param fg its colour
/// @param bg the background
/// @param fmt printf's format
/// @return the x after it
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(x, y, buf, fg, bg);
}

/// A bevelled rectangle.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param raised raised (else sunken)
/// @param face its colour
static void bevel(int x, int y, int w, int h, bool raised, uint8_t face) {
    fill(x, y, w, h, face);
    uint8_t a = raised ? C_LIGHT : C_DARK, b = raised ? C_DARK : C_LIGHT;
    fill(x, y, w, 1, a);
    fill(x, y, 1, h, a);
    fill(x, y + h - 1, w, 1, b);
    fill(x + w - 1, y, 1, h, b);
}

/// Remembers a place to click (only while the whole picture is drawn).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param id HIT_*
/// @param arg which
static void hit(int x, int y, int w, int h, int id, int arg) {
    if (S->collect && S->nhits < (int)(sizeof S->hits / sizeof *S->hits))
        S->hits[S->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = S->nhits - 1; i >= 0; --i) {
        const hit_t *h = &S->hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
            return h;
    }
    return NULL;
}

/// A button: pressed-in and yellow when lit.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param label its text
/// @param lit pressed
/// @param id what a click on it does (0: nothing)
/// @param arg which
static void button(int x, int y, int w, int h, const char *label, bool lit, int id, int arg) {
    uint8_t face = lit ? C_SEL : C_FACE;
    bevel(x, y, w, h, !lit, face);
    int n = strlen(label);
    text(x + (w - n * 8) / 2, y + (h - 16) / 2, label, lit ? C_YELLOW : C_TEXT, face);
    if (id)
        hit(x, y, w, h, id, arg);
}

/// A value between a pair of arrow buttons, each clickable.
/// @param x left
/// @param y top
/// @param w the value's width
/// @param s the value
/// @param id HIT_*: arg -1 for the left arrow, +1 for the right
static void spinner(int x, int y, int w, const char *s, int id) {
    button(x, y, 20, 22, "<", false, id, -1);
    bevel(x + 22, y, w, 22, false, C_FIELD);
    text(x + 22 + (w - (int)strlen(s) * 8) / 2, y + 3, s, C_CYAN, C_FIELD);
    hit(x + 22, y, w, 22, id, 0);
    button(x + 24 + w, y, 20, 22, ">", false, id, 1);
}

/// A slider: a sunken track with a knob.
/// @param x left
/// @param y top
/// @param frac where the knob is, 0..1
/// @param id HIT_*
static void slider(int x, int y, float frac, int id) {
    bevel(x, y + 7, SL_W, 8, false, C_FIELD);
    int kx = x + (int)(frac * (SL_W - 10) + 0.5f);
    fill(x + 1, y + 8, kx - x, 6, C_SEL);
    bevel(kx, y + 1, 10, 20, true, C_FACE);
    hit(x - 4, y, SL_W + 8, 22, id, 0);
}

/// A step's size as text.
/// @param s where it goes
/// @param n its room
/// @param hz the step
static void fmt_step(char *s, int n, uint32_t hz) {
    if (hz >= 1000000) {
        snprintf(s, n, "%lu MHz", (unsigned long)(hz / 1000000));
    } else if (hz >= 1000) {
        // 12500 -> "12.5 kHz", 6250 -> "6.25 kHz"
        char frac[4];
        snprintf(frac, sizeof frac, "%03lu", (unsigned long)(hz % 1000));
        for (int i = 2; i >= 0 && frac[i] == '0'; --i)
            frac[i] = 0;
        snprintf(s, n, "%lu%s%s kHz", (unsigned long)(hz / 1000), frac[0] ? "." : "", frac);
    } else {
        snprintf(s, n, "%lu Hz", (unsigned long)hz);
    }
}

/// The gain setting as text.
/// @param s where it goes
/// @param n its room
static void fmt_gain(char *s, int n) {
    int count;
    const int16_t *g = rtl_gains(&count);
    if (S->gain < 0)
        snprintf(s, n, "Auto");
    else
        snprintf(s, n, "%d.%d dB", g[S->gain] / 10, g[S->gain] % 10);
}

/// The title strip: the program, the dongle, and a message or the load.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    int x = text(8, 4, "SDR receiver", C_YELLOW, C_STRIP);
    if (S->running) {
        const char *name = S->dev.product[0] ? S->dev.product : "RTL2832U";
        x = textf(x + 16, 4, C_LABEL, C_STRIP, "%s, %s, %lu kS/s%s", name, rtl_tuner_name(S->dev.tuner),
                  (unsigned long)(S->dev.rate / 1000), S->dev.direct ? ", direct" : "");
    }
    bool msg_on = S->msg[0] && now_ms() < S->msg_until;
    if (msg_on) {
        int n = strlen(S->msg);
        text(W - 8 - n * 8, 4, S->msg, C_TEXT, C_STRIP);
    } else if (S->running) {
        textf(W - 8 - 24 * 8, 4, C_DIM, C_STRIP, "CPU %3d%%  lost %6lu", (S->load + 5) / 10,
              (unsigned long)rtl_usb_overruns());
    }
}

/// The frequency's characters: dddd.ddd.ddd, leading zeros as spaces (or
/// the frequency being typed).
/// @param s where they go (DIG_N + 1)
static void freq_chars(char *s) {
    char d[11];
    snprintf(d, sizeof d, "%010lu", (unsigned long)S->freq);
    int k = 0;
    bool lead = true;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7)
            s[k++] = '.';
        if (lead && d[i] == '0' && i < 3)
            s[k++] = ' ';
        else
            s[k++] = d[i], lead = false;
    }
    s[k] = 0;
}

/// The power of ten a big character stands for (-1: a dot).
/// @param i its place, 0..DIG_N-1
/// @return the digit's place (9 for the 1 GHz digit... 0 for 1 Hz)
static int digit_place(int i) {
    static const int8_t p[DIG_N] = {9, 8, 7, 6, -1, 5, 4, 3, -1, 2, 1, 0};
    return p[i];
}

/// The frequency panel: the big digits, the mode, step, meter and gain.
static void draw_display(void) {
    bevel(4, DISP_Y, W - 8, DISP_H, true, C_FACE);
    bevel(10, DISP_Y + 5, DIG_N * DIG_W + 52, DISP_H - 10, false, C_FIELD);
    if (S->typing) {
        char s[24];
        snprintf(s, sizeof s, "%s_", S->typed);
        int x = DIG_X;
        for (const char *p = s; *p; ++p, x += DIG_W)
            chs(x, DIG_Y, (uint8_t)*p, C_CYAN, C_FIELD, 3);
        text(DIG_X + DIG_N * DIG_W + 4, DIG_Y + 30, "MHz", C_CYAN, C_FIELD);
    } else {
        char s[DIG_N + 1];
        freq_chars(s);
        for (int i = 0; i < DIG_N; ++i) {
            int x = DIG_X + i * DIG_W;
            chs(x, DIG_Y, (uint8_t)(s[i] == ' ' ? '0' : s[i]), s[i] == ' ' ? C_UNLIT : C_YELLOW, C_FIELD, 3);
            if (digit_place(i) >= 0)
                hit(x, DIG_Y, DIG_W, DIG_H, HIT_DIGIT, digit_place(i));
        }
        text(DIG_X + DIG_N * DIG_W + 4, DIG_Y + 30, "MHz", C_LABEL, C_FIELD);
    }
    char st[16], gs[16];
    fmt_step(st, sizeof st, steps[S->step]);
    fmt_gain(gs, sizeof gs);
    int x = textf(INFO_X, DISP_Y + 6, C_YELLOW, C_FACE, "%s", mode_names[S->mode]);
    textf(x + 16, DISP_Y + 6, C_LABEL, C_FACE, "step %s  gain %s", st, gs);
}

/// The signal meter and squelch light (drawn often, straight to the screen).
static void draw_meter(void) {
    int y = DISP_Y + 26, w = W - 12 - INFO_X;
    bevel(INFO_X, y, w, 12, false, C_FIELD);
    float db = S->running ? dsp_level_db(S->dsp) : -100.0f;
    int lw = (int)((db + 100.0f) / 100.0f * (w - 2));
    if (lw < 0) lw = 0;
    if (lw > w - 2) lw = w - 2;
    bool open = S->running && S->dsp->sq_open;
    fill(INFO_X + 1, y + 1, lw, 10, open ? C_GREEN : C_DIM);
    if (S->squelch > -100) {
        int sx = INFO_X + 1 + (int)((S->squelch + 100) / 100.0f * (w - 2));
        fill(sx, y + 1, 2, 10, C_ORANGE);
    }
    fill(INFO_X, y + 14, w, 16, C_FACE);
    textf(INFO_X, y + 15, C_TEXT, C_FACE, "%6.1f dBFS", (double)db);
    const char *sq = S->squelch <= -100 ? "squelch off" : open ? "open" : "shut";
    text(INFO_X + 120, y + 15, sq, S->squelch <= -100 ? C_DIM : open ? C_GREEN : C_ORANGE, C_FACE);
    if (S->scanning)
        text(INFO_X + 210, y + 15, "SCAN", C_YELLOW, C_FACE);
    else if (S->muted)
        text(INFO_X + 210, y + 15, "MUTE", C_ORANGE, C_FACE);
}

/// The spectrum's pixel row for a level.
/// @param db the level
/// @return its y (SP_Y at the top)
static int spec_y(float db) {
    float bottom = S->floor_db - 8.0f;
    int v = (int)((db - bottom) / SPAN_DB * SP_H);
    if (v < 0) v = 0;
    if (v > SP_H - 1) v = SP_H - 1;
    return SP_Y + SP_H - 1 - v;
}

/// The x of a frequency on the spectrum (may be off it).
/// @param hz the frequency
/// @return its x
static int freq_x(uint32_t hz) {
    int64_t d = (int64_t)hz - (int64_t)S->centre;
    return SP_X + SP_W / 2 + (int)(d * SP_W / (int64_t)S->rate);
}

/// The channel's width on the spectrum, each side (Hz).
/// @return half the band
static uint32_t chan_half(void) {
    return S->mode == DSP_WFM ? 100000 : S->mode == DSP_NFM ? 7500 : 5000;
}

/// The spectrum: the grid, the channel's band, the levels and the scale.
static void draw_spectrum(void) {
    fill(SP_X, SP_Y, SP_W, SP_H, C_FIELD);
    int a = freq_x(S->freq - chan_half()), b = freq_x(S->freq + chan_half());
    if (a < SP_X) a = SP_X;
    if (b > SP_X + SP_W) b = SP_X + SP_W;
    if (b > a)
        fill(a, SP_Y, b - a, SP_H, C_CHAN);
    for (int k = 1; k < 6; ++k)
        fill(SP_X, SP_Y + k * SP_H / 6, SP_W, 1, C_GRID);  // every 10 dB
    // The scale: a line every 25 kHz, labels every 50 (40 kHz at 288 kS/s... same).
    uint32_t lo = S->centre - S->rate / 2, hi = S->centre + S->rate / 2;
    fill(SP_X, SCALE_Y, SP_W, 16, C_FIELD);
    for (uint32_t f = (lo + 24999) / 25000 * 25000; f < hi; f += 25000) {
        int x = freq_x(f);
        fill(x, SP_Y, 1, SP_H, C_GRID);
        if (f % 50000 == 0) {
            char s[16];
            snprintf(s, sizeof s, "%lu.%03lu", (unsigned long)(f / 1000000), (unsigned long)(f / 1000 % 1000));
            int n = strlen(s);
            if (x - n * 4 >= SP_X && x + n * 4 <= SP_X + SP_W)
                text(x - n * 4, SCALE_Y, s, C_DIM, C_FIELD);
        }
    }
    if (S->have_spec) {
        for (int c = 0; c < SP_W; ++c) {
            int y = spec_y(S->col[c]);
            uint8_t *p = S->t + y * W + SP_X + c;
            *p = C_CYAN;
            for (p += W; p < S->t + (SP_Y + SP_H) * W; p += W)
                *p = *p == C_FIELD ? C_SPEC : (uint8_t)(*p | C_SPEC);
        }
    }
    int cx = freq_x(S->freq);
    if (cx >= SP_X && cx < SP_X + SP_W)
        fill(cx, SP_Y, 1, SP_H, C_RED);
    hit(SP_X, SP_Y, SP_W, SCALE_Y + 16 - SP_Y, HIT_SPEC, 0);
}

/// The waterfall, from its rows.
static void draw_waterfall(void) {
    for (int r = 0; r < WF_H; ++r)
        memcpy(S->t + (WF_Y + r) * W + SP_X, S->wf + ((S->wf_top + r) % WF_H) * SP_W, SP_W);
    hit(SP_X, WF_Y, SP_W, WF_H, HIT_SPEC, 0);
}

/// The controls' panel.
static void draw_controls(void) {
    bevel(4, CTL_Y, W - 8, FILE_Y - 4 - CTL_Y, true, C_FACE);
    for (int m = 0; m < DSP_MODES; ++m)
        button(10 + m * 54, ROW_A, 50, 22, mode_names[m], m == S->mode, HIT_MODE, m);
    char s[24];
    text(180, ROW_A + 3, "Step", C_LABEL, C_FACE);
    fmt_step(s, sizeof s, steps[S->step]);
    spinner(216, ROW_A, 72, s, HIT_STEP);
    text(348, ROW_A + 3, "Gain", C_LABEL, C_FACE);
    fmt_gain(s, sizeof s);
    spinner(384, ROW_A, 72, s, HIT_GAIN);
    button(554, ROW_A, 76, 22, "Bias-T", S->bias, HIT_BIAS, 0);

    text(10, ROW_B + 3, "Volume", C_LABEL, C_FACE);
    slider(VOL_X, ROW_B, S->volume / 100.0f, HIT_VOL);
    button(256, ROW_B, 52, 22, "Mute", S->muted, HIT_MUTE, 0);
    text(318, ROW_B + 3, "Squelch", C_LABEL, C_FACE);
    slider(SQ_X, ROW_B, (S->squelch + 100) / 100.0f, HIT_SQ);
    if (S->squelch <= -100)
        snprintf(s, sizeof s, "off");
    else
        snprintf(s, sizeof s, "%d dB", S->squelch);
    text(SQ_X + SL_W + 8, ROW_B + 3, s, C_CYAN, C_FACE);

    static const char *const tl[4] = {"<<", "<", ">", ">>"};
    for (int i = 0; i < 4; ++i)
        button(10 + i * 40, ROW_C, 36, 22, tl[i], false, HIT_TUNE, i);
    snprintf(s, sizeof s, "%lu kS/s", (unsigned long)(S->rate / 1000));
    button(176, ROW_C, 88, 22, s, false, HIT_RATE, 0);
    text(276, ROW_C + 3, "PPM", C_LABEL, C_FACE);
    snprintf(s, sizeof s, "%+d", S->ppm);
    spinner(304, ROW_C, 48, s, HIT_PPM);
    snprintf(s, sizeof s, "De-emph %d us", S->deemph);
    button(420, ROW_C, 128, 22, s, false, HIT_DEEMPH, 0);
}

/// The buttons along the bottom.
static void draw_buttons(void) {
    fill(0, FILE_Y - 2, W, H - FILE_Y + 2, C_BG);
    for (int i = 0; i < BB_COUNT; ++i)
        button(4 + i * 63, FILE_Y, 60, 26, bb_names[i], i == BB_SCAN && S->scanning, HIT_BTN, i);
    textf(4 + BB_COUNT * 63 + 4, FILE_Y + 5, C_DIM, C_BG, "%d fav%s", S->nfavs, S->nfavs == 1 ? "" : "s");
}

/// A dialog's frame and title.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param title its title
static void box(int x, int y, int w, int h, const char *title) {
    hit(0, 0, W, H, HIT_SWALLOW, 0);  // clicks outside it do nothing
    bevel(x, y, w, h, true, C_FACE);
    fill(x + 2, y + 2, w - 4, 20, C_STRIP);
    text(x + 8, y + 4, title, C_YELLOW, C_STRIP);
}

#define FB_X 80   ///< the favourites box
#define FB_Y 60
#define FB_W 480
#define FB_ROWS 14
#define FB_H (34 + FB_ROWS * 18 + 40)

/// The favourites box: the list, and buttons for the one selected.
static void draw_favs(void) {
    box(FB_X, FB_Y, FB_W, FB_H, "Favourites");
    int ly = FB_Y + 28;
    bevel(FB_X + 8, ly, FB_W - 16, FB_ROWS * 18 + 4, false, C_FIELD);
    if (!S->nfavs)
        text(FB_X + 20, ly + 6, "None yet: Add fav keeps the frequency you are on.", C_DIM, C_FIELD);
    for (int r = 0; r < FB_ROWS && S->fav_top + r < S->nfavs; ++r) {
        int i = S->fav_top + r, y = ly + 2 + r * 18;
        uint8_t bg = i == S->fav_sel ? C_SEL : (r & 1) ? C_ROWALT : C_FIELD;
        fill(FB_X + 10, y, FB_W - 20, 18, bg);
        const fav_t *f = &S->favs[i];
        textf(FB_X + 14, y + 1, i == S->fav_sel ? C_YELLOW : C_TEXT, bg, "%4lu.%03lu.%03lu", (unsigned long)(f->hz / 1000000),
              (unsigned long)(f->hz / 1000 % 1000), (unsigned long)(f->hz % 1000));
        text(FB_X + 14 + 13 * 8, y + 1, mode_names[f->mode], C_LABEL, bg);
        text(FB_X + 14 + 18 * 8, y + 1, f->name, C_CYAN, bg);
        hit(FB_X + 10, y, FB_W - 20, 18, HIT_FAVROW, i);
    }
    int by = FB_Y + FB_H - 34;
    for (int i = 0; i < FV_COUNT; ++i)
        button(FB_X + 8 + i * 77, by, 72, 26, fv_names[i], false, HIT_FAVBTN, i);
}

/// The box for a favourite's name.
static void draw_name(void) {
    box(120, 180, 400, 90, S->naming_fav < 0 ? "Name the favourite" : "Rename the favourite");
    bevel(136, 212, 368, 22, false, C_FIELD);
    textf(140, 215, C_CYAN, C_FIELD, "%s_", S->naming);
    text(136, 244, "Enter keeps it, Esc doesn't", C_DIM, C_FACE);
}

/// The keys.
static void draw_help(void) {
    static const char *const lines[] = {
        "Left/Right       tune down/up a step",
        "PgDn/PgUp        tune down/up 1 MHz",
        "Up/Down          volume",
        "0-9 . Enter      type a frequency in MHz",
        "Wheel on digit   change that digit (click: up, right-click: down)",
        "Click spectrum   tune there (the wheel steps)",
        "M                next mode (WFM, NFM, AM)",
        "S / Shift+S      step bigger / smaller",
        "G / Shift+G      gain up / down (below 0: auto)",
        "Q / Shift+Q      squelch up / down",
        "Space            mute",
        "F / L            add a favourite / the list",
        "N / P            next / previous favourite",
        "C                scan the favourites (squelch must be on)",
        "Esc              close a box, stop typing, or quit",
    };
    int n = (int)(sizeof lines / sizeof *lines);
    box(60, 60, 520, 40 + n * 18, "Keys (any key closes this)");
    for (int i = 0; i < n; ++i)
        text(76, 88 + i * 18, lines[i], C_TEXT, C_FACE);
}

/// Draws the whole picture off the screen.
static void draw(void) {
    S->t = S->g;
    S->collect = true;
    S->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_top();
    draw_display();
    draw_meter();
    pump();
    bevel(4, SP_Y - 2, W - 8, SCALE_Y + 18 - SP_Y, false, C_FIELD);
    draw_spectrum();
    bevel(4, WF_Y - 2, W - 8, WF_H + 4, false, C_FIELD);
    draw_waterfall();
    pump();
    draw_controls();
    draw_buttons();
    if (!S->running) {
        bevel(120, 150, 400, 80, true, C_FACE);
        char e[sizeof S->err];
        snprintf(e, sizeof e, "%s", S->err[0] ? S->err : "starting the dongle...");
        e[0] = (char)toupper((unsigned char)e[0]);
        text(136, 166, e, C_ORANGE, C_FACE);
        text(136, 190, "Plug an RTL-SDR dongle into the USB hub;", C_TEXT, C_FACE);
        text(136, 206, "it starts by itself. Esc leaves.", C_TEXT, C_FACE);
    }
    switch (S->dlg) {
    case DLG_FAVS: draw_favs(); break;
    case DLG_NAME: draw_name(); break;
    case DLG_HELP: draw_help(); break;
    }
    S->collect = false;
    S->t = dvi_framebuf;
}

/// Copies the picture onto the screen.
static void show(void) {
    memcpy(dvi_framebuf, S->g, W * H);
    pump();
}

static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S->msg, sizeof S->msg, fmt, ap);
    va_end(ap);
    S->msg_until = now_ms() + 4000;
    S->dirty = true;
}

// ---- Sound -------------------------------------------------------------------------------------------

/// The audio source (core 1): the next 48 kHz frame from the ring.
/// @param left the left sample
/// @param right the right
/// @return true (the receiver is playing, if quietly)
static bool sdr_src(int32_t *left, int32_t *right) {
    uint32_t have = a_in - a_out;
    *left = *right = 0;
    if (!a_playing) {
        if (have < AUD_START)
            return true;
        a_playing = true;
    }
    if (!have) {
        a_playing = false;
        a_underruns = a_underruns + 1;
        return true;
    }
    int32_t v = aring[a_out % AUD_N];
    a_out = a_out + 1;
    *left = *right = v;
    return true;
}

/// Puts audio in the ring; drops a sample now and then if it is filling
/// (the dongle's clock is a little faster than the HDMI sound's), and all of
/// it if it is full.
/// @param a the samples
/// @param n how many
static void audio_put(const int16_t *a, int n) {
    static uint32_t count;
    for (int i = 0; i < n; ++i) {
        uint32_t have = a_in - a_out;
        if (have >= AUD_N)
            return;
        if (have > AUD_HIGH && (++count & 255) == 0)
            continue;
        aring[a_in % AUD_N] = a[i];
        a_in = a_in + 1;
    }
}

// ---- The dongle -------------------------------------------------------------------------------------

/// Demodulates what has come, feeds the spectrum and the sound.
static void pump(void) {
    if (!S || !S->running)
        return;
    uint32_t len;
    const uint8_t *b;
    while ((b = rtl_usb_read(&len)) != NULL) {
        uint32_t t0 = time_us_32();
        int pairs = (int)(len / 2);
        dsp_spec_feed(S->spec, b, pairs);
        int n = dsp_process(S->dsp, b, pairs, S->abuf, (int)(sizeof S->abuf / sizeof *S->abuf));
        rtl_usb_done();
        audio_put(S->abuf, n);
        S->busy_us += time_us_32() - t0;
    }
    uint32_t now = time_us_32();
    if (now - S->busy_since >= 1000000) {
        S->load = (int)((uint64_t)S->busy_us * 1000 / (now - S->busy_since));
        S->busy_us = 0;
        S->busy_since = now;
    }
}

/// The demodulator's volume and squelch from the settings.
static void dsp_settings(void) {
    S->dsp->volume = S->muted ? 0.0f : (S->volume / 100.0f) * (S->volume / 100.0f) * 2.0f;
    S->dsp->squelch_db = S->squelch <= -100 ? -200.0f : (float)S->squelch;
    dsp_set_deemph(S->dsp, S->deemph);
}

/// Tunes the dongle to `centre` (holding the stream while it does).
/// @return false if the tuner wouldn't
static bool retune(void) {
    rtl_usb_hold(true);
    bool ok = rtl_set_freq(&S->dev, S->centre);
    rtl_usb_hold(false);
    return ok;
}

/// Tunes to a frequency: the channel moves within the band the dongle
/// sends, and the dongle is tuned again when it would leave it (always, for
/// wide FM, whose channel is the whole band).
/// @param hz the frequency
static void tune(uint32_t hz) {
    if (hz < FREQ_MIN)
        hz = FREQ_MIN;
    if (hz > FREQ_MAX)
        hz = FREQ_MAX;
    S->freq = hz;
    S->dirty = true;
    if (!S->running) {
        S->centre = hz;
        return;
    }
    int64_t off = (int64_t)hz - (int64_t)S->centre;
    int64_t room = (int64_t)S->rate * 35 / 100 - (int64_t)chan_half();
    if (S->mode == DSP_WFM || off > room || off < -room) {
        S->centre = hz;
        off = 0;
        if (!retune())
            message("The tuner won't tune to %lu.%03lu MHz", (unsigned long)(hz / 1000000),
                    (unsigned long)(hz / 1000 % 1000));
    }
    dsp_set_offset(S->dsp, (int32_t)off);
}

/// Stops the dongle and the sound (talking to it only if it is still there).
static void stop_dongle(void) {
    if (!S->running)
        return;
    S->running = false;
    if (audio_get_source() == sdr_src) {
        audio_set_source(NULL);
        sleep_ms(5);  // core 1 may be in the middle of a frame
    }
    if (rtl_usb_present()) {
        rtl_usb_stream_stop();
        rtl_stream(&S->dev, false);
        rtl_close(&S->dev);
    }
}

/// Sets the dongle up and starts it sending; S->err says why not.
/// @return false if it couldn't
static bool start_dongle(void) {
    S->err[0] = 0;
    if (!rtl_open(&S->dev, S->err, sizeof S->err))
        return false;
    rtl_set_ppm(&S->dev, S->ppm);  // before the rate: only noted
    int count;
    const int16_t *g = rtl_gains(&count);
    S->centre = S->freq;
    if (!rtl_set_rate(&S->dev, S->rate) || !rtl_set_gain(&S->dev, S->gain < 0 ? RTL_GAIN_AUTO : g[S->gain])) {
        snprintf(S->err, sizeof S->err, "the dongle didn't take the rate");
        rtl_close(&S->dev);
        return false;
    }
    if (S->bias)
        rtl_set_bias_tee(&S->dev, true);
    rtl_set_freq(&S->dev, S->centre);
    dsp_init(S->dsp, S->rate, S->mode);
    dsp_settings();
    dsp_spec_again(S->spec);
    if (!rtl_stream(&S->dev, true) || !rtl_usb_stream_start(S->ring)) {
        snprintf(S->err, sizeof S->err, "the dongle won't send");
        rtl_close(&S->dev);
        return false;
    }
    a_in = a_out = 0;
    a_playing = false;
    audio_set_source(sdr_src);
    S->running = true;
    S->busy_since = time_us_32();
    S->busy_us = 0;
    tune(S->freq);
    return true;
}

/// Restarts the stream at a new rate.
static void change_rate(void) {
    S->rate = S->rate == 240000 ? 288000 : 240000;
    if (!S->running)
        return;
    rtl_usb_stream_stop();
    rtl_stream(&S->dev, false);
    rtl_set_rate(&S->dev, S->rate);
    dsp_init(S->dsp, S->rate, S->mode);
    dsp_settings();
    dsp_spec_again(S->spec);
    rtl_stream(&S->dev, true);
    rtl_usb_stream_start(S->ring);
    tune(S->freq);
}

// ---- The spectrum and waterfall ---------------------------------------------------------------------

/// Makes the waterfall's colours.
static void make_palette(void) {
    static const uint8_t pts[][3] = {{0, 0, 0}, {0, 0, 2}, {0, 2, 3}, {0, 6, 3}, {7, 7, 0}, {7, 1, 0}, {7, 7, 3}};
    const int np = (int)(sizeof pts / sizeof *pts) - 1;
    for (int i = 0; i < 64; ++i) {
        float t = (float)i / 63.0f * np;
        int k = (int)t;
        if (k >= np)
            k = np - 1;
        float f = t - k;
        int r = (int)(pts[k][0] + (pts[k + 1][0] - pts[k][0]) * f + 0.5f);
        int g = (int)(pts[k][1] + (pts[k + 1][1] - pts[k][1]) * f + 0.5f);
        int b = (int)(pts[k][2] + (pts[k + 1][2] - pts[k][2]) * f + 0.5f);
        S->wf_pal[i] = RGB332(r, g, b);
    }
}

/// Takes a new spectrum: the columns, the noise floor, a waterfall row.
/// @return false if there wasn't a new one
static bool spectrum_step(void) {
    dsp_spec_t *sp = S->spec;
    if (!sp->fresh)
        return false;
    sp->fresh = false;
    float sum = 0.0f;
    for (int c = 0; c < SP_W; ++c) {
        int b0 = c * DSP_FFT_N / SP_W, b1 = (c + 1) * DSP_FFT_N / SP_W;
        float m = sp->db[b0];
        for (int b = b0 + 1; b < b1; ++b)
            if (sp->db[b] > m)
                m = sp->db[b];
        if (m < -127.0f) m = -127.0f;
        if (m > 0.0f) m = 0.0f;
        S->col[c] = (int8_t)m;
        sum += m;
    }
    // The floor: the average of the columns below the average.
    float mean = sum / SP_W, low = 0.0f;
    int nlow = 0;
    for (int c = 0; c < SP_W; ++c)
        if (S->col[c] <= mean)
            low += S->col[c], ++nlow;
    float fl = nlow ? low / nlow : mean;
    S->floor_db = S->have_spec ? S->floor_db + (fl - S->floor_db) * 0.05f : fl;
    S->have_spec = true;
    S->wf_top = (S->wf_top + WF_H - 1) % WF_H;
    uint8_t *row = S->wf + S->wf_top * SP_W;
    float bottom = S->floor_db - 4.0f;
    for (int c = 0; c < SP_W; ++c) {
        int v = (int)((S->col[c] - bottom) * 64.0f / (SPAN_DB - 10));
        row[c] = S->wf_pal[v < 0 ? 0 : v > 63 ? 63 : v];
    }
    dsp_spec_again(sp);
    return true;
}

/// Shows a new spectrum line on the screen: the spectrum, and the waterfall
/// moved down a row (in the framebuffer itself, quicker than from PSRAM).
static void spectrum_show(void) {
    S->t = dvi_framebuf;
    draw_spectrum();
    memmove(dvi_framebuf + (WF_Y + 1) * W, dvi_framebuf + WF_Y * W, (WF_H - 1) * W);
    memcpy(dvi_framebuf + WF_Y * W + SP_X, S->wf + S->wf_top * SP_W, SP_W);
}

// ---- Settings -----------------------------------------------------------------------------------------

/// The settings at the start.
static void defaults(void) {
    S->freq = 100000000;
    S->mode = DSP_WFM;
    S->step = 9;  // 100 kHz
    S->gain = -1;
    S->squelch = -100;
    S->volume = 60;
    S->rate = 240000;
    S->deemph = 75;
    S->naming_fav = -1;
}

/// Reads /SDR.INI.
static void load_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_READ) != FR_OK)
        return;
    char line[96];
    int count;
    rtl_gains(&count);
    while (f_gets(line, sizeof line, &f)) {
        char *e = strchr(line, '=');
        if (!e || line[0] == '#')
            continue;
        *e++ = 0;
        e[strcspn(e, "\r\n")] = 0;
        long v = strtol(e, NULL, 10);
        if (!strcmp(line, "freq") && v >= (long)FREQ_MIN && v <= (long)FREQ_MAX) S->freq = (uint32_t)v;
        else if (!strcmp(line, "mode") && v >= 0 && v < DSP_MODES) S->mode = (int)v;
        else if (!strcmp(line, "step") && v >= 0 && v < NSTEPS) S->step = (int)v;
        else if (!strcmp(line, "gain") && v >= -1 && v < count) S->gain = (int)v;
        else if (!strcmp(line, "squelch") && v >= -100 && v <= 0) S->squelch = (int)v;
        else if (!strcmp(line, "volume") && v >= 0 && v <= 100) S->volume = (int)v;
        else if (!strcmp(line, "rate") && (v == 240000 || v == 288000)) S->rate = (uint32_t)v;
        else if (!strcmp(line, "ppm") && v >= -200 && v <= 200) S->ppm = (int)v;
        else if (!strcmp(line, "deemph") && (v == 50 || v == 75)) S->deemph = (int)v;
        else if (!strcmp(line, "bias")) S->bias = v != 0;
        else if (!strcmp(line, "fav") && S->nfavs < MAX_FAVS) {
            // fav=Hz|mode|name
            char *p = strchr(e, '|');
            fav_t *fv = &S->favs[S->nfavs];
            fv->hz = (uint32_t)strtoul(e, NULL, 10);
            fv->mode = p ? (uint8_t)atoi(p + 1) : 0;
            char *q = p ? strchr(p + 1, '|') : NULL;
            snprintf(fv->name, sizeof fv->name, "%s", q ? q + 1 : "");
            if (fv->hz >= FREQ_MIN && fv->hz <= FREQ_MAX && fv->mode < DSP_MODES)
                ++S->nfavs;
        }
    }
    f_close(&f);
}

/// Writes /SDR.INI.
static void save_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    f_printf(&f, "# RTL-SDR receiver (the sdr command)\n");
    f_printf(&f, "# freq in Hz; mode 0 WFM, 1 NFM, 2 AM; gain -1 auto or a step; fav=Hz|mode|name\n");
    f_printf(&f, "freq=%lu\nmode=%d\nstep=%d\ngain=%d\nsquelch=%d\nvolume=%d\nrate=%lu\nppm=%d\ndeemph=%d\nbias=%d\n",
             (unsigned long)S->freq, S->mode, S->step, S->gain, S->squelch, S->volume, (unsigned long)S->rate, S->ppm,
             S->deemph, S->bias ? 1 : 0);
    for (int i = 0; i < S->nfavs; ++i)
        f_printf(&f, "fav=%lu|%d|%s\n", (unsigned long)S->favs[i].hz, S->favs[i].mode, S->favs[i].name);
    f_close(&f);
}

// ---- Actions -----------------------------------------------------------------------------------------

/// Changes the mode.
/// @param m DSP_*
static void set_mode(int m) {
    S->mode = m;
    if (S->running) {
        dsp_set_mode(S->dsp, m);
        dsp_settings();
        tune(S->freq);
    }
    S->dirty = true;
}

/// Steps the gain (below the lowest: auto).
/// @param d -1 or +1
static void gain_step(int d) {
    int count;
    const int16_t *g = rtl_gains(&count);
    int v = S->gain + d;
    if (v < -1) v = -1;
    if (v >= count) v = count - 1;
    S->gain = v;
    if (S->running) {
        rtl_usb_hold(true);
        rtl_set_gain(&S->dev, v < 0 ? RTL_GAIN_AUTO : g[v]);
        rtl_usb_hold(false);
    }
    S->dirty = true;
}

/// Changes the crystal correction.
/// @param d the change
static void ppm_step(int d) {
    int v = S->ppm + d;
    if (v < -200 || v > 200)
        return;
    S->ppm = v;
    if (S->running) {
        rtl_usb_hold(true);
        rtl_set_ppm(&S->dev, v);
        rtl_usb_hold(false);
        tune(S->freq);
    }
    S->dirty = true;
}

/// The squelch level (-100: off).
/// @param v dBFS
static void set_squelch(int v) {
    S->squelch = v < -100 ? -100 : v > 0 ? 0 : v;
    if (S->running)
        dsp_settings();
    S->dirty = true;
}

/// The volume.
/// @param v 0-100
static void set_volume(int v) {
    S->volume = v < 0 ? 0 : v > 100 ? 100 : v;
    if (S->running)
        dsp_settings();
    S->dirty = true;
}

/// Tunes to a favourite.
/// @param i which
static void go_fav(int i) {
    if (i < 0 || i >= S->nfavs)
        return;
    if (S->favs[i].mode != S->mode)
        set_mode(S->favs[i].mode);
    tune(S->favs[i].hz);
    if (S->favs[i].name[0])
        message("%s", S->favs[i].name);
}

/// The favourite at this frequency, or -1.
/// @return its index
static int fav_here(void) {
    for (int i = 0; i < S->nfavs; ++i)
        if (S->favs[i].hz == S->freq)
            return i;
    return -1;
}

/// Starts or stops the scan of the favourites.
static void scan_toggle(void) {
    if (S->scanning) {
        S->scanning = false;
    } else if (S->nfavs < 2) {
        message("Scan needs two favourites or more");
    } else if (S->squelch <= -100) {
        message("Set the squelch first: scan stops where it opens");
    } else {
        S->scanning = true;
        S->scan_at = fav_here();
        S->scan_next = 0;
    }
    S->dirty = true;
}

/// Scan: on to the next favourite unless something is heard here.
static void scan_step(void) {
    if (!S->scanning || !S->running)
        return;
    uint32_t now = now_ms();
    if (S->dsp->sq_open) {
        S->scan_next = now + SCAN_HANG_MS;  // stay while it is heard, and a little after
        return;
    }
    if ((int32_t)(now - S->scan_next) < 0)
        return;
    S->scan_at = (S->scan_at + 1) % S->nfavs;
    go_fav(S->scan_at);
    S->dsp->sq_open = false;
    S->scan_next = now + SCAN_DWELL_MS;
}

/// Adds a favourite here, and asks for its name.
static void add_fav(void) {
    if (fav_here() >= 0) {
        message("That frequency is a favourite already");
        return;
    }
    if (S->nfavs >= MAX_FAVS) {
        message("The favourites are full (%d)", MAX_FAVS);
        return;
    }
    S->naming[0] = 0;
    S->naming_fav = -1;
    S->dlg = DLG_NAME;
    S->dirty = true;
}

/// The name typed: a new favourite, or a renamed one.
static void name_done(void) {
    if (S->naming_fav < 0) {
        fav_t *f = &S->favs[S->nfavs++];
        f->hz = S->freq;
        f->mode = (uint8_t)S->mode;
        snprintf(f->name, sizeof f->name, "%s", S->naming);
        // Kept in frequency order.
        for (int i = S->nfavs - 1; i > 0 && S->favs[i - 1].hz > S->favs[i].hz; --i) {
            fav_t t = S->favs[i];
            S->favs[i] = S->favs[i - 1];
            S->favs[i - 1] = t;
        }
        message("Added to the favourites");
        S->dlg = DLG_NONE;
    } else {
        snprintf(S->favs[S->naming_fav].name, NAME_LEN, "%s", S->naming);
        S->dlg = DLG_FAVS;
    }
    save_ini();
    S->dirty = true;
}

/// The favourites box's buttons.
/// @param b FV_*
static void fav_button(int b) {
    int i = S->fav_sel;
    bool have = i >= 0 && i < S->nfavs;
    switch (b) {
    case FV_TUNE:
        if (have)
            go_fav(i);
        S->dlg = DLG_NONE;
        break;
    case FV_RENAME:
        if (have) {
            snprintf(S->naming, sizeof S->naming, "%s", S->favs[i].name);
            S->naming_fav = i;
            S->dlg = DLG_NAME;
        }
        break;
    case FV_DELETE:
        if (have) {
            memmove(&S->favs[i], &S->favs[i + 1], (S->nfavs - i - 1) * sizeof *S->favs);
            if (--S->nfavs <= S->fav_sel)
                S->fav_sel = S->nfavs - 1;
            save_ini();
        }
        break;
    case FV_UP:
    case FV_DOWN: {
        int j = b == FV_UP ? i - 1 : i + 1;
        if (have && j >= 0 && j < S->nfavs) {
            fav_t t = S->favs[i];
            S->favs[i] = S->favs[j];
            S->favs[j] = t;
            S->fav_sel = j;
            save_ini();
        }
        break;
    }
    case FV_CLOSE: S->dlg = DLG_NONE; break;
    }
    if (S->fav_sel < S->fav_top)
        S->fav_top = S->fav_sel < 0 ? 0 : S->fav_sel;
    if (S->fav_sel >= S->fav_top + FB_ROWS)
        S->fav_top = S->fav_sel - FB_ROWS + 1;
    S->dirty = true;
}

/// Opens the favourites box on the one tuned (if any).
static void open_favs(void) {
    S->fav_sel = fav_here();
    if (S->fav_sel < 0)
        S->fav_sel = S->nfavs ? 0 : -1;
    S->fav_top = S->fav_sel > FB_ROWS / 2 ? S->fav_sel - FB_ROWS / 2 : 0;
    S->dlg = DLG_FAVS;
    S->dirty = true;
}

/// The next favourite up or down from here.
/// @param d +1 or -1
static void fav_step(int d) {
    if (!S->nfavs)
        return;
    int i = fav_here();
    if (i < 0) {
        // The nearest in that direction.
        i = d > 0 ? 0 : S->nfavs - 1;
        for (int k = 0; k < S->nfavs; ++k)
            if (d > 0 ? S->favs[k].hz > S->freq : S->favs[S->nfavs - 1 - k].hz < S->freq) {
                i = d > 0 ? k : S->nfavs - 1 - k;
                break;
            }
    } else {
        i = (i + d + S->nfavs) % S->nfavs;
    }
    go_fav(i);
}

/// A frequency typed (MHz) is done.
static void typed_done(void) {
    S->typing = false;
    double mhz = atof(S->typed);
    if (mhz > 0)
        tune((uint32_t)(mhz * 1e6 + 0.5));
    S->dirty = true;
}

/// The bottom row's buttons.
/// @param b BB_*
static void bottom_button(int b) {
    switch (b) {
    case BB_FAVS: open_favs(); break;
    case BB_ADD: add_fav(); break;
    case BB_PREV: fav_step(-1); break;
    case BB_NEXT: fav_step(1); break;
    case BB_SCAN: scan_toggle(); break;
    case BB_FREQ:
        S->typing = true;
        S->typed[0] = 0;
        break;
    case BB_HELP: S->dlg = DLG_HELP; break;
    case BB_QUIT: S->quit = true; break;
    }
    S->dirty = true;
}

/// Tunes by a step from the frequency, snapping to the step's multiples.
/// @param n how many steps (+ up)
static void step_tune(int n) {
    int64_t st = steps[S->step], f = S->freq;
    int64_t to = n > 0 ? (f / st + n) * st : ((f + st - 1) / st + n) * st;
    tune(to < 0 ? 0 : (uint32_t)to);
}

// ---- Input ---------------------------------------------------------------------------------------------

/// A key.
/// @param k it
static void key(int k) {
    S->dirty = true;
    if (S->dlg == DLG_HELP) {
        S->dlg = DLG_NONE;
        return;
    }
    if (S->dlg == DLG_NAME) {
        int n = strlen(S->naming);
        if (k == '\r' || k == '\n')
            name_done();
        else if (k == KEY_ESC)
            S->dlg = S->naming_fav < 0 ? DLG_NONE : DLG_FAVS;
        else if ((k == 8 || k == 0x7f) && n)
            S->naming[n - 1] = 0;
        else if (k >= 32 && k < 127 && k != '|' && n < NAME_LEN - 1)
            S->naming[n] = (char)k, S->naming[n + 1] = 0;
        return;
    }
    if (S->dlg == DLG_FAVS) {
        switch (k) {
        case KEY_UP: if (S->fav_sel > 0) --S->fav_sel; fav_button(-1); break;
        case KEY_DOWN: if (S->fav_sel < S->nfavs - 1) ++S->fav_sel; fav_button(-1); break;
        case '\r': case '\n': fav_button(FV_TUNE); break;
        case KEY_DEL: fav_button(FV_DELETE); break;
        case KEY_ESC: S->dlg = DLG_NONE; break;
        }
        return;
    }
    if (S->typing) {
        int n = strlen(S->typed);
        if (k == '\r' || k == '\n')
            typed_done();
        else if (k == KEY_ESC)
            S->typing = false;
        else if ((k == 8 || k == 0x7f) && n)
            S->typed[n - 1] = 0;
        else if (((k >= '0' && k <= '9') || (k == '.' && !strchr(S->typed, '.'))) && n < 10)
            S->typed[n] = (char)k, S->typed[n + 1] = 0;
        return;
    }
    if ((k >= '0' && k <= '9') || k == '.') {
        S->typing = true;
        S->typed[0] = (char)k;
        S->typed[1] = 0;
        return;
    }
    switch (k) {
    case KEY_LEFT: step_tune(-1); break;
    case KEY_RIGHT: step_tune(1); break;
    case KEY_PGDN: tune(S->freq > 1000000 ? S->freq - 1000000 : FREQ_MIN); break;
    case KEY_PGUP: tune(S->freq + 1000000); break;
    case KEY_UP: set_volume(S->volume + 5); break;
    case KEY_DOWN: set_volume(S->volume - 5); break;
    case 'm': case 'M': set_mode((S->mode + 1) % DSP_MODES); break;
    case 's': if (S->step < NSTEPS - 1) ++S->step; break;
    case 'S': if (S->step > 0) --S->step; break;
    case 'g': gain_step(1); break;
    case 'G': gain_step(-1); break;
    case 'q': set_squelch(S->squelch + 5); break;
    case 'Q': set_squelch(S->squelch - 5); break;
    case ' ': S->muted = !S->muted; if (S->running) dsp_settings(); break;
    case 'f': case 'F': add_fav(); break;
    case 'l': case 'L': open_favs(); break;
    case 'n': case 'N': fav_step(1); break;
    case 'p': case 'P': fav_step(-1); break;
    case 'c': case 'C': scan_toggle(); break;
    case KEY_F1: case 'h': case 'H': case '?': S->dlg = DLG_HELP; break;
    case KEY_ESC: S->quit = true; break;
    }
}

/// A slider's value from the pointer.
/// @param id HIT_VOL or HIT_SQ
/// @param x the pointer's x
static void slide(int id, int x) {
    int x0 = id == HIT_VOL ? VOL_X : SQ_X;
    float f = (float)(x - x0 - 5) / (SL_W - 10);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    if (id == HIT_VOL)
        set_volume((int)(f * 100 + 0.5f));
    else
        set_squelch((int)(f * 100 + 0.5f) - 100);
}

/// The frequency at a point on the spectrum, snapped to the step.
/// @param x the point's x
/// @return the frequency
static uint32_t spec_freq(int x) {
    int64_t f = (int64_t)S->centre + (int64_t)(x - SP_X - SP_W / 2) * S->rate / SP_W;
    int64_t st = steps[S->step];
    f = (f + st / 2) / st * st;
    return f < 0 ? 0 : (uint32_t)f;
}

/// A mouse event.
/// @param e it
static void mouse(const mouse_event_t *e) {
    if (e->type == MOUSE_UP) {
        S->drag = 0;
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (S->drag && (e->buttons & MOUSE_LEFT))
            slide(S->drag, e->x);
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (!h)
        return;
    if (e->type == MOUSE_WHEEL) {
        int d = e->wheel > 0 ? 1 : -1;
        switch (h->id) {
        case HIT_DIGIT: {
            uint32_t p = 1;
            for (int i = 0; i < h->arg; ++i)
                p *= 10;
            int64_t f = (int64_t)S->freq + d * (int64_t)p;
            tune(f < FREQ_MIN ? FREQ_MIN : (uint32_t)f);
            break;
        }
        case HIT_SPEC: step_tune(d); break;
        case HIT_VOL: set_volume(S->volume + 5 * d); break;
        case HIT_SQ: set_squelch(S->squelch + 2 * d); break;
        case HIT_STEP: S->step = d > 0 ? (S->step < NSTEPS - 1 ? S->step + 1 : S->step) : (S->step ? S->step - 1 : 0); break;
        case HIT_GAIN: gain_step(d); break;
        case HIT_PPM: ppm_step(d); break;
        case HIT_FAVROW:
            S->fav_top += d > 0 ? -1 : 1;
            if (S->fav_top > S->nfavs - FB_ROWS) S->fav_top = S->nfavs - FB_ROWS;
            if (S->fav_top < 0) S->fav_top = 0;
            break;
        }
        S->dirty = true;
        return;
    }
    // MOUSE_DOWN.
    int d = h->arg ? h->arg : (e->button == MOUSE_RIGHT ? -1 : 1);
    switch (h->id) {
    case HIT_DIGIT: {
        uint32_t p = 1;
        for (int i = 0; i < h->arg; ++i)
            p *= 10;
        int64_t f = (int64_t)S->freq + (e->button == MOUSE_RIGHT ? -(int64_t)p : (int64_t)p);
        tune(f < FREQ_MIN ? FREQ_MIN : (uint32_t)f);
        break;
    }
    case HIT_SPEC: tune(spec_freq(e->x)); break;
    case HIT_MODE: set_mode(h->arg); break;
    case HIT_STEP:
        if (d > 0 && S->step < NSTEPS - 1) ++S->step;
        if (d < 0 && S->step > 0) --S->step;
        break;
    case HIT_GAIN: gain_step(d); break;
    case HIT_PPM: ppm_step(d); break;
    case HIT_BIAS:
        S->bias = !S->bias;
        if (S->running) {
            rtl_usb_hold(true);
            rtl_set_bias_tee(&S->dev, S->bias);
            rtl_usb_hold(false);
        }
        break;
    case HIT_VOL:
    case HIT_SQ:
        S->drag = h->id;
        slide(h->id, e->x);
        break;
    case HIT_MUTE:
        S->muted = !S->muted;
        if (S->running)
            dsp_settings();
        break;
    case HIT_TUNE:  // << < > >>: a megahertz, a step
        if (h->arg == 1 || h->arg == 2)
            step_tune(h->arg == 1 ? -1 : 1);
        else
            tune(h->arg == 0 ? (S->freq > FREQ_MIN + 1000000 ? S->freq - 1000000 : FREQ_MIN) : S->freq + 1000000);
        break;
    case HIT_RATE: change_rate(); break;
    case HIT_DEEMPH:
        S->deemph = S->deemph == 75 ? 50 : 75;
        if (S->running)
            dsp_settings();
        break;
    case HIT_BTN: bottom_button(h->arg); break;
    case HIT_FAVROW:
        if (S->fav_sel == h->arg && e->double_click)
            fav_button(FV_TUNE);
        else
            S->fav_sel = h->arg;
        break;
    case HIT_FAVBTN: fav_button(h->arg); break;
    }
    S->dirty = true;
}

// ---- The receiver ---------------------------------------------------------------------------------------

/// Frees what start() got.
static void stop(void) {
    stop_dongle();
    if (core1_job_now() == rtl_usb_job)
        core1_job_stop();
    if (S->spec_psram)
        psram_free(S->spec);
    else
        free(S->spec);
    psram_free(aring);
    aring = NULL;
    psram_free(S->wf);
    psram_free(S->g);
    psram_free(S);
    S = NULL;
}

/// Gets the memory and settings, and starts core 1's USB job.
/// @return false if there isn't the memory
static bool start(void) {
    S = psram_malloc(sizeof *S);
    if (!S)
        return false;
    memset(S, 0, sizeof *S);
    defaults();
    size_t ov;
    uint8_t *area = app_overlay_claim(APP_OV_SDR, &ov, NULL);  // stops the internet radio if it plays
    size_t need = RTL_SLOTS * RTL_SLOT_BYTES + ((sizeof(dsp_t) + 7) & ~(size_t)7);
    S->g = psram_malloc(W * H);
    S->wf = psram_malloc(WF_H * SP_W);
    aring = psram_malloc(AUD_N * sizeof *aring);
    S->spec = malloc(sizeof *S->spec);
    if (!S->spec) {
        S->spec = psram_malloc(sizeof *S->spec);
        S->spec_psram = true;
    }
    if (ov < need || !S->g || !S->wf || !aring || !S->spec) {
        stop();
        return false;
    }
    S->ring = area;
    S->dsp = (dsp_t *)(area + RTL_SLOTS * RTL_SLOT_BYTES);
    memset(S->wf, 0, WF_H * SP_W);
    dsp_spec_init(S->spec);
    make_palette();
    load_ini();
    S->centre = S->freq;
    S->floor_db = -60.0f;
    S->t = dvi_framebuf;
    core1_job_start(rtl_usb_job);
    return true;
}

/// The receiver's screen.
static void receiver(void) {
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    S->dirty = true;
    bool msg_shown = false;
    while (!S->quit) {
        uint32_t now = now_ms();
        if (S->running && !rtl_usb_present()) {
            stop_dongle();
            snprintf(S->err, sizeof S->err, "the dongle was unplugged");
            S->dirty = true;
        }
        if (!S->running && (int32_t)(now - S->next_try) >= 0) {
            S->next_try = now + 1000;
            if (rtl_usb_present()) {
                if (!start_dongle())
                    S->next_try = now + 3000;
                S->dirty = true;
            } else if (!S->err[0]) {
                snprintf(S->err, sizeof S->err, "no RTL-SDR dongle plugged in");
                S->dirty = true;
            }
        }
        pump();
        int k;
        while (!S->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!S->quit && mouse_event(&e))
            mouse(&e);
        mouse_pointer_update();
        pump();
        scan_step();
        if (S->running && (int32_t)(now - S->next_spec) >= 0 && spectrum_step()) {
            S->next_spec = now + SPEC_EVERY_MS;
            if (!S->dirty && S->dlg == DLG_NONE)
                spectrum_show();
        }
        if ((int32_t)(now - S->next_meter) >= 0) {
            S->next_meter = now + 100;
            if (!S->dirty && S->dlg == DLG_NONE) {
                S->t = dvi_framebuf;
                draw_meter();
                draw_top();
            }
        }
        bool msg_on = S->msg[0] && now < S->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            S->dirty = true;
        }
        if (S->dirty && !S->quit && !S->drag) {
            S->dirty = false;
            draw();
            show();
        } else if (S->dirty && S->drag) {
            // Dragging a slider: just the controls.
            S->dirty = false;
            S->t = dvi_framebuf;
            draw_controls();
            draw_meter();
        }
        if (!S->running)
            sleep_ms(5);
        else if (!rtl_usb_read(&(uint32_t){0}))
            sleep_us(500);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
}

// ---- The command ------------------------------------------------------------------------------------------

/// Prints how to use the command.
static void usage(void) {
    printf("usage: sdr [MHz] [wfm|nfm|am]   the receiver (an RTL-SDR dongle on the hub)\n"
           "       sdr info                 the dongle and its tuner\n");
}

/// `sdr info`: what the dongle is.
static void info(void) {
    if (!rtl_usb_present()) {
        printf("sdr: no RTL-SDR dongle plugged in\n");
        return;
    }
    uint16_t vid, pid;
    rtl_usb_ids(&vid, &pid);
    rtl_dev_t *d = psram_malloc(sizeof *d);
    if (!d)
        return;
    void (*was)(void) = core1_job_now();  // the internet radio's decoding, say
    core1_job_start(rtl_usb_job);
    char err[64];
    bool ok = rtl_open(d, err, sizeof err);
    printf("USB %04x:%04x, %u-byte packets: %s %s\n", vid, pid, rtl_usb_packet_size(), d->maker, d->product);
    if (ok)
        printf("tuner %s%s; 0.5-1766 MHz, %s\n", rtl_tuner_name(d->tuner), d->blog_v4 ? " (RTL-SDR Blog V4)" : "",
               d->blog_v4 ? "HF through its upconverter" : d->tuner == RTL_TUNER_R820T ? "HF by direct sampling below 24 MHz" : "24 MHz up");
    else
        printf("sdr: %s\n", err);
    rtl_close(d);
    if (was)
        core1_job_start(was);
    else
        core1_job_stop();
    psram_free(d);
}

void sdr_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        usage();
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "info")) {
        info();
        return;
    }
    if (!start()) {
        printf("sdr: not enough memory\n");
        return;
    }
    for (int i = 1; i < argc; ++i) {
        double mhz = atof(argv[i]);
        if (mhz > 0)
            S->freq = (uint32_t)(mhz * 1e6 + 0.5);
        for (int m = 0; m < DSP_MODES; ++m)
            if (!strcasecmp(argv[i], mode_names[m]))
                S->mode = m;
    }
    receiver();
    save_ini();
    stop();
}
