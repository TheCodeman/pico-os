/**
 * @file si5351_ui.c
 * @brief The `si5351` command: a graphical panel for a Si5351A clock generator in the tracker's style (bevelled panels drawn off the screen, the scanout mouse arrow), and subcommands for scripts.
 *
 * Three channel cards, each with a 7-segment frequency readout tuned a digit
 * at a time (click or point at a digit, then the wheel or the arrows), and
 * its drive, inversion, PLL and phase; below them the crystal, calibration,
 * I2C and PLL details. The setup, 8 named memories and the I2C settings are
 * kept in /SI5351.INI; the last setup comes back (and, if asked, goes to the
 * chip) when the panel opens.
 *
 * Memory: the panel's state and off-screen picture are in PSRAM while it
 * runs; nothing stays in SRAM.
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

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "si5351_pico.h"

#define W DVI_WIDTH         ///< the screen's width
#define H DVI_HEIGHT        ///< its height
#define INI_PATH "/SI5351.INI"  ///< the settings file
#define MEMORIES 8          ///< named memories
#define DIGITS 9            ///< readout digits: 100 MHz down to 1 Hz

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< lit segments, titles
#define C_CYAN   0x5f  ///< the digit being tuned
#define C_GREEN  0x3c  ///< good
#define C_ORANGE 0xf4  ///< warnings
#define C_RED    0xe0  ///< errors
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons
#define C_STRIP  0x0b  ///< the title strip
#define C_SEG    0x24  ///< unlit segments
#define C_LEDOFF 0x24  ///< an unlit LED

/// Layout.
#define TOP_H    24                  ///< the title strip's height
#define CARD_Y   28                  ///< the first card's top
#define CARD_H   112                 ///< a card's height
#define CARD_GAP 4                   ///< between cards
#define CARD_X   4                   ///< the cards' left edge
#define CARD_W   632                 ///< their width
#define WELL_X   (CARD_X + 72)       ///< the readout's left edge
#define WELL_W   328                 ///< its width
#define DIG_X    (WELL_X + 10)       ///< the first digit's left edge
#define DIG_W    24                  ///< a digit's width
#define DIG_H    44                  ///< its height
#define DIG_PITCH 32                 ///< digit to digit
#define DIG_GAP  12                  ///< extra between groups of three
#define CTRL_X   (CARD_X + 412)      ///< the controls' column
#define BOT_Y    (CARD_Y + 3 * (CARD_H + CARD_GAP))  ///< the bottom panel
#define BOT_H    (H - 4 - BOT_Y)     ///< its height

/// What a click hits.
enum {
    HIT_CARD = 1, HIT_DIGIT, HIT_ONOFF, HIT_DRIVE, HIT_PLL, HIT_INV, HIT_PHASE, HIT_PHASE_DN, HIT_PHASE_UP,
    HIT_STEP_DN, HIT_STEP_UP, HIT_XTAL, HIT_LOAD, HIT_CAL, HIT_IQ, HIT_MEM, HIT_SAVE, HIT_APPLY, HIT_QUIT,
    HIT_AUTO, HIT_MEM_ROW, HIT_MEM_LOAD, HIT_MEM_STORE, HIT_MEM_CLEAR, HIT_MEM_CLOSE, HIT_YES, HIT_NO,
};

/// A place to click: its rectangle, what it is and which.
typedef struct {
    int16_t x;    ///< left
    int16_t y;    ///< top
    int16_t w;    ///< width
    int16_t h;    ///< height
    int16_t id;   ///< HIT_*
    int16_t arg;  ///< which (channel, digit, value)
} hit_t;

/// What typed text is going into.
enum { ENTRY_NONE, ENTRY_FREQ, ENTRY_PHASE, ENTRY_CAL, ENTRY_NAME };

/// Which dialog is up.
enum { DLG_NONE, DLG_MEMORIES, DLG_QUIT };

/// A named memory.
typedef struct {
    bool used;              ///< it holds a setup
    char name[24];          ///< its name
    si5351_config_t cfg;    ///< the setup
} memory_t;

/// Everything the panel and the command keep (PSRAM, while they run).
typedef struct {
    si5351_config_t cfg;    ///< the setup
    si5351_plan_t plan;     ///< its plan
    si5351_dev_t dev;       ///< the chip
    si5351_i2c_t i2c;       ///< where it is
    bool bus_open;          ///< the I2C bus is open
    const char *bus_error;  ///< why it couldn't be opened
    bool found;             ///< the chip answers
    uint8_t status;         ///< its status register
    bool apply_on_start;    ///< the last setup goes to the chip when the panel opens
    memory_t mem[MEMORIES]; ///< the memories
    uint8_t *g;             ///< the off-screen picture (PSRAM)
    hit_t hits[96];         ///< places to click
    int nhits;              ///< their count
    int ch;                 ///< the channel selected
    int digit[SI5351_OUTPUTS];  ///< each channel's digit being tuned (0 = 1 Hz)
    int hover_ch;           ///< the digit under the pointer: channel (-1 none)
    int hover_digit;        ///< and digit
    int entry;              ///< ENTRY_*
    char text[24];          ///< what has been typed
    int dialog;             ///< DLG_*
    int mem_sel;            ///< the memory selected in the dialog
    char msg[96];           ///< a message for the title strip
    uint32_t msg_until;     ///< until when
    bool dirty;             ///< the picture must be drawn again
    bool quit;              ///< leaving
} ui_t;

static ui_t *U;  ///< the state, while the panel or a command runs

static const uint32_t pow10s[DIGITS] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};  ///< digit weights
static const char *const drive_ma[4] = {"2", "4", "6", "8"};  ///< drive strengths' labels

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
        memset(U->g + (y + r) * W + x, c, w);
}

/// Draws a character of the 8x16 font.
/// @param x left
/// @param y top
/// @param c the character
/// @param fg its colour
/// @param bg the background
static void ch8(int x, int y, uint8_t c, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x > W - 8 || y > H - 16)
        return;
    const uint8_t *gl = font8x16[c];
    uint8_t *d = U->g + y * W + x;
    for (int r = 0; r < 16; ++r, d += W)
        for (int i = 0; i < 8; ++i)
            d[i] = gl[r] & (0x80 >> i) ? fg : bg;
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
        ch8(x, y, (uint8_t)*s, fg, bg);
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
    char buf[96];
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
    if (U->nhits < (int)(sizeof U->hits / sizeof *U->hits))
        U->hits[U->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = U->nhits - 1; i >= 0; --i) {
        const hit_t *h = &U->hits[i];
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

/// An LED: a round-cornered lamp.
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

/// One 7-segment digit.
/// @param x left
/// @param y top
/// @param d the digit (0-9), or -1 for a blank (leading zero)
/// @param on the lit colour
static void seven_seg(int x, int y, int d, uint8_t on) {
    // Segments a-g as bits 0-6.
    static const uint8_t segs[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    uint8_t m = d >= 0 ? segs[d] : 0;
    static const struct { int8_t x, y, w, h; } r[7] = {
        {3, 0, 18, 4}, {20, 3, 4, 18}, {20, 23, 4, 18}, {3, 40, 18, 4},
        {0, 23, 4, 18}, {0, 3, 4, 18}, {3, 20, 18, 4},
    };
    for (int s = 0; s < 7; ++s)
        fill(x + r[s].x, y + r[s].y, r[s].w, r[s].h, m & (1 << s) ? on : C_SEG);
}

/// A digit's left edge in the readout.
/// @param k the digit (0 = 1 Hz, 8 = 100 MHz)
/// @return its x
static int digit_x(int k) {
    int i = DIGITS - 1 - k;  // from the left
    return DIG_X + i * DIG_PITCH + (i / 3) * DIG_GAP;
}

/// A step size as text.
/// @param k the digit
/// @param out the text
/// @param size room in out
static void step_text(int k, char *out, int size) {
    static const char *const s[DIGITS] = {"1 Hz", "10 Hz", "100 Hz", "1 kHz", "10 kHz",
                                          "100 kHz", "1 MHz", "10 MHz", "100 MHz"};
    snprintf(out, size, "%s", s[k]);
}

/// Draws a channel's card.
/// @param ch the channel
static void draw_card(int ch) {
    const si5351_out_t *o = &U->cfg.out[ch];
    const si5351_out_plan_t *p = &U->plan.out[ch];
    int y0 = CARD_Y + ch * (CARD_H + CARD_GAP);
    bool sel = ch == U->ch;
    bool iq_slave = U->cfg.iq && ch == 1;
    bevel(CARD_X, y0, CARD_W, CARD_H, true, C_FACE);
    hit(CARD_X, y0, CARD_W, CARD_H, HIT_CARD, ch);
    if (sel) {  // a cyan frame round the channel being worked on
        fill(CARD_X + 2, y0 + 2, CARD_W - 4, 2, C_CYAN);
        fill(CARD_X + 2, y0 + CARD_H - 4, CARD_W - 4, 2, C_CYAN);
        fill(CARD_X + 2, y0 + 2, 2, CARD_H - 4, C_CYAN);
        fill(CARD_X + CARD_W - 4, y0 + 2, 2, CARD_H - 4, C_CYAN);
    }
    // The channel's name, and on/off.
    textf(CARD_X + 12, y0 + 10, sel ? C_YELLOW : C_TEXT, C_FACE, "CLK%d", ch);
    led(CARD_X + 50, y0 + 13, o->enabled && !p->error, U->found ? C_GREEN : C_ORANGE);  // orange: no chip
    button(CARD_X + 8, y0 + 34, 56, 24, o->enabled ? "ON" : "OFF", o->enabled, HIT_ONOFF, ch);
    if (iq_slave)
        text(CARD_X + 12, y0 + 66, "Q", C_CYAN, C_FACE);
    else if (U->cfg.iq && ch == 0)
        text(CARD_X + 12, y0 + 66, "I", C_CYAN, C_FACE);
    // The readout.
    bevel(WELL_X, y0 + 6, WELL_W, 72, false, C_FIELD);
    uint32_t f = iq_slave ? U->cfg.out[0].freq_hz : o->freq_hz;
    bool editing = U->entry == ENTRY_FREQ && sel;
    uint8_t lit = !o->enabled ? C_DIM : p->error ? C_ORANGE : C_YELLOW;
    int lead = DIGITS - 1;
    while (lead > 0 && f / pow10s[lead] == 0)
        --lead;
    for (int k = 0; k < DIGITS; ++k) {
        int x = digit_x(k), y = y0 + 12;
        if (editing)
            seven_seg(x, y, -1, lit);
        else
            seven_seg(x, y, k <= lead ? (int)(f / pow10s[k] % 10) : -1, lit);
        bool cur = sel && U->digit[ch] == k && !editing;
        bool hov = U->hover_ch == ch && U->hover_digit == k;
        if (cur || hov)
            fill(x, y + DIG_H + 3, DIG_W, 3, cur ? C_CYAN : C_DIM);
        if (!iq_slave)
            hit(x - 4, y0 + 8, DIG_PITCH, 64, HIT_DIGIT, ch * 16 + k);
    }
    // Group points, and the units under the groups.
    for (int g = 1; g < 3; ++g) {
        int x = digit_x(9 - 3 * g) + DIG_W + 2;
        fill(x + 1, y0 + 12 + DIG_H - 4, 4, 4, lit);
    }
    text(digit_x(7) + 4, y0 + 60, "MHz", C_DIM, C_FIELD);
    text(digit_x(4) + 4, y0 + 60, "kHz", C_DIM, C_FIELD);
    text(digit_x(1) + 8, y0 + 60, "Hz", C_DIM, C_FIELD);
    if (editing) {  // typed text over the readout
        fill(WELL_X + 8, y0 + 22, WELL_W - 16, 24, C_FIELD);
        textf(WELL_X + 16, y0 + 26, C_CYAN, C_FIELD, "> %s_   (k, M or Enter)", U->text);
    }
    // Under the readout: the step and what is really made.
    char step[16];
    step_text(U->digit[ch], step, sizeof step);
    button(CARD_X + 8, y0 + 82, 24, 22, "<", false, HIT_STEP_DN, ch);
    button(CARD_X + 36, y0 + 82, 24, 22, ">", false, HIT_STEP_UP, ch);
    textf(WELL_X, y0 + 85, C_LABEL, C_FACE, "Step %-8s", step);
    if (p->error)
        textf(WELL_X + 112, y0 + 85, C_ORANGE, C_FACE, "%.26s", p->error);
    else if (o->enabled)
        textf(WELL_X + 112, y0 + 85, C_DIM, C_FACE, "%.3f Hz", p->actual_hz);
    // The controls.
    int x = CTRL_X, y = y0 + 8;
    text(x, y + 3, "Drive", C_LABEL, C_FACE);
    for (int d = 0; d < 4; ++d)
        button(x + 56 + d * 30, y, 28, 22, drive_ma[d], o->drive == d, HIT_DRIVE, ch * 16 + d);
    text(x + 178, y + 3, "mA", C_LABEL, C_FACE);
    y += 26;
    text(x, y + 3, "PLL", C_LABEL, C_FACE);
    int pll = p->pll;
    button(x + 56, y, 28, 22, "A", pll == 0, iq_slave ? 0 : HIT_PLL, ch * 16 + 0);
    button(x + 86, y, 28, 22, "B", pll == 1, iq_slave ? 0 : HIT_PLL, ch * 16 + 1);
    button(x + 128, y, 72, 22, "Invert", o->invert, HIT_INV, ch);
    y += 26;
    text(x, y + 3, "Phase", C_LABEL, C_FACE);
    button(x + 56, y, 22, 22, "-", false, iq_slave ? 0 : HIT_PHASE_DN, ch);
    bevel(x + 80, y, 64, 22, false, C_FIELD);
    if (U->entry == ENTRY_PHASE && sel)
        textf(x + 84, y + 3, C_CYAN, C_FIELD, "%s_", U->text);
    else
        textf(x + 84, y + 3, o->phase || iq_slave ? C_TEXT : C_DIM, C_FIELD, "%5.1f%c",
              o->enabled && !p->error ? p->phase_deg : (double)(iq_slave ? 90 : o->phase), 0xf8);
    if (!iq_slave)
        hit(x + 80, y, 64, 22, HIT_PHASE, ch);
    button(x + 146, y, 22, 22, "+", false, iq_slave ? 0 : HIT_PHASE_UP, ch);
    y += 26;
    if (o->enabled && !p->error) {
        if (p->divby4)
            textf(x, y + 3, C_DIM, C_FACE, "MS 4 (DIVBY4)  R %d", 1 << p->r_log2);
        else if (p->integer)
            textf(x, y + 3, C_DIM, C_FACE, "MS %lu int  R %d  PH %d", (unsigned long)p->ms.a, 1 << p->r_log2,
                  p->phoff);
        else
            textf(x, y + 3, C_DIM, C_FACE, "MS %lu+%lu/%lu R%d", (unsigned long)p->ms.a, (unsigned long)p->ms.b,
                  (unsigned long)p->ms.c, 1 << p->r_log2);
    }
}

/// Draws the bottom panel: crystal, calibration, PLLs, the chip and I2C.
static void draw_bottom(void) {
    int y0 = BOT_Y;
    bevel(CARD_X, y0, CARD_W, BOT_H, true, C_FACE);
    int x = CARD_X + 8, y = y0 + 6;
    text(x, y + 3, "Crystal", C_LABEL, C_FACE);
    button(x + 64, y, 64, 22, "25 MHz", U->cfg.xtal_hz == 25000000, HIT_XTAL, 25);
    button(x + 130, y, 64, 22, "27 MHz", U->cfg.xtal_hz == 27000000, HIT_XTAL, 27);
    text(x + 206, y + 3, "Load", C_LABEL, C_FACE);
    static const int loads[3] = {6, 8, 10};
    for (int i = 0; i < 3; ++i) {
        char l[4];
        snprintf(l, sizeof l, "%d", loads[i]);
        button(x + 246 + i * 30, y, 28, 22, l, U->cfg.load_pf == loads[i], HIT_LOAD, loads[i]);
    }
    text(x + 338, y + 3, "pF  Cal", C_LABEL, C_FACE);
    bevel(x + 398, y, 112, 22, false, C_FIELD);
    if (U->entry == ENTRY_CAL)
        textf(x + 402, y + 3, C_CYAN, C_FIELD, "%s_", U->text);
    else
        textf(x + 402, y + 3, U->cfg.cal_ppb ? C_TEXT : C_DIM, C_FIELD, "%+8.3f ppm", U->cfg.cal_ppb / 1000.0);
    hit(x + 398, y, 112, 22, HIT_CAL, 0);
    button(x + 516, y, 100, 22, "I/Q 0+1", U->cfg.iq, HIT_IQ, 0);
    // The PLLs.
    y += 28;
    for (int pl = 0; pl < 2; ++pl, y += 18) {
        const si5351_ratio_t *r = &U->plan.pll[pl];
        bool used = U->plan.pll_used[pl];
        bool lol = U->found && (U->status & (pl ? SI5351_LOL_B : SI5351_LOL_A));
        led(x, y + 3, U->found && used && !lol, lol ? C_RED : C_GREEN);
        char ratio[32];
        if (r->b)
            snprintf(ratio, sizeof ratio, "%lu+%lu/%lu", (unsigned long)r->a, (unsigned long)r->b, (unsigned long)r->c);
        else
            snprintf(ratio, sizeof ratio, "%lu int", (unsigned long)r->a);
        if (used)
            textf(x + 16, y, C_TEXT, C_FACE, "PLL %c  %s = %.6f MHz%s", 'A' + pl, ratio, U->plan.vco_hz[pl] / 1e6,
                  lol ? " NO LOCK" : "");
        else
            textf(x + 16, y, C_DIM, C_FACE, "PLL %c  not used", 'A' + pl);
    }
    // The chip and the bus.
    y += 4;
    led(x, y + 3, U->found, C_GREEN);
    if (!U->bus_open)
        textf(x + 16, y, C_RED, C_FACE, "I2C: %.60s", U->bus_error ? U->bus_error : "closed");
    else
        textf(x + 16, y, U->found ? C_GREEN : C_ORANGE, C_FACE, "%s  I2C%d GP%d/GP%d %d kHz at 0x%02X",
              U->found ? "Si5351 found" : "No Si5351 answers", (U->i2c.sda / 2) & 1, U->i2c.sda, U->i2c.scl,
              U->i2c.khz, U->i2c.addr);
    if (U->found) {
        led(x + 368, y + 3, U->status & SI5351_SYS_INIT, C_ORANGE);
        text(x + 382, y, "INIT", C_LABEL, C_FACE);
    }
    // Buttons on the right, beside the PLLs.
    int bx = CARD_X + CARD_W - 8 - 3 * 64, by = BOT_Y + 34;
    button(bx, by, 62, 24, "Mem", U->dialog == DLG_MEMORIES, HIT_MEM, 0);
    button(bx + 64, by, 62, 24, "Save", false, HIT_SAVE, 0);
    button(bx + 128, by, 62, 24, "Apply", false, HIT_APPLY, 0);
    button(bx, by + 28, 126, 24, U->apply_on_start ? "Auto-apply on" : "Auto-apply off", U->apply_on_start,
           HIT_AUTO, 0);
    button(bx + 128, by + 28, 62, 24, "Quit", false, HIT_QUIT, 0);
}

/// Draws the title strip, with the message or the keys.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    text(8, 4, "Si5351A Clock Generator", C_YELLOW, C_STRIP);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (U->msg[0] && now < U->msg_until)
        textf(208, 4, C_TEXT, C_STRIP, "%.53s", U->msg);
    else
        text(208, 4, "Tab chan  \x1b\x1a digit  \x18\x19 tune  0-9 type  F1 keys", C_LABEL, C_STRIP);
    fill(0, TOP_H, W, CARD_Y - TOP_H, C_BG);
}

/// Draws the memories dialog over the rest.
static void draw_memories(void) {
    int w = 440, h = 260, x = (W - w) / 2, y = 90;
    bevel(x - 2, y - 2, w + 4, h + 4, true, C_LIGHT);
    bevel(x, y, w, h, true, C_FACE);
    hit(x, y, w, h, HIT_MEM_CLOSE, -1);  // (under the rest: swallows clicks)
    fill(x + 2, y + 2, w - 4, 20, C_STRIP);
    text(x + 10, y + 4, "Memories", C_YELLOW, C_STRIP);
    for (int i = 0; i < MEMORIES; ++i) {
        int ry = y + 30 + i * 22;
        bool sel = i == U->mem_sel;
        uint8_t bg = sel ? C_SEL : C_FIELD;
        fill(x + 10, ry, w - 20, 20, bg);
        const memory_t *m = &U->mem[i];
        if (U->entry == ENTRY_NAME && sel)
            textf(x + 16, ry + 2, C_CYAN, bg, "%d  %s_", i + 1, U->text);
        else if (m->used) {
            char f[20];
            si5351_freq_text(m->cfg.out[0].freq_hz, f, sizeof f);
            textf(x + 16, ry + 2, sel ? C_YELLOW : C_TEXT, bg, "%d  %-22.22s CLK0 %s", i + 1, m->name, f);
        } else
            textf(x + 16, ry + 2, C_DIM, bg, "%d  (empty)", i + 1);
        hit(x + 10, ry, w - 20, 20, HIT_MEM_ROW, i);
    }
    int by = y + h - 32;
    button(x + 10, by, 80, 24, "Load", false, HIT_MEM_LOAD, 0);
    button(x + 94, by, 80, 24, "Store", false, HIT_MEM_STORE, 0);
    button(x + 178, by, 80, 24, "Clear", false, HIT_MEM_CLEAR, 0);
    button(x + w - 90, by, 80, 24, "Close", false, HIT_MEM_CLOSE, 0);
}

/// Draws the question asked when leaving.
static void draw_quit(void) {
    int w = 400, h = 96, x = (W - w) / 2, y = 180;
    bevel(x - 2, y - 2, w + 4, h + 4, true, C_LIGHT);
    bevel(x, y, w, h, true, C_FACE);
    hit(x, y, w, h, HIT_NO + 100, 0);  // swallows clicks
    text(x + 16, y + 14, "Leave the outputs running?", C_YELLOW, C_FACE);
    text(x + 16, y + 34, "(No switches them all off before leaving)", C_DIM, C_FACE);
    button(x + 16, y + 60, 120, 24, "Yes (Enter)", true, HIT_YES, 0);
    button(x + 144, y + 60, 120, 24, "No, stop", false, HIT_NO, 0);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    U->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_top();
    for (int ch = 0; ch < SI5351_OUTPUTS; ++ch)
        draw_card(ch);
    draw_bottom();
    if (U->dialog == DLG_MEMORIES)
        draw_memories();
    else if (U->dialog == DLG_QUIT)
        draw_quit();
}

/// Copies the picture onto the screen.
static void show(void) {
    memcpy(dvi_framebuf, U->g, W * H);
}

// ---- The chip -----------------------------------------------------------------------------------------

/// Sets a message for the title strip (for 4 seconds).
/// @param fmt printf's format
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U->msg, sizeof U->msg, fmt, ap);
    va_end(ap);
    U->msg_until = to_ms_since_boot(get_absolute_time()) + 4000;
    U->dirty = true;
}

/// Opens the bus and looks for the chip.
static void connect(void) {
    si5351_bus_t bus;
    U->bus_error = si5351_bus_open(&U->i2c, &bus);
    U->bus_open = !U->bus_error;
    U->found = false;
    if (!U->bus_open)
        return;
    si5351_init(&U->dev, &bus);
    U->found = si5351_probe(&U->dev, &U->status);
}

/// Plans the setup and, if the chip is there, sends it what changed.
/// @return false if the chip stopped answering
static bool apply(void) {
    U->dirty = true;
    if (!U->found) {
        si5351_plan(&U->cfg, &U->plan);
        return true;
    }
    if (si5351_apply(&U->dev, &U->cfg, &U->plan))
        return true;
    U->found = false;
    message("The Si5351 stopped answering");
    return false;
}

// ---- /SI5351.INI ---------------------------------------------------------------------------------

/// A setup as one line: name|xtal|load|cal|iq|then each output as
/// freq,on,drive,invert,pll,phase.
/// @param name the name
/// @param c the setup
/// @param out the line
/// @param size room in out
static void config_line(const char *name, const si5351_config_t *c, char *out, int size) {
    int n = snprintf(out, size, "%s|%lu|%d|%ld|%d", name, (unsigned long)c->xtal_hz, c->load_pf, (long)c->cal_ppb,
                     c->iq);
    for (int i = 0; i < SI5351_OUTPUTS && n < size; ++i) {
        const si5351_out_t *o = &c->out[i];
        n += snprintf(out + n, size - n, "|%lu,%d,%d,%d,%d,%d", (unsigned long)o->freq_hz, o->enabled, o->drive,
                      o->invert, o->pll, o->phase);
    }
}

/// Reads a setup line back (what config_line makes).
/// @param line the text after "key="
/// @param name set to the name, or NULL
/// @param name_size room in name
/// @param c set to the setup
/// @return it parsed
static bool parse_config(char *line, char *name, int name_size, si5351_config_t *c) {
    si5351_defaults(c);
    char *f[8];
    int n = 0;
    for (char *p = line; n < 8;) {
        f[n++] = p;
        p = strchr(p, '|');
        if (!p)
            break;
        *p++ = 0;
    }
    if (n < 8)
        return false;
    if (name)
        snprintf(name, name_size, "%s", f[0]);
    unsigned long xtal = strtoul(f[1], NULL, 10);
    c->xtal_hz = xtal == 27000000 ? 27000000 : 25000000;
    int load = atoi(f[2]);
    c->load_pf = load == 6 || load == 8 ? load : 10;
    long cal = strtol(f[3], NULL, 10);
    c->cal_ppb = cal > 999999 || cal < -999999 ? 0 : cal;
    c->iq = atoi(f[4]) != 0;
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        unsigned long fr;
        int on, drv, inv, pll, ph;
        if (sscanf(f[5 + i], "%lu,%d,%d,%d,%d,%d", &fr, &on, &drv, &inv, &pll, &ph) != 6)
            return false;
        si5351_out_t *o = &c->out[i];
        o->freq_hz = fr < SI5351_MIN_HZ ? SI5351_MIN_HZ : fr > SI5351_MAX_HZ ? SI5351_MAX_HZ : fr;
        o->enabled = on;
        o->drive = drv & 3;
        o->invert = inv;
        o->pll = pll & 1;
        o->phase = (uint16_t)((ph % 360 + 360) % 360);
    }
    return true;
}

/// Loads /SI5351.INI: I2C settings, auto-apply, the last setup and the memories.
static void load_ini(void) {
    si5351_defaults(&U->cfg);
    U->i2c = (si5351_i2c_t){2, 3, 400, SI5351_ADDR};
    U->apply_on_start = true;
    FIL f;
    if (f_open(&f, INI_PATH, FA_READ) != FR_OK)
        return;
    char line[200];
    while (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq++ = 0;
        if (!strcmp(line, "i2c")) {
            int sda, scl, khz, addr;
            if (sscanf(eq, "%d,%d,%d,%i", &sda, &scl, &khz, &addr) == 4)
                U->i2c = (si5351_i2c_t){sda, scl, khz, addr & 0x7f};
        } else if (!strcmp(line, "apply")) {
            U->apply_on_start = atoi(eq) != 0;
        } else if (!strcmp(line, "last")) {
            if (!parse_config(eq, NULL, 0, &U->cfg))
                si5351_defaults(&U->cfg);
        } else if (!strncmp(line, "mem", 3)) {
            int i = atoi(line + 3) - 1;
            if (i >= 0 && i < MEMORIES)
                U->mem[i].used = parse_config(eq, U->mem[i].name, sizeof U->mem[i].name, &U->mem[i].cfg);
        }
    }
    f_close(&f);
}

/// Saves /SI5351.INI.
/// @return saved
static bool save_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    char line[200];
    f_printf(&f, "# Si5351 clock generator (the si5351 command)\n");
    f_printf(&f, "# i2c=sda,scl,kHz,address  apply=1 sends the last setup to the chip on start\n");
    f_printf(&f, "# setups: name|crystal Hz|load pF|cal ppb|I/Q|then per output Hz,on,drive 0-3,invert,PLL 0/1,phase\n");
    f_printf(&f, "i2c=%d,%d,%d,0x%02X\n", U->i2c.sda, U->i2c.scl, U->i2c.khz, U->i2c.addr);
    f_printf(&f, "apply=%d\n", U->apply_on_start);
    config_line("", &U->cfg, line, sizeof line);
    f_printf(&f, "last=%s\n", line);
    for (int i = 0; i < MEMORIES; ++i)
        if (U->mem[i].used) {
            config_line(U->mem[i].name, &U->mem[i].cfg, line, sizeof line);
            f_printf(&f, "mem%d=%s\n", i + 1, line);
        }
    return f_close(&f) == FR_OK;
}

// ---- Editing --------------------------------------------------------------------------------------

/// Tunes a channel by a step.
/// @param ch the channel
/// @param k the digit (its weight is the step)
/// @param dir +1 up, -1 down
static void tune(int ch, int k, int dir) {
    if (U->cfg.iq && ch == 1)
        ch = 0;
    int64_t f = (int64_t)U->cfg.out[ch].freq_hz + (int64_t)dir * pow10s[k];
    if (f < SI5351_MIN_HZ)
        f = SI5351_MIN_HZ;
    if (f > SI5351_MAX_HZ)
        f = SI5351_MAX_HZ;
    U->cfg.out[ch].freq_hz = (uint32_t)f;
    apply();
}

/// Sets a channel's phase (not CLK1 in I/Q, which is 90).
/// @param ch the channel
/// @param deg degrees
static void set_phase(int ch, int deg) {
    if (U->cfg.iq && ch == 1)
        return;
    U->cfg.out[ch].phase = (uint16_t)((deg % 360 + 360) % 360);
    apply();
    const si5351_out_plan_t *p = &U->plan.out[ch];
    if (U->cfg.out[ch].phase && U->cfg.out[ch].enabled && !p->error && fabs(p->phase_deg - U->cfg.out[ch].phase) > 0.5)
        message("CLK%d: phase needs an integer divider: %.1f made", ch, p->phase_deg);
}

/// Starts typing into a field.
/// @param what ENTRY_*
/// @param first the first character, or 0
static void start_entry(int what, int first) {
    U->entry = what;
    U->text[0] = 0;
    if (first)
        snprintf(U->text, sizeof U->text, "%c", first);
    U->dirty = true;
}

/// Takes what was typed (Enter, or a unit key for a frequency).
/// @param unit 'k' or 'M' for a frequency, else 0
static void finish_entry(int unit) {
    int what = U->entry;
    U->entry = ENTRY_NONE;
    U->dirty = true;
    if (what == ENTRY_FREQ) {
        char t[32];
        snprintf(t, sizeof t, "%s%s", U->text, unit == 'k' ? "k" : unit == 'M' ? "M" : "");
        uint32_t hz;
        if (!si5351_parse_freq(t, &hz) || hz < SI5351_MIN_HZ || hz > SI5351_MAX_HZ) {
            message("%s: give 2.5 kHz to 200 MHz (e.g. 10M, 7074k)", t);
            return;
        }
        int ch = U->cfg.iq && U->ch == 1 ? 0 : U->ch;
        U->cfg.out[ch].freq_hz = hz;
        apply();
    } else if (what == ENTRY_PHASE) {
        set_phase(U->ch, atoi(U->text));
    } else if (what == ENTRY_CAL) {
        double ppm = atof(U->text);
        if (ppm < -999 || ppm > 999) {
            message("Calibration: -999 to 999 ppm");
            return;
        }
        U->cfg.cal_ppb = (int32_t)lround(ppm * 1000);
        apply();
    } else if (what == ENTRY_NAME) {
        memory_t *m = &U->mem[U->mem_sel];
        snprintf(m->name, sizeof m->name, "%s", U->text[0] ? U->text : "unnamed");
        for (char *c = m->name; *c; ++c)
            if (*c == '|')
                *c = '/';
        m->cfg = U->cfg;
        m->used = true;
        save_ini();
        message("Stored in memory %d", U->mem_sel + 1);
    }
}

/// A key while typing into a field.
/// @param k the key
/// @return the key was taken
static bool entry_key(int k) {
    if (!U->entry)
        return false;
    int n = strlen(U->text);
    U->dirty = true;
    if (k == KEY_ESC) {
        U->entry = ENTRY_NONE;
    } else if (k == '\r' || k == '\n') {
        finish_entry(0);
    } else if (k == 8 || k == 0x7f || k == KEY_DEL) {
        if (n)
            U->text[n - 1] = 0;
    } else if (U->entry == ENTRY_FREQ && (k == 'k' || k == 'K' || k == 'm' || k == 'M')) {
        finish_entry(k == 'k' || k == 'K' ? 'k' : 'M');
    } else if (k >= 0x20 && k < 0x7f && n < (int)sizeof U->text - 1) {
        bool ok = U->entry == ENTRY_NAME || isdigit(k) || k == '.' ||
                  (U->entry == ENTRY_CAL && (k == '-' || k == '+'));
        if (ok) {
            U->text[n] = (char)k;
            U->text[n + 1] = 0;
        }
    }
    return true;
}

/// Toggles quadrature (CLK0 I, CLK1 Q).
static void toggle_iq(void) {
    U->cfg.iq = !U->cfg.iq;
    if (U->cfg.iq) {
        U->cfg.out[1].enabled = U->cfg.out[0].enabled;
        U->cfg.out[1].pll = U->cfg.out[0].pll;
    }
    apply();
    if (U->cfg.iq && U->plan.out[1].error)
        message("I/Q: %s", U->plan.out[1].error);
    else if (U->cfg.iq)
        message("I/Q: CLK1 is CLK0 delayed 90 degrees");
}

/// Loads memory i.
/// @param i the memory
static void recall(int i) {
    if (!U->mem[i].used) {
        message("Memory %d is empty", i + 1);
        return;
    }
    U->cfg = U->mem[i].cfg;
    apply();
    message("Memory %d: %s", i + 1, U->mem[i].name);
}

/// A key in the memories dialog.
/// @param k the key
static void memory_key(int k) {
    U->dirty = true;
    if (k == KEY_UP && U->mem_sel > 0)
        --U->mem_sel;
    else if (k == KEY_DOWN && U->mem_sel < MEMORIES - 1)
        ++U->mem_sel;
    else if (k >= '1' && k <= '0' + MEMORIES)
        U->mem_sel = k - '1';
    else if (k == '\r' || k == 'l' || k == 'L') {
        recall(U->mem_sel);
        U->dialog = DLG_NONE;
    } else if (k == 's' || k == 'S')
        start_entry(ENTRY_NAME, 0);
    else if (k == KEY_DEL || k == 'c' || k == 'C') {
        U->mem[U->mem_sel].used = false;
        save_ini();
    } else if (k == KEY_ESC || k == 'm' || k == 'M' || k == KEY_F2)
        U->dialog = DLG_NONE;
}

/// Leaves the panel: outputs left running, or switched off.
/// @param keep_running leave them as they are
static void leave(bool keep_running) {
    if (!keep_running && U->found)
        si5351_all_off(&U->dev);
    U->quit = true;
}

/// F1: the keys, shown as messages in turn.
static void help(void) {
    message("o on/off  d drive  i invert  p PLL  h phase  q I/Q  x crystal  l load  c cal  m memories  s save  a apply  Esc quit");
    U->msg_until += 4000;
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    U->dirty = true;
    if (U->dialog == DLG_QUIT) {
        if (k == '\r' || k == 'y' || k == 'Y')
            leave(true);
        else if (k == 'n' || k == 'N')
            leave(false);
        else if (k == KEY_ESC)
            U->dialog = DLG_NONE;
        return;
    }
    if (entry_key(k))
        return;
    if (U->dialog == DLG_MEMORIES) {
        memory_key(k);
        return;
    }
    int ch = U->ch;
    si5351_out_t *o = &U->cfg.out[ch];
    switch (k) {
    case '\t': U->ch = (ch + 1) % SI5351_OUTPUTS; break;
    case KEY_PGUP: U->ch = (ch + SI5351_OUTPUTS - 1) % SI5351_OUTPUTS; break;
    case KEY_PGDN: U->ch = (ch + 1) % SI5351_OUTPUTS; break;
    case KEY_LEFT: if (U->digit[ch] < DIGITS - 1) ++U->digit[ch]; break;
    case KEY_RIGHT: if (U->digit[ch] > 0) --U->digit[ch]; break;
    case KEY_UP: tune(ch, U->digit[ch], 1); break;
    case KEY_DOWN: tune(ch, U->digit[ch], -1); break;
    case 'o': case 'O': case ' ':
        o->enabled = !o->enabled;
        apply();
        break;
    case 'd': case 'D': o->drive = (o->drive + 1) & 3; apply(); break;
    case 'i': case 'I': o->invert = !o->invert; apply(); break;
    case 'p': case 'P':
        if (!(U->cfg.iq && ch == 1)) {
            o->pll ^= 1;
            apply();
        }
        break;
    case 'h': case 'H': start_entry(ENTRY_PHASE, 0); break;
    case 'q': case 'Q': toggle_iq(); break;
    case 'x': case 'X':
        U->cfg.xtal_hz = U->cfg.xtal_hz == 25000000 ? 27000000 : 25000000;
        apply();
        break;
    case 'l': case 'L':
        U->cfg.load_pf = U->cfg.load_pf == 6 ? 8 : U->cfg.load_pf == 8 ? 10 : 6;
        apply();
        break;
    case 'c': case 'C': start_entry(ENTRY_CAL, 0); break;
    case 'm': case 'M': case KEY_F2: U->dialog = DLG_MEMORIES; break;
    case 's': case 'S': message(save_ini() ? "Saved to " INI_PATH : "Can't write " INI_PATH); break;
    case 'a': case 'A':
        connect();
        if (U->found) {
            si5351_invalidate(&U->dev);
            apply();
            message("Everything sent to the chip again");
        } else {
            apply();
            message("No Si5351 answers at 0x%02X", U->i2c.addr);
        }
        break;
    case KEY_F1: help(); break;
    case KEY_ESC: U->dialog = DLG_QUIT; break;
    default:
        if (isdigit(k) || k == '.')
            start_entry(ENTRY_FREQ, k);
        break;
    }
}

/// Handles a click on a place.
/// @param h the place
/// @param e the event
static void click(const hit_t *h, const mouse_event_t *e) {
    U->dirty = true;
    int ch = h->arg / 16, v = h->arg % 16;
    if (U->dialog == DLG_QUIT) {
        if (h->id == HIT_YES)
            leave(true);
        else if (h->id == HIT_NO)
            leave(false);
        return;
    }
    if (U->dialog == DLG_MEMORIES) {
        switch (h->id) {
        case HIT_MEM_ROW:
            U->mem_sel = h->arg;
            if (e->double_click)
                memory_key('\r');
            break;
        case HIT_MEM_LOAD: memory_key('\r'); break;
        case HIT_MEM_STORE: memory_key('s'); break;
        case HIT_MEM_CLEAR: memory_key(KEY_DEL); break;
        case HIT_MEM_CLOSE: if (h->arg == 0) U->dialog = DLG_NONE; break;
        }
        return;
    }
    if (U->entry)
        finish_entry(0);
    switch (h->id) {
    case HIT_CARD: U->ch = h->arg; break;
    case HIT_DIGIT:
        U->ch = ch;
        U->digit[ch] = v;
        if (e->button == MOUSE_RIGHT)
            tune(ch, v, -1);
        break;
    case HIT_ONOFF: U->ch = h->arg; key('o'); break;
    case HIT_DRIVE: U->ch = ch; U->cfg.out[ch].drive = v; apply(); break;
    case HIT_PLL: U->ch = ch; U->cfg.out[ch].pll = v; apply(); break;
    case HIT_INV: U->ch = h->arg; key('i'); break;
    case HIT_PHASE: U->ch = h->arg; start_entry(ENTRY_PHASE, 0); break;
    case HIT_PHASE_DN: U->ch = h->arg; set_phase(h->arg, U->cfg.out[h->arg].phase - 90); break;
    case HIT_PHASE_UP: U->ch = h->arg; set_phase(h->arg, U->cfg.out[h->arg].phase + 90); break;
    case HIT_STEP_DN: U->ch = h->arg; if (U->digit[h->arg] > 0) --U->digit[h->arg]; break;
    case HIT_STEP_UP: U->ch = h->arg; if (U->digit[h->arg] < DIGITS - 1) ++U->digit[h->arg]; break;
    case HIT_XTAL: U->cfg.xtal_hz = h->arg * 1000000u; apply(); break;
    case HIT_LOAD: U->cfg.load_pf = (uint8_t)h->arg; apply(); break;
    case HIT_CAL: start_entry(ENTRY_CAL, 0); break;
    case HIT_IQ: toggle_iq(); break;
    case HIT_MEM: U->dialog = DLG_MEMORIES; break;
    case HIT_SAVE: key('s'); break;
    case HIT_APPLY: key('a'); break;
    case HIT_QUIT: U->dialog = DLG_QUIT; break;
    case HIT_AUTO: U->apply_on_start = !U->apply_on_start; break;
    }
}

/// Handles a mouse event: clicks, the wheel on a digit, pointing at digits.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type == MOUSE_MOVE) {
        int hc = -1, hd = -1;
        if (h && h->id == HIT_DIGIT && !U->dialog) {
            hc = h->arg / 16;
            hd = h->arg % 16;
        }
        if (hc != U->hover_ch || hd != U->hover_digit) {
            U->hover_ch = hc;
            U->hover_digit = hd;
            U->dirty = true;
        }
    } else if (e->type == MOUSE_WHEEL) {
        if (U->dialog)
            return;
        if (h && h->id == HIT_DIGIT)
            tune(h->arg / 16, h->arg % 16, e->wheel > 0 ? 1 : -1);
        else
            tune(U->ch, U->digit[U->ch], e->wheel > 0 ? 1 : -1);
    } else if (e->type == MOUSE_DOWN && h) {
        click(h, e);
    }
}

// ---- The panel --------------------------------------------------------------------------------------

/// Starts the state (PSRAM).
/// @return false if there is no memory for it
static bool start(void) {
    U = psram_malloc(sizeof *U);
    if (!U)
        return false;
    memset(U, 0, sizeof *U);
    U->hover_ch = U->hover_digit = -1;
    for (int i = 0; i < SI5351_OUTPUTS; ++i)
        U->digit[i] = 3;  // 1 kHz steps
    load_ini();
    return true;
}

/// Frees the state, closing the bus.
static void stop(void) {
    if (U->bus_open)
        si5351_bus_close();
    psram_free(U->g);
    psram_free(U);
    U = NULL;
}

/// The graphical panel.
static void panel(void) {
    U->g = psram_malloc(W * H);
    if (!U->g) {
        printf("si5351: not enough memory\n");
        return;
    }
    connect();
    if (U->found) {
        si5351_read_back(&U->dev);  // so only differences go (running outputs keep going)
        if (U->apply_on_start) {
            apply();
            message("Last setup sent to the Si5351");
        } else {
            si5351_plan(&U->cfg, &U->plan);
            message("Last setup loaded (not sent: A sends it)");
        }
    } else {
        si5351_plan(&U->cfg, &U->plan);
        if (U->bus_open)
            message("No Si5351 answers: check the wiring (A tries again)");
        else
            message("I2C: %s", U->bus_error);
    }
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    draw();
    show();
    uint32_t next_poll = 0;
    bool msg_shown = true;
    while (!U->quit) {
        int k;
        while (!U->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!U->quit && mouse_event(&e))
            mouse(&e);
        mouse_pointer_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - next_poll) >= 0) {  // the status (or the chip coming back)
            uint8_t s = U->status;
            bool was = U->found;
            if (U->bus_open)
                U->found = si5351_probe(&U->dev, &U->status);
            if (U->found != was || s != U->status)
                U->dirty = true;
            if (U->found && !was) {
                si5351_read_back(&U->dev);
                apply();
                message("Si5351 found");
            }
            next_poll = now + 500;
        }
        bool msg_on = U->msg[0] && now < U->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            U->dirty = true;
        }
        if (U->dirty && !U->quit) {
            U->dirty = false;
            draw();
            show();
        }
        sleep_ms(10);
    }
    save_ini();
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
}

// ---- The command ------------------------------------------------------------------------------------------

/// Prints the setup and what the chip says.
static void print_status(void) {
    if (!U->bus_open)
        printf("I2C: %s\n", U->bus_error);
    else if (!U->found)
        printf("No Si5351 answers at 0x%02X (I2C%d, GP%d/GP%d, %d kHz)\n", U->i2c.addr, (U->i2c.sda / 2) & 1,
               U->i2c.sda, U->i2c.scl, U->i2c.khz);
    else
        printf("Si5351 at 0x%02X: %s%s%s%s\n", U->i2c.addr, U->status & SI5351_SYS_INIT ? "starting " : "",
               U->status & SI5351_LOL_A ? "PLL A unlocked " : "", U->status & SI5351_LOL_B ? "PLL B unlocked " : "",
               U->status & (SI5351_SYS_INIT | SI5351_LOL_A | SI5351_LOL_B) ? "" : "running, PLLs locked");
    printf("Crystal %lu MHz, %d pF, %+.3f ppm%s\n", (unsigned long)(U->cfg.xtal_hz / 1000000), U->cfg.load_pf,
           U->cfg.cal_ppb / 1000.0, U->cfg.iq ? ", I/Q on CLK0/CLK1" : "");
    for (int p = 0; p < 2; ++p)
        if (U->plan.pll_used[p])
            printf("PLL %c: %lu + %lu/%lu = %.6f MHz\n", 'A' + p, (unsigned long)U->plan.pll[p].a,
                   (unsigned long)U->plan.pll[p].b, (unsigned long)U->plan.pll[p].c, U->plan.vco_hz[p] / 1e6);
    for (int i = 0; i < SI5351_OUTPUTS; ++i) {
        const si5351_out_t *o = &U->cfg.out[i];
        const si5351_out_plan_t *p = &U->plan.out[i];
        char f[24];
        si5351_freq_text(U->cfg.iq && i == 1 ? U->cfg.out[0].freq_hz : o->freq_hz, f, sizeof f);
        printf("CLK%d: %-4s %-14s %s mA, PLL %c%s", i, o->enabled ? "on" : "off", f, drive_ma[o->drive],
               'A' + p->pll, o->invert ? ", inverted" : "");
        if (o->enabled && p->error)
            printf(" - %s", p->error);
        else if (o->enabled && p->phase_deg)
            printf(", %.1f deg", p->phase_deg);
        printf("\n");
    }
}

/// An output number from an argument.
/// @param s the argument
/// @return 0-2, or -1 (with a message)
static int arg_output(const char *s) {
    if (s && (s[0] >= '0' && s[0] <= '2') && !s[1])
        return s[0] - '0';
    if (s && !strncasecmp(s, "clk", 3) && s[3] >= '0' && s[3] <= '2' && !s[4])
        return s[3] - '0';
    printf("si5351: which output? 0, 1 or 2\n");
    return -1;
}

/// Prints how to use the command.
static void usage(void) {
    printf("usage: si5351                  the panel (clockgen too)\n"
           "       si5351 set N FREQ       e.g. si5351 set 0 10M  (k, M, Hz)\n"
           "       si5351 on N | off N | off all\n"
           "       si5351 drive N 2|4|6|8  pll N A|B  invert N on|off  phase N DEG\n"
           "       si5351 xtal 25|27  load 6|8|10  cal PPM  iq on|off\n"
           "       si5351 recall 1-8 | status | apply\n"
           "       si5351 i2c SDA SCL [kHz [address]]\n");
}

/// Runs a subcommand on U->cfg.
/// @param argc the argument count
/// @param argv the arguments
/// @return 1 to send the setup to the chip, 0 if not, -1 on a mistake
static int subcommand(int argc, char **argv) {
    const char *c = argv[1];
    int n;
    if (!strcmp(c, "status"))
        return 0;
    if (!strcmp(c, "apply")) {
        si5351_invalidate(&U->dev);
        return 1;
    }
    if (!strcmp(c, "set") && argc > 3) {
        uint32_t hz;
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        if (!si5351_parse_freq(argv[3], &hz) || hz < SI5351_MIN_HZ || hz > SI5351_MAX_HZ) {
            printf("si5351: %s: give 2.5 kHz to 200 MHz (e.g. 10M, 7074k)\n", argv[3]);
            return -1;
        }
        U->cfg.out[n].freq_hz = hz;
        U->cfg.out[n].enabled = true;
        return 1;
    }
    if (!strcmp(c, "off") && argc > 2 && !strcmp(argv[2], "all")) {
        for (int i = 0; i < SI5351_OUTPUTS; ++i)
            U->cfg.out[i].enabled = false;
        return 1;
    }
    if ((!strcmp(c, "on") || !strcmp(c, "off")) && argc > 2) {
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        U->cfg.out[n].enabled = !strcmp(c, "on");
        return 1;
    }
    if (!strcmp(c, "drive") && argc > 3) {
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        int ma = atoi(argv[3]);
        if (ma != 2 && ma != 4 && ma != 6 && ma != 8) {
            printf("si5351: drive 2, 4, 6 or 8 (mA)\n");
            return -1;
        }
        U->cfg.out[n].drive = (uint8_t)(ma / 2 - 1);
        return 1;
    }
    if (!strcmp(c, "pll") && argc > 3) {
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        U->cfg.out[n].pll = toupper((unsigned char)argv[3][0]) == 'B';
        return 1;
    }
    if (!strcmp(c, "invert") && argc > 3) {
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        U->cfg.out[n].invert = !strcmp(argv[3], "on");
        return 1;
    }
    if (!strcmp(c, "phase") && argc > 3) {
        if ((n = arg_output(argv[2])) < 0)
            return -1;
        U->cfg.out[n].phase = (uint16_t)((atoi(argv[3]) % 360 + 360) % 360);
        return 1;
    }
    if (!strcmp(c, "xtal") && argc > 2) {
        U->cfg.xtal_hz = atoi(argv[2]) == 27 || atoi(argv[2]) == 27000000 ? 27000000 : 25000000;
        return 1;
    }
    if (!strcmp(c, "load") && argc > 2) {
        int pf = atoi(argv[2]);
        U->cfg.load_pf = pf == 6 || pf == 8 ? pf : 10;
        return 1;
    }
    if (!strcmp(c, "cal") && argc > 2) {
        U->cfg.cal_ppb = (int32_t)lround(atof(argv[2]) * 1000);
        return 1;
    }
    if (!strcmp(c, "iq") && argc > 2) {
        U->cfg.iq = !strcmp(argv[2], "on");
        if (U->cfg.iq)
            U->cfg.out[1].enabled = U->cfg.out[0].enabled;
        return 1;
    }
    if (!strcmp(c, "recall") && argc > 2) {
        n = atoi(argv[2]) - 1;
        if (n < 0 || n >= MEMORIES || !U->mem[n].used) {
            printf("si5351: memory %s is empty\n", argv[2]);
            return -1;
        }
        U->cfg = U->mem[n].cfg;
        return 1;
    }
    if (!strcmp(c, "i2c") && argc > 3) {
        U->i2c.sda = atoi(argv[2]);
        U->i2c.scl = atoi(argv[3]);
        if (argc > 4)
            U->i2c.khz = atoi(argv[4]);
        if (argc > 5)
            U->i2c.addr = (int)strtol(argv[5], NULL, 0) & 0x7f;
        return 0;
    }
    usage();
    return -1;
}

void si5351_command(int argc, char **argv) {
    if (!start()) {
        printf("si5351: not enough memory\n");
        return;
    }
    if (argc < 2) {
        panel();
        stop();
        return;
    }
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage();
        stop();
        return;
    }
    int r = subcommand(argc, argv);
    if (r >= 0) {
        connect();
        // Only what differs is written: outputs left running keep going.
        if (U->found && strcmp(argv[1], "apply"))
            si5351_read_back(&U->dev);
        if (r == 1 && U->found) {
            if (!si5351_apply(&U->dev, &U->cfg, &U->plan))
                printf("si5351: the chip stopped answering\n");
        } else {
            si5351_plan(&U->cfg, &U->plan);
        }
        if (r == 1 && !U->found)
            printf("(saved; no chip to send it to)\n");
        if (r == 1 || !strcmp(argv[1], "i2c"))
            save_ini();
        if (!strcmp(argv[1], "status") || !strcmp(argv[1], "i2c") || !U->found)
            print_status();
        for (int i = 0; r == 1 && i < SI5351_OUTPUTS; ++i)
            if (U->cfg.out[i].enabled && U->plan.out[i].error)
                printf("CLK%d: %s\n", i, U->plan.out[i].error);
    }
    stop();
}
