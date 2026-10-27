/**
 * @file uitest.c
 * @brief The si5351 panel drawn on a PC against a simulated chip, for screenshots and a check that the panel's keys drive the chip.
 *
 *     ./uitest.sh  (builds this, writes si5351_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "../../src/si5351/si5351_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock

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

FRESULT f_open(FIL *f, const char *path, int mode) {
    (void)path;
    f->f = fopen("SI5351.INI", mode & FA_WRITE ? "w" : "r");
    return f->f ? FR_OK : 1;
}
char *f_gets(char *buf, int n, FIL *f) { return fgets(buf, n, f->f); }
int f_printf(FIL *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f->f, fmt, ap);
    va_end(ap);
    return r;
}
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }

// ---- The simulated chip --------------------------------------------------------------------------------

static uint8_t chip[256];  ///< its registers
static bool present = true;  ///< it answers

/// The simulated chip's write.
static bool sim_write(void *ctx, uint8_t reg, const uint8_t *d, int n) {
    (void)ctx;
    if (!present)
        return false;
    memcpy(chip + reg, d, n);
    return true;
}

/// The simulated chip's read (status: running, locked).
static bool sim_read(void *ctx, uint8_t reg, uint8_t *d, int n) {
    (void)ctx;
    if (!present)
        return false;
    chip[0] = 0;
    memcpy(d, chip + reg, n);
    return true;
}

const char *si5351_bus_open(const si5351_i2c_t *i2c, si5351_bus_t *bus) {
    (void)i2c;
    bus->write = sim_write;
    bus->read = sim_read;
    bus->ctx = NULL;
    return NULL;
}
void si5351_bus_close(void) {}

/// An arrow where the mouse would be (the board draws it as the screen is sent).
static void arrow(int x, int y) {
    static const char *const a[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
        "X....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
    };
    for (int r = 0; r < 16; ++r)
        for (int c = 0; a[r][c]; ++c)
            if (a[r][c] != ' ')
                U->g[(y + r) * W + x + c] = a[r][c] == 'X' ? C_BG : C_TEXT;
}

/// Saves the picture as raw RGB332 (uitest.sh makes PNGs of them).
static void snap(const char *name) {
    FILE *f = fopen(name, "wb");
    fwrite(U->g, 1, W * H, f);
    fclose(f);
}

/// Types keys into the panel.
static void keys(const char *s) {
    for (; *s; ++s)
        key(*s == '\n' ? '\r' : (unsigned char)*s);
}

int main(void) {
    remove("SI5351.INI");
    start();
    U->g = malloc(W * H);
    connect();
    si5351_read_back(&U->dev);
    // 1: three outputs: CLK0 alone on PLL A, CLK1 and CLK2 sharing B.
    U->cfg.out[0] = (si5351_out_t){14074000, true, 3, false, 0, 0};
    U->cfg.out[1] = (si5351_out_t){7074000, true, 2, false, 1, 0};
    U->cfg.out[2] = (si5351_out_t){25000000, true, 1, false, 1, 0};
    apply();
    U->ch = 0;
    U->digit[0] = 3;
    U->hover_ch = 1;
    U->hover_digit = 2;
    message("Last setup sent to the Si5351");
    draw();
    arrow(digit_x(2) + 14, CARD_Y + 1 * (CARD_H + CARD_GAP) + 40);
    snap("shot_main.raw");
    // Tuning with the keys: up three times at 1 kHz.
    U->hover_ch = U->hover_digit = -1;
    key(KEY_UP), key(KEY_UP), key(KEY_UP);
    printf("CLK0 after 3 x up: %lu Hz (want 14077000)\n", (unsigned long)U->cfg.out[0].freq_hz);
    // 2: I/Q at 7.074 MHz, typed in.
    U->cfg.out[2].enabled = false;
    keys("7.074M");
    key('q');
    U->cfg.out[0].phase = 0;
    U->cfg.out[1].drive = 3;
    apply();
    draw();
    snap("shot_iq.raw");
    printf("I/Q: CLK1 PHOFF %d, divider %lu, chip reg 166 = %d\n", U->plan.out[1].phoff,
           (unsigned long)U->plan.out[1].ms.a, chip[166]);
    // 3: typing a frequency into CLK2.
    key('q');
    U->ch = 2;
    U->cfg.out[2].enabled = true;
    apply();
    keys("10.7");
    U->msg[0] = 0;
    draw();
    snap("shot_entry.raw");
    keys("M");
    printf("CLK2 after typing 10.7M: %lu Hz\n", (unsigned long)U->cfg.out[2].freq_hz);
    // 4: memories.
    const char *names[] = {"40m FT8 + 20m FT8", "I/Q 7.074 MHz SDR", "10 MHz reference", "2m beacon 144.4"};
    for (int i = 0; i < 4; ++i) {
        U->mem[i].used = true;
        snprintf(U->mem[i].name, sizeof U->mem[i].name, "%s", names[i]);
        U->mem[i].cfg = U->cfg;
    }
    U->mem[1].cfg.out[0].freq_hz = 7074000;
    U->mem[2].cfg.out[0].freq_hz = 10000000;
    U->mem[3].cfg.out[0].freq_hz = 144400000;
    U->mem_sel = 1;
    key('m');
    draw();
    snap("shot_mem.raw");
    key(KEY_ESC);
    // 5: no chip.
    present = false;
    connect();
    apply();
    message("No Si5351 answers: check the wiring (A tries again)");
    draw();
    snap("shot_none.raw");
    // 6: leaving.
    present = true;
    connect();
    key(KEY_ESC);
    draw();
    snap("shot_quit.raw");
    save_ini();
    return 0;
}
