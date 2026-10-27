/**
 * @file as_port_pico.c
 * @brief The Applesoft port layer on the Pico 2 mini OS: the HDMI framebuffer, the USB keyboard (and the Debug Probe's serial port), HDMI audio, and files on the USB drive through FatFs.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "as_port.h"
#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "shell.h"
#include "stack_switch.h"

// ---- Memory ------------------------------------------------------------------
// The emulated 64 KB doesn't fit in SRAM next to the framebuffer, so big
// blocks come from the PSRAM.

#define BIG_BLOCK 4096

void *asp_alloc(size_t n) {
    void *p = n >= BIG_BLOCK ? psram_malloc(n) : malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void asp_free(void *p) {
    if (!p)
        return;
    if ((uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x12000000u)
        psram_free(p);
    else
        free(p);
}

// ---- Screen ------------------------------------------------------------------

uint8_t *asp_framebuffer(void) {
    return dvi_framebuf;
}

void asp_screen_begin(void) {
    // The console stops drawing (messages go to the serial port only) and
    // its cursor goes, while the Apple screen is up.
    console_write("\x1b[?25l", 6);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
}

void asp_screen_end(void) {
    stdio_set_driver_enabled(&console_stdio_driver, true);
    console_write("\x1b" "c", 2);  // reset: clear, cursor home and on
}

void asp_mirror(int c) {
    // To the serial terminal: plain ASCII with CR LF line ends.
    if (c == '\r') {
        putchar('\r');
        putchar('\n');
    } else if (c == 7 || c == 8 || (c >= 0x20 && c < 0x7f)) {
        putchar(c);
    }
}

// ---- Keyboard, paddles -------------------------------------------------------

int asp_key(uint32_t timeout_us) {
    int c = getchar_timeout_us(timeout_us);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

bool asp_input_done(void) {
    return false;
}

int asp_paddle(int n) {
    (void)n;
    return 127;
}

bool asp_button(int n) {
    (void)n;
    return false;
}

// ---- Time and sound ----------------------------------------------------------

uint64_t asp_micros(void) {
    return time_us_64();
}

void asp_sleep_us(uint32_t us) {
    sleep_us(us);
}

void asp_tone(unsigned freq_hz, unsigned ms) {
    audio_beep(freq_hz, ms);
}

void asp_speaker_click(uint64_t when_us) {
    audio_speaker_click(when_us);
}

// ---- Files ---------------------------------------------------------------------

struct asp_file {
    FIL fil;
};

static FRESULT last_error;

const char *asp_file_error(void) {
    return fresult_name(last_error);
}

asp_file *asp_open(const char *name, int mode) {
    if (!shell_drive_mounted()) {
        last_error = FR_NOT_READY;
        return NULL;
    }
    static const BYTE modes[] = {
        FA_READ, FA_WRITE | FA_CREATE_ALWAYS, FA_WRITE | FA_OPEN_APPEND,
        FA_READ | FA_WRITE | FA_OPEN_ALWAYS,
    };
    asp_file *f = malloc(sizeof *f);
    if (!f) {
        last_error = FR_NOT_ENOUGH_CORE;
        return NULL;
    }
    last_error = f_open(&f->fil, name, modes[mode]);
    if (last_error != FR_OK) {
        free(f);
        return NULL;
    }
    return f;
}

int asp_read(asp_file *f, void *buf, int n) {
    UINT got;
    last_error = f_read(&f->fil, buf, (UINT)n, &got);
    return last_error == FR_OK ? (int)got : -1;
}

int asp_write(asp_file *f, const void *buf, int n) {
    UINT put;
    last_error = f_write(&f->fil, buf, (UINT)n, &put);
    return last_error == FR_OK && put == (UINT)n ? n : -1;
}

bool asp_seek(asp_file *f, uint32_t pos) {
    last_error = f_lseek(&f->fil, pos);
    return last_error == FR_OK;
}

uint32_t asp_tell(asp_file *f) {
    return (uint32_t)f_tell(&f->fil);
}

uint32_t asp_size(asp_file *f) {
    return (uint32_t)f_size(&f->fil);
}

bool asp_close(asp_file *f) {
    last_error = f_close(&f->fil);
    free(f);
    return last_error == FR_OK;
}

bool asp_exists(const char *name) {
    FILINFO fi;
    return shell_drive_mounted() && f_stat(name, &fi) == FR_OK;
}

bool asp_remove(const char *name) {
    last_error = f_unlink(name);
    return last_error == FR_OK;
}

bool asp_rename(const char *from, const char *to) {
    last_error = f_rename(from, to);
    return last_error == FR_OK;
}

bool asp_list(void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx) {
    if (!shell_drive_mounted()) {
        last_error = FR_NOT_READY;
        return false;
    }
    DIR dir;
    FILINFO fi;
    last_error = f_opendir(&dir, "");
    if (last_error != FR_OK)
        return false;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS))
            continue;
        fn(ctx, fi.fname, (uint32_t)fi.fsize, fi.fattrib & AM_DIR);
    }
    f_closedir(&dir);
    return true;
}

// ---- Other -------------------------------------------------------------------

bool asp_stack_low(void) {
    char here;
    return &here < stack_bottom() + 2048;
}
