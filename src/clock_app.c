/**
 * @file clock_app.c
 * @brief The clock command: picks the processor clock the board starts at (clock_setting.h), from a full-screen menu drawn like the tracker's dialogs or from the command line.
 *
 * A new clock takes effect at the next start: the board's USB, HDMI, WiFi,
 * flash and PSRAM are all set up for the clock when it starts, so it is
 * saved and the board restarts.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "clock_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_setting.h"
#include "console.h"
#include "dvi.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "shell.h"
#include "trk.h"
#include "usb_keyboard.h"

/// What each clock means for the rest of the board.
typedef struct {
    unsigned mhz;
    const char *volts, *flash, *psram, *note;
} speed;
static const speed speeds[CLOCK_SETTING_COUNT] = {
    {126, "1.10 V", "42 MHz", "42 MHz", "Slow and cool: everything works, emulators run behind."},
    {252, "1.20 V", "84 MHz", "42 MHz", "The default. Most things keep up; Batman & Robin doesn't."},
    {378, "1.30 V", "94.5 MHz", "47 MHz",
     "An overclock, on trial: half as fast again, for the heaviest Genesis games."},
};

static unsigned running_mhz(void) {
    return (clock_get_hz(clk_sys) + MHZ / 2) / MHZ;
}

static void status(void) {
    unsigned now = running_mhz(), saved = clock_setting_saved();
    printf("Running at %u MHz; starts at %u MHz%s.\n", now, saved,
           clock_setting_fell_back() ? " (it failed to start twice, so this start used the default)" : "");
    for (int i = 0; i < CLOCK_SETTING_COUNT; i++)
        printf("  %3u MHz: core %s, flash %s, PSRAM %s%s\n", speeds[i].mhz, speeds[i].volts, speeds[i].flash,
               speeds[i].psram, speeds[i].mhz == OS_CLOCK_MHZ ? " (default)" : "");
}

/// Saves mhz and restarts the board (with the console back).
static void apply(unsigned mhz) {
    if (!clock_setting_save(mhz)) {
        printf("Couldn't save the clock setting.\n");
        return;
    }
    printf("Saved: the board starts at %u MHz from now on. Restarting...\n", mhz);
    shell_run_command("reset");
}

// ---- The menu ----

#define BOX_X  8
#define BOX_Y  22
#define BOX_W  624
#define BOX_H  434
#define LIST_X 24
#define LIST_Y 96
#define ROW_H  56
#define STATUS_Y (DVI_HEIGHT - 18)

static void draw(int cur) {
    unsigned now = running_mhz(), saved = clock_setting_saved();
    gfx_target = dvi_framebuf;
    gfx_fill(0, 0, DVI_WIDTH, DVI_HEIGHT, C_BG);
    gfx_fill(0, 0, DVI_WIDTH, 18, C_FACE);
    gfx_text(16, 1, "Clock", -1, C_TEXT, C_FACE);
    gfx_text(DVI_WIDTH - 8 - 14 * 8, 1, "Pico 2 mini OS", -1, C_YELLOW, C_FACE);
    gfx_bevel(BOX_X, BOX_Y, BOX_W, BOX_H, true, C_FACE);
    gfx_text(BOX_X + 8, BOX_Y + 4, "Processor clock", -1, C_YELLOW, C_FACE);
    gfx_printf(LIST_X, BOX_Y + 30, 72, C_LABEL, C_FACE, "Running at %u MHz; the board starts at %u MHz.%s", now,
               saved, clock_setting_fell_back() ? " (Fell back.)" : "");
    gfx_text(LIST_X, BOX_Y + 48, "All three give HDMI its 126 MHz. A new clock needs a restart.", -1, C_LABEL,
             C_FACE);
    gfx_bevel(LIST_X - 2, LIST_Y - 2, BOX_W - 2 * (LIST_X - BOX_X) + 4, CLOCK_SETTING_COUNT * ROW_H + 4, false,
              C_FIELD);
    for (int i = 0; i < CLOCK_SETTING_COUNT; i++) {
        const speed *s = &speeds[i];
        int y = LIST_Y + i * ROW_H, w = BOX_W - 2 * (LIST_X - BOX_X);
        bool lit = i == cur;
        uint8_t bg = lit ? C_CURROW : C_FIELD, fg = lit ? C_YELLOW : C_TEXT;
        gfx_fill(LIST_X, y, w, ROW_H, bg);
        gfx_printf(LIST_X + 8, y + 4, 9, fg, bg, "%u MHz", s->mhz);
        gfx_printf(LIST_X + 96, y + 4, 50, lit ? C_YELLOW : C_CYAN, bg, "core %s  flash %s  PSRAM %s", s->volts,
                   s->flash, s->psram);
        char tags[40] = "";
        if (s->mhz == now)
            strcat(tags, " running");
        if (s->mhz == saved)
            strcat(tags, " saved");
        if (s->mhz == OS_CLOCK_MHZ)
            strcat(tags, " default");
        gfx_text(LIST_X + w - 8 - (int)strlen(tags) * 8, y + 4, tags, -1, lit ? C_YELLOW : C_GREEN, bg);
        gfx_text(LIST_X + 96, y + 24, s->note, (w - 104) / 8, lit ? C_YELLOW : C_DIM, bg);
    }
    int by = LIST_Y + CLOCK_SETTING_COUNT * ROW_H + 24, bx = BOX_X + BOX_W - 2 * 128 - 16;
    gfx_bevel(bx, by, 120, 22, true, C_BLUE);
    gfx_text(bx + 28, by + 3, "Restart", -1, C_YELLOW, C_BLUE);
    gfx_bevel(bx + 128, by, 120, 22, true, C_FACE);
    gfx_text(bx + 128 + 36, by + 3, "Cancel", -1, C_TEXT, C_FACE);
    gfx_fill(0, STATUS_Y, DVI_WIDTH, 18, C_FACE);
    gfx_text(8, STATUS_Y + 1, "Up/Down choose  Return/Start restart at it  Esc/F10 leave", -1, C_LABEL, C_FACE);
}

enum { K_UP = 0x100, K_DOWN, K_GO, K_LEAVE };

/// A key from the console (USB keyboard or serial), or the gamepad, or -1.
static int key(void) {
    int c = getchar_timeout_us(0);
    if (c == '\r' || c == '\n')
        return K_GO;
    if (c == 0x1b) {
        int c2 = getchar_timeout_us(2000);
        if (c2 != '[')
            return K_LEAVE;
        int num = 0, fin;
        while ((fin = getchar_timeout_us(2000)) >= '0' && fin <= '9')
            num = num * 10 + fin - '0';
        return fin == 'A' ? K_UP : fin == 'B' ? K_DOWN : fin == '~' && num == 21 ? K_LEAVE : -1;
    }
    static int held = -1;
    uint8_t x, y;
    uint32_t b;
    int k = -1;
    if (usb_gamepad_read(&x, &y, &b))
        k = y < 64 ? K_UP : y > 191 ? K_DOWN : b & (1 | 0xf80) ? K_GO : b & 2 ? K_LEAVE : -1;
    if (k == held)
        return -1;
    held = k;
    return k;
}

static void menu(void) {
    int cur = 0;
    for (int i = 0; i < CLOCK_SETTING_COUNT; i++)
        if (speeds[i].mhz == clock_setting_saved())
            cur = i;
    console_write("\x1b[?25l", 6);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    while (getchar_timeout_us(0) >= 0) {
    }
    unsigned chosen = 0;
    for (;;) {
        draw(cur);
        int k;
        while ((k = key()) < 0)
            sleep_ms(10);
        if (k == K_UP && cur > 0)
            --cur;
        else if (k == K_DOWN && cur < CLOCK_SETTING_COUNT - 1)
            ++cur;
        else if (k == K_LEAVE)
            break;
        else if (k == K_GO) {
            chosen = speeds[cur].mhz;
            break;
        }
    }
    memset(dvi_framebuf, 0, DVI_WIDTH * DVI_HEIGHT);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    console_write("\x1b" "c", 2);
    if (chosen && (chosen != clock_setting_saved() || chosen != running_mhz()))
        apply(chosen);
    else
        status();
}

void clock_main(int argc, char **argv) {
    if (argc == 1) {
        menu();
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "status")) {
        status();
        return;
    }
    unsigned mhz = argc == 2 ? (unsigned)atoi(argv[1]) : 0;
    for (int i = 0; i < CLOCK_SETTING_COUNT; i++)
        if (speeds[i].mhz == mhz) {
            apply(mhz);
            return;
        }
    printf("usage: clock [126 | 252 | 378 | status]\n"
           "  without one: a menu to pick the clock the board starts at\n");
}
