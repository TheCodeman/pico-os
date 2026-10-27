/**
 * @file fm_ui.c
 * @brief The `fm` command: a graphical FM radio for a Si4703 in the tracker's style (bevelled panels drawn off the screen, the scanout mouse arrow), and subcommands for scripts.
 *
 * The panel: a 7-segment frequency readout with the signal meter, stereo
 * and RDS (the station's name, programme type and radio text); a dial of
 * the whole band with the stations a scan found and the favourites marked
 * (click it to tune); tuning, seek and scan buttons; the favourites and the
 * scan's stations; volume, band, spacing, de-emphasis and seek sensitivity.
 * Everything is kept in /FMRADIO.INI.
 *
 * Memory: the panel's state and off-screen picture are in PSRAM while it
 * runs; nothing stays in SRAM. The radio itself goes on playing after the
 * panel closes (the module has its own audio output).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "si4703_pico.h"

#define W DVI_WIDTH              ///< the screen's width
#define H DVI_HEIGHT             ///< its height
#define INI_PATH "/FMRADIO.INI"  ///< the settings file
#define MAX_FAVS 30              ///< favourites kept
#define MAX_FOUND 100            ///< stations a scan keeps
#define NAME_LEN 24              ///< a station's name

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< lit segments, titles
#define C_CYAN   0x5f  ///< RDS text
#define C_GREEN  0x3c  ///< good
#define C_ORANGE 0xf4  ///< warnings
#define C_RED    0xe0  ///< the dial's needle, errors
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the selected row
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip
#define C_SEG    0x24  ///< unlit segments
#define C_LEDOFF 0x24  ///< an unlit LED

/// Layout.
#define TOP_H    24    ///< the title strip
#define DISP_Y   28    ///< the display panel
#define DISP_H   168   ///< its height
#define DIAL_Y   200   ///< the band dial
#define DIAL_H   64    ///< its height
#define BTN_Y    268   ///< the button row
#define LIST_Y   300   ///< the lists
#define LIST_W   392   ///< their width
#define LIST_H   (H - 4 - LIST_Y)  ///< their height
#define ROW_H    18    ///< a list row
#define ROWS     ((LIST_H - 30) / ROW_H)  ///< list rows shown
#define SET_X    400   ///< the settings panel
#define SET_W    (W - 4 - SET_X)  ///< its width

/// What a click hits.
enum {
    HIT_DIAL = 1, HIT_SEEK_DN, HIT_STEP_DN, HIT_STEP_UP, HIT_SEEK_UP, HIT_SCAN, HIT_FAV, HIT_MUTE, HIT_MONO,
    HIT_TAB, HIT_ROW, HIT_VOL, HIT_VOL_DN, HIT_VOL_UP, HIT_BAND, HIT_SPACE, HIT_DE, HIT_SEEKS, HIT_QUIT,
    HIT_YES, HIT_NO, HIT_SWALLOW,
};

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A station in a list.
typedef struct {
    uint32_t khz;          ///< its frequency
    uint8_t rssi;          ///< its signal when found (scan)
    char name[NAME_LEN];   ///< its name (RDS, or typed), or ""
} station_t;

/// What the panel is doing besides playing.
enum { DO_NONE, DO_SEEK, DO_SCAN_START, DO_SCAN_SEEK, DO_SCAN_LISTEN };

/// Everything the panel and the command keep (PSRAM, while they run).
typedef struct {
    si4703_dev_t dev;       ///< the chip
    si4703_config_t cfg;    ///< the settings
    si4703_pins_t pins;     ///< where it is
    si4703_status_t st;     ///< the last status
    si4703_rds_t rds;       ///< RDS received
    bool bus_open;          ///< the bus is open
    const char *bus_error;  ///< why it couldn't be
    bool found;             ///< the chip answers
    uint32_t khz;           ///< the frequency tuned (or last tuned)
    station_t favs[MAX_FAVS];  ///< the favourites
    int nfavs;              ///< their count
    station_t scan[MAX_FOUND];  ///< the stations the last scan found
    int nscan;              ///< their count
    int tab;                ///< the list shown: 0 favourites, 1 scan
    int sel[2];             ///< each list's selected row
    int top[2];             ///< each list's first row shown
    int busy;               ///< DO_*
    uint32_t busy_until;    ///< when listening for a name stops (scan)
    uint32_t scan_return;   ///< where to go back to after a scan
    char typed[12];         ///< a frequency being typed
    char naming[NAME_LEN];  ///< a favourite's name being typed
    bool naming_on;         ///< it is being typed
    bool asking_quit;       ///< "keep playing?" is up
    bool dragging_volume;   ///< the volume slider is being dragged
    char msg[96];           ///< a message for the title strip
    uint32_t msg_until;     ///< until when
    uint8_t *g;             ///< the off-screen picture (PSRAM)
    hit_t hits[96];         ///< places to click
    int nhits;              ///< their count
    bool dirty;             ///< draw again
    bool quit;              ///< leaving
    bool keep_playing;      ///< leaving with the radio on
} fm_t;

static fm_t *F;  ///< the state, while the panel or a command runs

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// ---- Drawing into the off-screen picture ---------------------------------------------------------

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
        memset(F->g + (y + r) * W + x, c, w);
}

/// Draws a character of the 8x16 font, scale times as big.
/// @param x left
/// @param y top
/// @param c the character
/// @param fg its colour
/// @param bg the background
/// @param scale 1 or more
static void chs(int x, int y, uint8_t c, uint8_t fg, uint8_t bg, int scale) {
    if (x < 0 || y < 0 || x > W - 8 * scale || y > H - 16 * scale)
        return;
    const uint8_t *gl = font8x16[c];
    for (int r = 0; r < 16; ++r)
        for (int i = 0; i < 8; ++i)
            fill(x + i * scale, y + r * scale, scale, scale, gl[r] & (0x80 >> i) ? fg : bg);
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

/// Remembers a place to click.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param id HIT_*
/// @param arg which
static void hit(int x, int y, int w, int h, int id, int arg) {
    if (F->nhits < (int)(sizeof F->hits / sizeof *F->hits))
        F->hits[F->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = F->nhits - 1; i >= 0; --i) {
        const hit_t *h = &F->hits[i];
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

/// An LED.
/// @param x left
/// @param y top
/// @param on lit
/// @param colour its colour when lit
static void led(int x, int y, bool on, uint8_t colour) {
    uint8_t c = on ? colour : C_LEDOFF;
    fill(x + 1, y, 8, 10, c);
    fill(x, y + 1, 10, 8, c);
    if (on)
        fill(x + 2, y + 2, 3, 2, C_TEXT);
}

/// One 7-segment digit of any size.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param t the segments' thickness
/// @param d the digit (0-9), or -1 for a blank
/// @param on the lit colour
static void seven_seg(int x, int y, int w, int h, int t, int d, uint8_t on) {
    static const uint8_t segs[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    uint8_t m = d >= 0 ? segs[d] : 0;
    int hh = (h - t) / 2;
    struct { int x, y, w, h; } r[7] = {
        {t, 0, w - 2 * t, t}, {w - t, t, t, hh - t}, {w - t, hh + t, t, hh - t}, {t, h - t, w - 2 * t, t},
        {0, hh + t, t, hh - t}, {0, t, t, hh - t}, {t, hh, w - 2 * t, t},
    };
    for (int s = 0; s < 7; ++s)
        fill(x + r[s].x, y + r[s].y, r[s].w, r[s].h, m & (1 << s) ? on : C_SEG);
}

// ---- The display ----------------------------------------------------------------------------------------

/// Draws the display: frequency, signal, stereo, RDS.
static void draw_display(void) {
    bevel(4, DISP_Y, W - 8, DISP_H, true, C_FACE);
    bevel(12, DISP_Y + 8, 616, DISP_H - 16, false, C_FIELD);
    bool on = F->found && F->dev.on;
    uint8_t lit = on ? (F->cfg.mute ? C_DIM : C_YELLOW) : C_DIM;
    // The frequency: up to three digits, a point, two decimals.
    int x = 28, y = DISP_Y + 20, dw = 36, dh = 64, t = 6;
    uint32_t v = F->khz / 10;  // in 10 kHz
    if (F->typed[0]) {
        textf(x, y + 20, C_CYAN, C_FIELD, "> %s_  MHz (Enter)", F->typed);
    } else {
        int d[5] = {(int)(v / 10000 % 10), (int)(v / 1000 % 10), (int)(v / 100 % 10), (int)(v / 10 % 10),
                    (int)(v % 10)};
        for (int i = 0; i < 5; ++i) {
            int dx = x + i * (dw + 10) + (i >= 3 ? 14 : 0);
            seven_seg(dx, y, dw, dh, t, i == 0 && !d[0] ? -1 : d[i], lit);
        }
        fill(x + 3 * (dw + 10) - 2, y + dh - t, t + 2, t, lit);  // the point
        text(x + 5 * (dw + 10) + 18, y + dh - 16, "MHz", C_DIM, C_FIELD);
    }
    // Stereo and the signal.
    int sx = 340;
    led(sx, y + 4, on && F->st.stereo, C_GREEN);
    text(sx + 16, y, on && F->st.stereo ? "STEREO" : F->cfg.mono ? "MONO" : "mono", on && F->st.stereo ? C_GREEN : C_DIM, C_FIELD);
    if (F->cfg.mute)
        text(sx + 80, y, "MUTED", C_ORANGE, C_FIELD);
    if (F->busy == DO_SEEK)
        text(sx + 136, y, "SEEKING", C_ORANGE, C_FIELD);
    else if (F->busy)
        textf(sx + 136, y, C_ORANGE, C_FIELD, "SCAN %d", F->nscan);
    text(sx, y + 24, "Signal", C_LABEL, C_FIELD);
    int bars = on ? (F->st.rssi * 30 + 37) / 75 : 0;
    for (int i = 0; i < 30; ++i)
        fill(sx + 56 + i * 7, y + 26, 5, 12, i < bars ? (i < 8 ? C_ORANGE : i < 20 ? C_YELLOW : C_GREEN) : C_LEDOFF);
    textf(sx + 56 + 214, y + 24, C_DIM, C_FIELD, "%2u", on ? F->st.rssi : 0);
    // RDS: the name (twice the size), the programme type, the text.
    const si4703_rds_t *r = &F->rds;
    if (r->ps[0]) {
        for (int i = 0; i < 8; ++i)
            chs(sx + i * 16, y + 46, (uint8_t)r->ps[i], C_CYAN, C_FIELD, 2);
    } else {
        text(sx, y + 54, on ? "RDS ..." : "", C_DIM, C_FIELD);
    }
    const char *pty = si4703_pty_name(r->pty, !F->cfg.de50);
    if (r->groups && pty[0])
        text(sx + 140, y + 54, pty, C_LABEL, C_FIELD);
    if (r->pi)
        textf(sx + 140, y + 70, C_DIM, C_FIELD, "PI %04X", r->pi);
    // The radio text, wrapped over two lines.
    char line[75];
    int n = (int)strlen(r->rt);
    snprintf(line, sizeof line, "%.74s", r->rt);
    text(24, DISP_Y + 112, line, C_TEXT, C_FIELD);
    if (n > 74)
        textf(24, DISP_Y + 130, C_TEXT, C_FIELD, "%.74s", r->rt + 74);
    if (!on)
        text(24, DISP_Y + 112,
             F->bus_open ? "No Si4703 answers: check the wiring and the reset pin (R tries again)"
                         : "I2C can't be opened (see the settings)",
             C_ORANGE, C_FIELD);
}

/// @def DX(k)
/// A frequency's x on the dial (inside draw_dial).

/// Draws the band dial: the scale, the stations found, the favourites, the needle.
static void draw_dial(void) {
    bevel(4, DIAL_Y, W - 8, DIAL_H, true, C_FACE);
    int x0 = 20, x1 = W - 20, y = DIAL_Y + 8, h = DIAL_H - 16;
    bevel(x0 - 6, y, x1 - x0 + 12, h, false, C_FIELD);
    hit(x0 - 6, y, x1 - x0 + 12, h, HIT_DIAL, 0);
    uint32_t lo = si4703_band_bottom(F->cfg.band), hi = si4703_band_top(F->cfg.band);
    int span = (int)(hi - lo);
#define DX(k) (x0 + (int)(((int64_t)(k) - lo) * (x1 - x0) / span))
    // The stations the scan found: bars as high as their signal.
    for (int i = 0; i < F->nscan; ++i) {
        uint32_t k = F->scan[i].khz;
        if (k < lo || k > hi)
            continue;
        int bh = F->scan[i].rssi * (h - 22) / 75 + 2;
        fill(DX(k) - 1, y + h - 20 - bh, 3, bh, C_GREEN);
    }
    // Every MHz a tick, every 2 a number.
    for (uint32_t k = (lo + 999) / 1000 * 1000; k <= hi; k += 1000) {
        int tx = DX(k), mhz = (int)(k / 1000);
        bool big = mhz % 2 == 0;
        fill(tx, y + h - 20, 1, big ? 6 : 3, C_LABEL);
        if (big) {
            char s[8];
            snprintf(s, sizeof s, "%d", mhz);
            text(tx - (int)strlen(s) * 4, y + h - 16, s, C_DIM, C_FIELD);
        }
    }
    // The favourites: little yellow marks at the top.
    for (int i = 0; i < F->nfavs; ++i)
        if (F->favs[i].khz >= lo && F->favs[i].khz <= hi)
            fill(DX(F->favs[i].khz) - 2, y + 3, 5, 4, C_YELLOW);
    // The needle.
    if (F->khz >= lo && F->khz <= hi)
        fill(DX(F->khz) - 1, y + 2, 2, h - 22, C_RED);
#undef DX
}

/// Draws the button row.
static void draw_buttons(void) {
    fill(4, BTN_Y - 2, W - 8, 30, C_BG);
    static const struct { const char *label; int id; } b[] = {
        {"<< Seek", HIT_SEEK_DN}, {"< Step", HIT_STEP_DN}, {"Step >", HIT_STEP_UP}, {"Seek >>", HIT_SEEK_UP},
        {"Scan", HIT_SCAN},       {"+ Fav", HIT_FAV},      {"Mute", HIT_MUTE},       {"Mono", HIT_MONO},
    };
    int x = 4;
    for (unsigned i = 0; i < sizeof b / sizeof *b; ++i) {
        bool lit = (b[i].id == HIT_MUTE && F->cfg.mute) || (b[i].id == HIT_MONO && F->cfg.mono) ||
                   (b[i].id == HIT_SCAN && F->busy >= DO_SCAN_START) || (b[i].id == HIT_SEEK_UP && F->busy == DO_SEEK);
        button(x, BTN_Y, 76, 26, b[i].label, lit, b[i].id, 0);
        x += 79;
    }
}

/// A list's stations and count.
/// @param tab 0 favourites, 1 the scan's
/// @param n set to the count
/// @return the stations
static station_t *list_of(int tab, int *n) {
    *n = tab ? F->nscan : F->nfavs;
    return tab ? F->scan : F->favs;
}

/// Draws the lists: favourites, or the stations the scan found.
static void draw_lists(void) {
    bevel(4, LIST_Y, LIST_W, LIST_H, true, C_FACE);
    button(10, LIST_Y + 4, 120, 22, "Favourites", F->tab == 0, HIT_TAB, 0);
    char s[24];
    snprintf(s, sizeof s, "Scan (%d)", F->nscan);
    button(132, LIST_Y + 4, 120, 22, s, F->tab == 1, HIT_TAB, 1);
    int n;
    station_t *l = list_of(F->tab, &n);
    int *sel = &F->sel[F->tab], *top = &F->top[F->tab];
    if (*sel >= n)
        *sel = n ? n - 1 : 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + ROWS)
        *top = *sel - ROWS + 1;
    int ly = LIST_Y + 30;
    bevel(8, ly - 2, LIST_W - 8, ROWS * ROW_H + 4, false, C_FIELD);
    for (int r = 0; r < ROWS; ++r) {
        int i = *top + r, y = ly + r * ROW_H;
        bool cur = i == *sel && i < n;
        bool playing = i < n && l[i].khz == F->khz;
        uint8_t bg = cur ? C_SEL : (r & 1) ? C_ROWALT : C_FIELD;
        fill(10, y, LIST_W - 12, ROW_H, bg);
        if (i >= n) {
            if (!n && r == 1)
                text(20, y + 1, F->tab ? "No scan yet: Scan (C) goes through the band" :
                                         "No favourites yet: + Fav (F) adds the station", C_DIM, bg);
            continue;
        }
        char f[12];
        si4703_freq_text(l[i].khz, f, sizeof f);
        textf(14, y + 1, playing ? C_YELLOW : C_TEXT, bg, "%c%6s", playing ? '>' : ' ', f);
        textf(78, y + 1, cur ? C_YELLOW : C_CYAN, bg, "%-22.22s", l[i].name);
        if (F->tab == 1)
            textf(LIST_W - 64, y + 1, C_DIM, bg, "%2u dB", l[i].rssi);
        hit(10, y, LIST_W - 12, ROW_H, HIT_ROW, i);
    }
    if (F->naming_on)
        textf(14, ly + ROWS * ROW_H + 4, C_CYAN, C_FACE, "Name: %s_", F->naming);
}

/// Draws the settings: volume, band, spacing, de-emphasis, seek, the chip.
static void draw_settings(void) {
    int x = SET_X, y = LIST_Y;
    bevel(x, y, SET_W, LIST_H, true, C_FACE);
    x += 8;
    y += 8;
    // Volume: a slider of 0-30.
    textf(x, y + 3, C_LABEL, C_FACE, "Vol %2d", F->cfg.volume);
    button(x + 56, y, 20, 22, "-", false, HIT_VOL_DN, 0);
    int vx = x + 80, vw = SET_W - 16 - 80 - 24;
    bevel(vx, y + 4, vw, 14, false, C_FIELD);
    int fw = F->cfg.volume * (vw - 4) / SI4703_VOLUME_MAX;
    fill(vx + 2, y + 6, fw, 10, F->cfg.mute ? C_DIM : C_GREEN);
    hit(vx, y, vw, 22, HIT_VOL, 0);
    button(vx + vw + 4, y, 20, 22, "+", false, HIT_VOL_UP, 0);
    y += 28;
    text(x, y + 3, "Band", C_LABEL, C_FACE);
    static const char *const bands[3] = {"87-108", "76-108", "76-90"};
    for (int b = 0; b < 3; ++b)
        button(x + 40 + b * 58, y, 56, 22, bands[b], F->cfg.band == b, HIT_BAND, b);
    y += 26;
    text(x, y + 3, "Step", C_LABEL, C_FACE);
    static const int spaces[3] = {200, 100, 50};
    for (int s = 0; s < 3; ++s) {
        char l[8];
        snprintf(l, sizeof l, "%dk", spaces[s]);
        button(x + 40 + s * 58, y, 56, 22, l, F->cfg.spacing == spaces[s], HIT_SPACE, spaces[s]);
    }
    y += 26;
    text(x, y + 3, "De-em", C_LABEL, C_FACE);
    button(x + 56, y, 64, 22, "75 us", !F->cfg.de50, HIT_DE, 0);
    button(x + 122, y, 64, 22, "50 us", F->cfg.de50, HIT_DE, 1);
    y += 26;
    text(x, y + 3, "Seek", C_LABEL, C_FACE);
    static const char *const seeks[3] = {"Few", "Normal", "Many"};
    for (int s = 0; s < 3; ++s)
        button(x + 40 + s * 58, y, 56, 22, seeks[s], F->cfg.seek == s, HIT_SEEKS, s);
    y += 30;
    led(x, y + 3, F->found, C_GREEN);
    textf(x + 14, y, F->found ? C_GREEN : C_ORANGE, C_FACE, "%s", F->found ? "Si4703 found" : "No Si4703");
    textf(x + 14, y + 16, C_DIM, C_FACE, "GP%d/GP%d RST GP%d", F->pins.sda, F->pins.scl, F->pins.rst);
    button(x + SET_W - 16 - 56, y + 2, 56, 26, "Quit", false, HIT_QUIT, 0);
}

/// Draws the title strip, with the message or the keys.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    text(8, 4, "Si4703 FM Radio", C_YELLOW, C_STRIP);
    if (F->msg[0] && now_ms() < F->msg_until)
        textf(144, 4, C_TEXT, C_STRIP, "%.60s", F->msg);
    else
        text(144, 4, "\x1b\x1a step  PgUp/Dn seek  C scan  F fav  +- vol  M mute  F1 keys", C_LABEL, C_STRIP);
    fill(0, TOP_H, W, DISP_Y - TOP_H, C_BG);
}

/// Draws the question asked when leaving.
static void draw_quit(void) {
    int w = 400, h = 96, x = (W - w) / 2, y = 180;
    bevel(x - 2, y - 2, w + 4, h + 4, true, C_LIGHT);
    bevel(x, y, w, h, true, C_FACE);
    hit(x, y, w, h, HIT_SWALLOW, 0);
    text(x + 16, y + 14, "Keep the radio playing?", C_YELLOW, C_FACE);
    text(x + 16, y + 34, "(No switches it off)", C_DIM, C_FACE);
    button(x + 16, y + 60, 120, 24, "Yes (Enter)", true, HIT_YES, 0);
    button(x + 144, y + 60, 120, 24, "No, off", false, HIT_NO, 0);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    F->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_top();
    draw_display();
    draw_dial();
    draw_buttons();
    draw_lists();
    draw_settings();
    if (F->asking_quit)
        draw_quit();
}

/// Copies the picture onto the screen.
static void show(void) {
    memcpy(dvi_framebuf, F->g, W * H);
}

/// Sets a message for the title strip (for 4 seconds).
/// @param fmt printf's format
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(F->msg, sizeof F->msg, fmt, ap);
    va_end(ap);
    F->msg_until = now_ms() + 4000;
    F->dirty = true;
}

// ---- /FMRADIO.INI --------------------------------------------------------------------------------

/// Reads a "khz|rssi|name" station.
/// @param s the text
/// @param st set
/// @return it parsed
static bool parse_station(char *s, station_t *st) {
    memset(st, 0, sizeof *st);
    char *a = strchr(s, '|');
    st->khz = strtoul(s, NULL, 10);
    if (st->khz < 60000 || st->khz > 120000)
        return false;
    if (a) {
        char *b = strchr(a + 1, '|');
        st->rssi = (uint8_t)atoi(a + 1);
        if (b)
            snprintf(st->name, sizeof st->name, "%s", b + 1);
    }
    return true;
}

/// Loads /FMRADIO.INI: the pins, the settings, the last station, favourites and the last scan.
static void load_ini(void) {
    si4703_defaults(&F->cfg);
    F->pins = (si4703_pins_t){2, 3, 400, 22};
    F->khz = 0;
    FIL f;
    if (f_open(&f, INI_PATH, FA_READ) != FR_OK)
        return;
    char line[160];
    while (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq++ = 0;
        int v = atoi(eq);
        if (!strcmp(line, "i2c")) {
            int a, b, c, d;
            if (sscanf(eq, "%d,%d,%d,%d", &a, &b, &c, &d) == 4)
                F->pins = (si4703_pins_t){a, b, c, d};
        } else if (!strcmp(line, "freq")) {
            F->khz = strtoul(eq, NULL, 10);
        } else if (!strcmp(line, "volume")) {
            F->cfg.volume = (uint8_t)(v < 0 ? 0 : v > SI4703_VOLUME_MAX ? SI4703_VOLUME_MAX : v);
        } else if (!strcmp(line, "band")) {
            F->cfg.band = (uint8_t)(v >= 0 && v <= 2 ? v : 0);
        } else if (!strcmp(line, "spacing")) {
            F->cfg.spacing = (uint8_t)(v == 200 || v == 50 ? v : 100);
        } else if (!strcmp(line, "deemphasis")) {
            F->cfg.de50 = v == 50;
        } else if (!strcmp(line, "seek")) {
            F->cfg.seek = (uint8_t)(v >= 0 && v <= 2 ? v : 1);
        } else if (!strcmp(line, "mono")) {
            F->cfg.mono = v;
        } else if (!strcmp(line, "mute")) {
            F->cfg.mute = v;
        } else if (!strcmp(line, "fav") && F->nfavs < MAX_FAVS) {
            if (parse_station(eq, &F->favs[F->nfavs]))
                ++F->nfavs;
        } else if (!strcmp(line, "scan") && F->nscan < MAX_FOUND) {
            if (parse_station(eq, &F->scan[F->nscan]))
                ++F->nscan;
        }
    }
    f_close(&f);
}

/// Saves /FMRADIO.INI.
/// @return saved
static bool save_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    f_printf(&f, "# Si4703 FM radio (the fm command)\n");
    f_printf(&f, "# i2c=sda,scl,kHz,reset pin; freq in kHz; fav= and scan= are kHz|signal|name\n");
    f_printf(&f, "i2c=%d,%d,%d,%d\n", F->pins.sda, F->pins.scl, F->pins.khz, F->pins.rst);
    f_printf(&f, "freq=%lu\nvolume=%d\nband=%d\nspacing=%d\ndeemphasis=%d\nseek=%d\nmono=%d\nmute=%d\n",
             (unsigned long)F->khz, F->cfg.volume, F->cfg.band, F->cfg.spacing, F->cfg.de50 ? 50 : 75, F->cfg.seek,
             F->cfg.mono, F->cfg.mute);
    for (int i = 0; i < F->nfavs; ++i)
        f_printf(&f, "fav=%lu|%d|%s\n", (unsigned long)F->favs[i].khz, F->favs[i].rssi, F->favs[i].name);
    for (int i = 0; i < F->nscan; ++i)
        f_printf(&f, "scan=%lu|%d|%s\n", (unsigned long)F->scan[i].khz, F->scan[i].rssi, F->scan[i].name);
    return f_close(&f) == FR_OK;
}

// ---- The radio ------------------------------------------------------------------------------------

/// Opens the bus and finds the chip: attaches to it if it is already
/// playing, else resets and powers it up with the settings.
/// @param power_up power it up if it is off
/// @return it is playing
static bool connect(bool power_up) {
    si4703_bus_t bus;
    F->bus_error = si4703_bus_open(&F->pins, &bus);
    F->bus_open = !F->bus_error;
    F->found = false;
    if (!F->bus_open)
        return false;
    if (si4703_attach(&F->dev, &bus)) {  // playing already: keep it so
        F->found = true;
        si4703_configure(&F->dev, &F->cfg);
        si4703_status(&F->dev, &F->st);
        F->khz = F->st.khz;
        return true;
    }
    F->found = si4703_init(&F->dev, &bus);
    if (!F->found || !power_up)
        return false;
    if (!si4703_power_up(&F->dev, &F->cfg)) {
        F->found = false;
        return false;
    }
    uint32_t lo = si4703_band_bottom(F->cfg.band), hi = si4703_band_top(F->cfg.band);
    if (F->khz < lo || F->khz > hi)
        F->khz = lo;
    si4703_tune(&F->dev, F->khz);
    si4703_status(&F->dev, &F->st);
    F->khz = F->st.khz;
    return true;
}

/// Tunes to a frequency (and starts RDS afresh).
/// @param khz the frequency
static void tune(uint32_t khz) {
    F->busy = DO_NONE;
    if (F->found && F->dev.on) {
        si4703_tune(&F->dev, khz);
        si4703_status(&F->dev, &F->st);
        F->khz = F->st.khz;
    } else {
        F->khz = si4703_channel_khz(&F->cfg, si4703_channel(&F->cfg, khz));
    }
    si4703_rds_clear(&F->rds);
    F->dirty = true;
}

/// Steps a channel up or down (wrapping round the band).
/// @param dir +1 or -1
static void step(int dir) {
    uint32_t lo = si4703_band_bottom(F->cfg.band), hi = si4703_band_top(F->cfg.band);
    int64_t k = (int64_t)F->khz + dir * F->cfg.spacing;
    uint32_t top = si4703_channel_khz(&F->cfg, si4703_channel(&F->cfg, hi));
    if (k < lo)
        k = top;
    if (k > top)
        k = lo;
    tune((uint32_t)k);
}

/// Starts a seek.
/// @param up upwards
static void seek(bool up) {
    if (!F->found || !F->dev.on) {
        message("No Si4703 to seek with");
        return;
    }
    si4703_rds_clear(&F->rds);
    si4703_seek_start(&F->dev, up);
    F->busy = DO_SEEK;
    F->dirty = true;
}

/// Applies the settings (after a band or spacing change, tunes again).
/// @param retune tune again (the band or spacing changed)
static void apply(bool retune) {
    if (F->found && F->dev.on)
        si4703_configure(&F->dev, &F->cfg);
    if (retune)
        tune(F->khz);
    F->dirty = true;
}

/// Starts a scan of the whole band (seeks from the bottom; listens a moment
/// on each station for its RDS name).
static void scan_start(void) {
    if (!F->found || !F->dev.on) {
        message("No Si4703 to scan with");
        return;
    }
    F->scan_return = F->khz;
    F->nscan = 0;
    F->tab = 1;
    F->sel[1] = F->top[1] = 0;
    F->busy = DO_SCAN_START;
    F->dirty = true;
}

/// Ends a scan: back to the station playing before.
/// @param why the message
static void scan_end(const char *why) {
    F->busy = DO_NONE;
    tune(F->scan_return);
    message("%s: %d stations", why, F->nscan);
    save_ini();
}

/// Keeps a seek or scan going (called every few milliseconds).
static void busy_step(void) {
    bool done, found;
    switch (F->busy) {
    case DO_SEEK:
        if (!si4703_seek_poll(&F->dev, &done, &found) || !done)
            return;
        si4703_status(&F->dev, &F->st);
        F->khz = F->st.khz;
        F->busy = DO_NONE;
        if (!found)
            message("No station found (band's end)");
        F->dirty = true;
        return;
    case DO_SCAN_START:
        si4703_tune(&F->dev, si4703_band_bottom(F->cfg.band));
        si4703_seek_start(&F->dev, true);
        F->busy = DO_SCAN_SEEK;
        return;
    case DO_SCAN_SEEK:
        if (!si4703_seek_poll(&F->dev, &done, &found) || !done)
            return;
        si4703_status(&F->dev, &F->st);
        F->khz = F->st.khz;
        if (!found || F->nscan >= MAX_FOUND) {
            scan_end("Scan done");
            return;
        }
        F->scan[F->nscan] = (station_t){F->st.khz, F->st.rssi, ""};
        ++F->nscan;
        si4703_rds_clear(&F->rds);
        F->busy = DO_SCAN_LISTEN;
        F->busy_until = now_ms() + 1500;
        F->dirty = true;
        return;
    case DO_SCAN_LISTEN:
        si4703_rds_poll(&F->dev, &F->rds, &F->st);
        if (F->rds.ps[0] || (int32_t)(now_ms() - F->busy_until) >= 0) {
            station_t *s = &F->scan[F->nscan - 1];
            snprintf(s->name, sizeof s->name, "%s", F->rds.ps);
            // the name, trailing spaces off
            for (int n = (int)strlen(s->name); n && s->name[n - 1] == ' '; --n)
                s->name[n - 1] = 0;
            si4703_seek_start(&F->dev, true);
            F->busy = DO_SCAN_SEEK;
            F->dirty = true;
        }
        return;
    }
}

/// The favourite at a frequency, or -1.
/// @param khz the frequency
/// @return its index
static int fav_index(uint32_t khz) {
    for (int i = 0; i < F->nfavs; ++i)
        if (F->favs[i].khz == khz)
            return i;
    return -1;
}

/// Adds the station playing to the favourites (named by RDS if it has a name),
/// or, if it is one, starts typing a new name for it.
static void add_fav(void) {
    int i = fav_index(F->khz);
    if (i < 0) {
        if (F->nfavs >= MAX_FAVS) {
            message("The favourites are full (%d)", MAX_FAVS);
            return;
        }
        i = F->nfavs++;
        F->favs[i] = (station_t){F->khz, F->st.rssi, ""};
        snprintf(F->favs[i].name, NAME_LEN, "%s", F->rds.ps);
        for (int n = (int)strlen(F->favs[i].name); n && F->favs[i].name[n - 1] == ' '; --n)
            F->favs[i].name[n - 1] = 0;
        message("Added to the favourites (F again names it)");
    } else {
        snprintf(F->naming, sizeof F->naming, "%s", F->favs[i].name);
        F->naming_on = true;
    }
    F->tab = 0;
    F->sel[0] = i;
    save_ini();
    F->dirty = true;
}

/// Deletes the selected favourite.
static void del_fav(void) {
    int i = F->sel[0];
    if (F->tab != 0 || i >= F->nfavs)
        return;
    memmove(&F->favs[i], &F->favs[i + 1], (F->nfavs - i - 1) * sizeof *F->favs);
    --F->nfavs;
    save_ini();
    F->dirty = true;
}

/// Sets the volume (0-30), unmuting.
/// @param v the volume
static void set_volume(int v) {
    F->cfg.volume = (uint8_t)(v < 0 ? 0 : v > SI4703_VOLUME_MAX ? SI4703_VOLUME_MAX : v);
    F->cfg.mute = false;
    apply(false);
}

// ---- Keys and the mouse ------------------------------------------------------------------------------

/// A key while a favourite's name is typed.
/// @param k the key
static void naming_key(int k) {
    int n = strlen(F->naming);
    if (k == '\r' || k == '\n') {
        int i = fav_index(F->khz);
        if (i >= 0)
            snprintf(F->favs[i].name, NAME_LEN, "%s", F->naming);
        F->naming_on = false;
        save_ini();
    } else if (k == KEY_ESC) {
        F->naming_on = false;
    } else if ((k == 8 || k == 0x7f) && n) {
        F->naming[n - 1] = 0;
    } else if (k >= 0x20 && k < 0x7f && k != '|' && n < NAME_LEN - 1) {
        F->naming[n] = (char)k;
        F->naming[n + 1] = 0;
    }
    F->dirty = true;
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    F->dirty = true;
    if (F->asking_quit) {
        if (k == '\r' || k == 'y' || k == 'Y')
            F->quit = F->keep_playing = true;
        else if (k == 'n' || k == 'N')
            F->quit = true, F->keep_playing = false;
        else if (k == KEY_ESC)
            F->asking_quit = false;
        return;
    }
    if (F->naming_on) {
        naming_key(k);
        return;
    }
    int n = strlen(F->typed);
    if (n) {  // typing a frequency
        if (k == '\r' || k == '\n') {
            uint32_t khz;
            if (si4703_parse_freq(F->typed, &khz))
                tune(khz);
            else
                message("%s: give MHz, e.g. 101.1", F->typed);
            F->typed[0] = 0;
        } else if (k == KEY_ESC) {
            F->typed[0] = 0;
        } else if (k == 8 || k == 0x7f) {
            F->typed[n - 1] = 0;
        } else if ((isdigit(k) || k == '.') && n < (int)sizeof F->typed - 1) {
            F->typed[n] = (char)k;
            F->typed[n + 1] = 0;
        }
        return;
    }
    if (F->busy >= DO_SCAN_START) {  // a scan: Esc (or C) stops it
        if (k == KEY_ESC || k == 'c' || k == 'C')
            scan_end("Scan stopped");
        return;
    }
    int ln;
    station_t *l = list_of(F->tab, &ln);
    int *sel = &F->sel[F->tab];
    switch (k) {
    case KEY_LEFT: step(-1); break;
    case KEY_RIGHT: step(1); break;
    case KEY_PGUP: seek(true); break;
    case KEY_PGDN: seek(false); break;
    case KEY_UP: if (*sel > 0) --*sel; break;
    case KEY_DOWN: if (*sel < ln - 1) ++*sel; break;
    case '\r': if (*sel < ln) tune(l[*sel].khz); break;
    case '\t': F->tab ^= 1; break;
    case '+': case '=': set_volume(F->cfg.volume + 1); break;
    case '-': set_volume(F->cfg.volume - 1); break;
    case 'm': case 'M': F->cfg.mute = !F->cfg.mute; apply(false); break;
    case 's': case 'S': F->cfg.mono = !F->cfg.mono; apply(false); break;
    case 'f': case 'F': add_fav(); break;
    case KEY_DEL: del_fav(); break;
    case 'c': case 'C': scan_start(); break;
    case 'r': case 'R':
        if (!connect(true))
            message(F->bus_open ? "No Si4703 answers" : "I2C: %s", F->bus_error ? F->bus_error : "?");
        else
            message("Si4703 playing");
        break;
    case KEY_F1:
        message("Arrows step  PgUp/Dn seek  Up/Down/Enter list  Tab  F fav  Del  S mono  R reset  Esc");
        F->msg_until += 4000;
        break;
    case KEY_ESC:
        if (F->busy == DO_SEEK) {
            si4703_seek_stop(&F->dev);
            F->busy = DO_NONE;
        } else {
            F->asking_quit = true;
        }
        break;
    default:
        if (isdigit(k)) {
            F->typed[0] = (char)k;
            F->typed[1] = 0;
        }
        break;
    }
}

/// Tunes to where the dial was clicked.
/// @param x the click's x
static void dial_click(int x) {
    uint32_t lo = si4703_band_bottom(F->cfg.band), hi = si4703_band_top(F->cfg.band);
    int x0 = 20, x1 = W - 20;
    if (x < x0)
        x = x0;
    if (x > x1)
        x = x1;
    tune(lo + (uint32_t)((int64_t)(x - x0) * (hi - lo) / (x1 - x0)));
}

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    if (e->type == MOUSE_UP) {
        F->dragging_volume = false;
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (F->dragging_volume && (e->buttons & MOUSE_LEFT)) {
            int vx = SET_X + 8 + 80, vw = SET_W - 16 - 80 - 24;
            set_volume((e->x - vx) * SI4703_VOLUME_MAX / (vw - 4));
        }
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type == MOUSE_WHEEL) {
        if (F->busy >= DO_SCAN_START || F->asking_quit)
            return;
        if (h && (h->id == HIT_VOL || h->id == HIT_VOL_DN || h->id == HIT_VOL_UP))
            set_volume(F->cfg.volume + (e->wheel > 0 ? 1 : -1));
        else if (h && h->id == HIT_ROW) {
            int n;
            list_of(F->tab, &n);
            F->sel[F->tab] += e->wheel > 0 ? -1 : 1;
            F->sel[F->tab] = F->sel[F->tab] < 0 ? 0 : F->sel[F->tab] >= n ? n - 1 : F->sel[F->tab];
            F->dirty = true;
        } else
            step(e->wheel > 0 ? 1 : -1);
        return;
    }
    if (e->type != MOUSE_DOWN || !h)
        return;
    F->dirty = true;
    if (F->asking_quit) {
        if (h->id == HIT_YES)
            F->quit = F->keep_playing = true;
        else if (h->id == HIT_NO)
            F->quit = true, F->keep_playing = false;
        return;
    }
    if (F->busy >= DO_SCAN_START) {
        if (h->id == HIT_SCAN)
            scan_end("Scan stopped");
        return;
    }
    switch (h->id) {
    case HIT_DIAL: dial_click(e->x); break;
    case HIT_SEEK_DN: seek(false); break;
    case HIT_STEP_DN: step(-1); break;
    case HIT_STEP_UP: step(1); break;
    case HIT_SEEK_UP: seek(true); break;
    case HIT_SCAN: scan_start(); break;
    case HIT_FAV: add_fav(); break;
    case HIT_MUTE: key('m'); break;
    case HIT_MONO: key('s'); break;
    case HIT_TAB: F->tab = h->arg; break;
    case HIT_ROW: {
        int n;
        station_t *l = list_of(F->tab, &n);
        F->sel[F->tab] = h->arg;
        if (h->arg < n && (e->double_click || l[h->arg].khz != F->khz))
            tune(l[h->arg].khz);
        if (e->button == MOUSE_RIGHT && F->tab == 0)
            del_fav();
        break;
    }
    case HIT_VOL: {
        F->dragging_volume = true;
        int vx = SET_X + 8 + 80, vw = SET_W - 16 - 80 - 24;
        set_volume((e->x - vx) * SI4703_VOLUME_MAX / (vw - 4));
        break;
    }
    case HIT_VOL_DN: set_volume(F->cfg.volume - 1); break;
    case HIT_VOL_UP: set_volume(F->cfg.volume + 1); break;
    case HIT_BAND:
        F->cfg.band = (uint8_t)h->arg;
        apply(true);
        break;
    case HIT_SPACE:
        F->cfg.spacing = (uint8_t)h->arg;
        apply(true);
        break;
    case HIT_DE: F->cfg.de50 = h->arg; apply(false); break;
    case HIT_SEEKS: F->cfg.seek = (uint8_t)h->arg; apply(false); break;
    case HIT_QUIT: F->asking_quit = true; break;
    }
}

// ---- The panel --------------------------------------------------------------------------------------

/// Starts the state (PSRAM) and reads the settings.
/// @return false if there is no memory for it
static bool start(void) {
    F = psram_malloc(sizeof *F);
    if (!F)
        return false;
    memset(F, 0, sizeof *F);
    si4703_rds_clear(&F->rds);
    load_ini();
    return true;
}

/// Frees the state, closing the bus (the radio playing on or off).
/// @param keep_playing leave the radio on
static void stop(bool keep_playing) {
    if (F->bus_open)
        si4703_bus_close(keep_playing && F->found && F->dev.on);
    psram_free(F->g);
    psram_free(F);
    F = NULL;
}

/// The graphical panel.
/// @return leave the radio playing
static bool panel(void) {
    F->g = psram_malloc(W * H);
    if (!F->g) {
        printf("fm: not enough memory\n");
        return true;
    }
    if (connect(true))
        message("Playing %.2f MHz", F->khz / 1000.0);
    else if (!F->bus_open)
        message("I2C: %s", F->bus_error);
    else
        message("No Si4703 answers: check the wiring (R tries again)");
    if (!F->khz)
        F->khz = si4703_band_bottom(F->cfg.band);
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    draw();
    show();
    uint32_t next_poll = 0;
    bool msg_shown = true;
    while (!F->quit) {
        int k;
        while (!F->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!F->quit && mouse_event(&e))
            mouse(&e);
        mouse_pointer_update();
        uint32_t now = now_ms();
        if (F->found && F->dev.on && F->busy)
            busy_step();
        if (F->found && F->dev.on && !F->busy && (int32_t)(now - next_poll) >= 0) {
            // The status and RDS, often enough to catch every group (40 ms).
            si4703_status_t was = F->st;
            char ps[9], rt[65];
            memcpy(ps, F->rds.ps, sizeof ps);
            memcpy(rt, F->rds.rt, sizeof rt);
            if (!si4703_rds_poll(&F->dev, &F->rds, &F->st)) {
                F->found = false;
                message("The Si4703 stopped answering");
            }
            if (was.rssi != F->st.rssi || was.stereo != F->st.stereo || strcmp(ps, F->rds.ps) ||
                strcmp(rt, F->rds.rt))
                F->dirty = true;
            next_poll = now + 20;
        }
        bool msg_on = F->msg[0] && now < F->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            F->dirty = true;
        }
        if (F->dirty && !F->quit) {
            F->dirty = false;
            draw();
            show();
        }
        sleep_ms(F->busy ? 2 : 10);
    }
    if (F->found && F->dev.on && !F->keep_playing)
        si4703_power_down(&F->dev);
    save_ini();
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
    return F->keep_playing;
}

// ---- The command ------------------------------------------------------------------------------------------

/// Listens a moment for RDS, then prints what is playing.
/// @param listen_ms how long to listen for the name and text
static void print_status(uint32_t listen_ms) {
    if (!F->bus_open) {
        printf("I2C: %s\n", F->bus_error);
        return;
    }
    if (!F->found) {
        printf("No Si4703 answers (I2C%d, GP%d/GP%d, reset GP%d)\n", (F->pins.sda / 2) & 1, F->pins.sda,
               F->pins.scl, F->pins.rst);
        return;
    }
    if (!F->dev.on) {
        printf("FM radio: off\n");
        return;
    }
    uint32_t until = now_ms() + listen_ms;
    while ((int32_t)(now_ms() - until) < 0 && !(F->rds.ps[0] && F->rds.rt[0])) {
        si4703_rds_poll(&F->dev, &F->rds, &F->st);
        sleep_ms(20);
    }
    si4703_status(&F->dev, &F->st);
    char f[12];
    printf("FM %s MHz, signal %u dBuV, %s, volume %d%s\n", si4703_freq_text(F->st.khz, f, sizeof f), F->st.rssi,
           F->st.stereo ? "stereo" : "mono", F->cfg.volume, F->cfg.mute ? " (muted)" : "");
    if (F->rds.ps[0])
        printf("  %s  %s\n", F->rds.ps, si4703_pty_name(F->rds.pty, !F->cfg.de50));
    if (F->rds.rt[0])
        printf("  %s\n", F->rds.rt);
}

/// Prints how to use the command.
static void usage(void) {
    printf("usage: fm                      the panel (fmradio too)\n"
           "       fm on [MHz] | off       play (the last station, or MHz) / switch off\n"
           "       fm tune MHz             e.g. fm tune 101.1\n"
           "       fm seek up|down         the next station\n"
           "       fm vol 0-30 | mute [on|off] | mono [on|off]\n"
           "       fm fav [n]              the favourites, or play number n\n"
           "       fm scan                 every station in the band\n"
           "       fm status               what is playing (with RDS)\n"
           "       fm band eu|wide|jp  step 200|100|50  deemph 75|50\n"
           "       fm i2c SDA SCL kHz RST  where the module is\n");
}

/// Runs a subcommand.
/// @param argc the argument count
/// @param argv the arguments
/// @return the radio is left playing
static bool subcommand(int argc, char **argv) {
    const char *c = argv[1];
    if (!strcmp(c, "i2c") && argc > 4) {
        F->pins.sda = atoi(argv[2]);
        F->pins.scl = atoi(argv[3]);
        F->pins.khz = atoi(argv[4]);
        if (argc > 5)
            F->pins.rst = atoi(argv[5]);
        save_ini();
        printf("fm: I2C%d GP%d/GP%d %d kHz, reset GP%d\n", (F->pins.sda / 2) & 1, F->pins.sda, F->pins.scl,
               F->pins.khz, F->pins.rst);
        return true;
    }
    if (!strcmp(c, "fav") && argc < 3) {
        for (int i = 0; i < F->nfavs; ++i) {
            char f[12];
            printf("%2d  %7s  %s\n", i + 1, si4703_freq_text(F->favs[i].khz, f, sizeof f), F->favs[i].name);
        }
        if (!F->nfavs)
            printf("No favourites yet (the panel's + Fav adds them)\n");
        return true;
    }
    if (!strcmp(c, "off")) {
        connect(false);
        if (F->found && F->dev.on)
            si4703_power_down(&F->dev);
        printf("FM radio off\n");
        return false;
    }
    bool status_only = !strcmp(c, "status");
    if (!connect(!status_only)) {
        print_status(0);
        return false;
    }
    if (!strcmp(c, "on") || !strcmp(c, "tune")) {
        uint32_t khz;
        if (argc > 2) {
            if (!si4703_parse_freq(argv[2], &khz)) {
                printf("fm: %s: give MHz, e.g. 101.1\n", argv[2]);
                return true;
            }
            tune(khz);
        }
    } else if (!strcmp(c, "seek")) {
        bool up = argc < 3 || strcmp(argv[2], "down");
        bool done = false, found = false;
        si4703_seek_start(&F->dev, up);
        for (int i = 0; i < 1000 && !done; ++i) {
            si4703_seek_poll(&F->dev, &done, &found);
            sleep_ms(5);
        }
        si4703_status(&F->dev, &F->st);
        F->khz = F->st.khz;
        if (!found)
            printf("No station found (band's end)\n");
    } else if (!strcmp(c, "vol") && argc > 2) {
        set_volume(atoi(argv[2]));
    } else if (!strcmp(c, "mute") || !strcmp(c, "mono")) {
        bool *b = !strcmp(c, "mute") ? &F->cfg.mute : &F->cfg.mono;
        *b = argc > 2 ? strcmp(argv[2], "off") != 0 : !*b;
        apply(false);
    } else if (!strcmp(c, "fav")) {
        int n = atoi(argv[2]) - 1;
        if (n < 0 || n >= F->nfavs) {
            printf("fm: no favourite %s\n", argv[2]);
            return true;
        }
        tune(F->favs[n].khz);
    } else if (!strcmp(c, "band") && argc > 2) {
        F->cfg.band = !strcmp(argv[2], "wide") ? SI4703_BAND_WIDE : !strcmp(argv[2], "jp") ? SI4703_BAND_JP
                                                                                          : SI4703_BAND_EU;
        apply(true);
    } else if (!strcmp(c, "step") && argc > 2) {
        int s = atoi(argv[2]);
        F->cfg.spacing = (uint8_t)(s == 200 || s == 50 ? s : 100);
        apply(true);
    } else if (!strcmp(c, "deemph") && argc > 2) {
        F->cfg.de50 = atoi(argv[2]) == 50;
        apply(false);
    } else if (!strcmp(c, "scan")) {
        printf("Scanning (Esc stops)...\n");
        scan_start();
        while (F->busy && !key_stop()) {
            busy_step();
            sleep_ms(2);
        }
        if (F->busy)
            scan_end("Scan stopped");
        for (int i = 0; i < F->nscan; ++i) {
            char f[12];
            printf("%7s MHz  %2u dBuV  %s\n", si4703_freq_text(F->scan[i].khz, f, sizeof f), F->scan[i].rssi,
                   F->scan[i].name);
        }
        printf("%d stations\n", F->nscan);
    } else if (!status_only) {
        usage();
        return true;
    }
    print_status(status_only ? 2500 : 0);
    save_ini();
    return true;
}

void fm_command(int argc, char **argv) {
    if (!start()) {
        printf("fm: not enough memory\n");
        return;
    }
    bool keep;
    if (argc < 2) {
        keep = panel();
        if (keep && F->found && F->dev.on)
            printf("The FM radio plays on: `fm off` switches it off, `fm` opens the panel.\n");
    } else if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage();
        keep = true;
    } else {
        keep = subcommand(argc, argv);
    }
    stop(keep);
}
