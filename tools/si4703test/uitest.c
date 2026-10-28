/**
 * @file uitest.c
 * @brief The fm panel drawn on a PC against the simulated Si4703, for screenshots and a check that its keys drive the chip, and that every scan row plays when picked by key or click (even in another band).
 *
 *     ./uitest.sh  (builds this, writes fm_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "../../src/fmradio/fm_ui.c"
#include "sim4703.h"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return KEY_NONE; }
bool key_stop(void) { return false; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }

FRESULT f_open(FIL *f, const char *path, int mode) {
    (void)path;
    f->f = fopen("FMRADIO.INI", mode & FA_WRITE ? "w" : "r");
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

const char *si4703_bus_open(const si4703_pins_t *p, si4703_bus_t *bus) {
    (void)p;
    *bus = sim_bus();
    return NULL;
}
void si4703_bus_close(bool keep_running) { (void)keep_running; }

/// The stations on the simulated air.
static const sim_station_t air[] = {
    {88100, 32, true, 0x1234, 14, "KXPR FM ", "Jazz all night long with Gary Vercelli"},
    {89900, 22, true, 0x2222, 3, "KDVS    ", "Community radio from Davis"},
    {91500, 18, false, 0, 0, NULL, NULL},
    {93700, 40, true, 0x3333, 9, "MIX 93.7", "Today's hits and yesterday's favourites"},
    {95500, 45, true, 0x5678, 5, "ROCK 955", "Now playing: Led Zeppelin - Kashmir (Physical Graffiti, 1975)"},
    {98500, 30, true, 0x4444, 10, "KNCI    ", "New country: Luke Combs - Fast Car"},
    {101100, 52, true, 0x9abc, 1, "NEWS 101", "Traffic and weather on the eights: I-80 slow at the causeway"},
    {103300, 12, false, 0, 0, NULL, NULL},
    {105100, 36, true, 0x5555, 9, "KHITS   ", "The Weeknd - Blinding Lights"},
    {107900, 28, true, 0x4321, 15, "CLASSIC ", "Mozart: Symphony No. 40 in G minor, K. 550 - Berlin Philharmonic"},
};

/// An arrow where the mouse would be.
/// @param x left
/// @param y top
static void arrow(int x, int y) {
    static const char *const a[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
        "X....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
    };
    for (int r = 0; r < 16; ++r)
        for (int c = 0; a[r][c]; ++c)
            if (a[r][c] != ' ')
                F->g[(y + r) * W + x + c] = a[r][c] == 'X' ? C_BG : C_TEXT;
}

/// Saves the picture as raw RGB332.
/// @param name the file
static void snap(const char *name) {
    FILE *f = fopen(name, "wb");
    fwrite(F->g, 1, W * H, f);
    fclose(f);
}

/// Lets the radio run a while (status and RDS polled).
/// @param ms how long
static void run_for(int ms) {
    for (int t = 0; t < ms; t += 20) {
        if (F->busy)
            busy_step();
        else
            si4703_rds_poll(&F->dev, &F->rds, &F->st);
        now_us += 20000;
    }
}

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

/// A left click, then the redraw the panel's loop would do.
/// @param x where
/// @param y where
static void click(int x, int y) {
    mouse_event_t e = {MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 0, (int16_t)x, (int16_t)y, false};
    mouse(&e);
    e.type = MOUSE_UP;
    e.buttons = 0;
    mouse(&e);
    draw();
}

/// A key, then the redraw.
/// @param k the key
static void press(int k) {
    key(k);
    draw();
}

/// Every one of 16 scan rows plays when picked, by Down and Enter and by a
/// click, though the band was changed to 76-90 after the scan (on the board
/// only the two rows below 90 MHz played).
static void test_scan_rows(void) {
    F->asking_quit = false;
    F->nscan = 16;
    for (int i = 0; i < 16; ++i)
        F->scan[i] = (station_t){88100 + (uint32_t)i * 1200, (uint8_t)(20 + i), "ROW"};
    F->tab = 1;
    for (int pass = 0; pass < 2; ++pass) {
        F->cfg.band = SI4703_BAND_JP;
        apply(true);
        F->sel[1] = F->top[1] = 0;
        draw();
        for (int i = 0; i < 16; ++i) {
            if (pass == 0) {  // the keys
                if (i)
                    press(KEY_DOWN);
                press('\r');
            } else {  // the mouse, Down scrolling the list when the row is below it
                while (i >= F->top[1] + ROWS)
                    press(KEY_DOWN);
                click(100, LIST_Y + 30 + (i - F->top[1]) * ROW_H + ROW_H / 2);
            }
            CHECK(F->sel[1] == i && F->khz == F->scan[i].khz, "%s row %d: selected %d, playing %lu (want %lu)",
                  pass ? "click" : "key", i, F->sel[1], (unsigned long)F->khz, (unsigned long)F->scan[i].khz);
        }
        CHECK(F->cfg.band == SI4703_BAND_EU, "band %u after 107.9", F->cfg.band);
    }
    printf("scan rows: %s\n", failures ? "FAILED" : "all 16 play by key and click");
}

int main(void) {
    remove("FMRADIO.INI");
    sim_set_stations(air, sizeof air / sizeof *air);
    start();
    F->g = malloc(W * H);
    F->khz = 101100;
    connect(true);
    printf("powered: %d at %lu\n", F->dev.on, (unsigned long)F->khz);
    // A scan, collecting the names.
    scan_start();
    while (F->busy)
        run_for(20);
    printf("scan: %d stations:", F->nscan);
    for (int i = 0; i < F->nscan; ++i)
        printf(" %lu(%s)", (unsigned long)F->scan[i].khz, F->scan[i].name);
    printf("\nback at %lu\n", (unsigned long)F->khz);
    // Favourites from the scan.
    static const int favs[] = {0, 1, 3, 5};
    for (unsigned i = 0; i < sizeof favs / sizeof *favs; ++i) {
        tune(F->scan[favs[i]].khz);
        run_for(800);
        add_fav();
    }
    tune(95500);
    run_for(2000);
    F->tab = 0;
    F->sel[0] = 1;
    F->cfg.volume = 22;
    F->msg[0] = 0;
    draw();
    arrow(DIAL_Y ? 300 : 0, DIAL_Y + 30);
    snap("shot_main.raw");
    printf("playing %lu: '%s' / '%s' stereo %d rssi %u\n", (unsigned long)F->khz, F->rds.ps, F->rds.rt, F->st.stereo,
           F->st.rssi);
    // The scan list.
    F->tab = 1;
    F->sel[1] = 6;
    tune(101100);
    run_for(2000);
    draw();
    snap("shot_scan.raw");
    // Seek keys.
    key(KEY_PGUP);
    while (F->busy)
        run_for(20);
    printf("seek up from 101.1: %lu\n", (unsigned long)F->khz);
    key(KEY_LEFT);
    printf("step down: %lu\n", (unsigned long)F->khz);
    // Japan's wide band, typing a frequency.
    F->cfg.band = SI4703_BAND_WIDE;
    apply(true);
    key('8');
    key('9');
    key('.');
    key('9');
    run_for(100);
    draw();
    snap("shot_type.raw");
    key('\r');
    run_for(1500);
    printf("typed 89.9: %lu '%s'\n", (unsigned long)F->khz, F->rds.ps);
    F->cfg.band = SI4703_BAND_EU;
    apply(true);
    // No chip.
    sim_present = false;
    F->found = false;
    F->rds = (si4703_rds_t){0};
    message("No Si4703 answers: check the wiring (R tries again)");
    draw();
    snap("shot_none.raw");
    sim_present = true;
    F->found = true;
    // Leaving.
    F->asking_quit = true;
    draw();
    snap("shot_quit.raw");
    save_ini();
    test_scan_rows();
    return failures != 0;
}
