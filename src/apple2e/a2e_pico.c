/**
 * @file a2e_pico.c
 * @brief The apple2e command on the Pico 2 mini OS.
 *
 * While it runs the scanout shows each framebuffer row twice and only the
 * first 204 rows at that (the picture and the status line, 24 rows down;
 * black above and below), so the rest of the framebuffer holds the Apple's
 * 128 KB of RAM, the Disk II's track buffers and the machine itself, and the
 * SRAM heap is left alone; the machine runs a frame (17030 cycles) at a time
 * on core 0, paced to the IIe's 60 Hz, drawing the lines whose screen memory
 * changed.
 *
 * Keys: the USB keyboard (or the serial console); arrows, Delete and Esc as
 * on the Apple. Caps Lock goes on when it starts, as IIe owners kept it (many
 * programs ignore lower case); the key still switches it. Left Alt or Left Win is Open Apple, Right Alt or Right Win
 * Solid Apple. F12 is Ctrl-Reset (hold Open Apple for a reboot, both Apple
 * keys for the self test). F10 saves changed disks and returns to the shell.
 * F7 and F8 turn the Mockingboard down and up (0-9, 6 unless -v N).
 * F9 opens a list of the disk images in the folder, to put one in a drive
 * (or take it out); the disk it replaces is saved first if it changed.
 *
 * Fast disk (on unless -n; F11 switches it): while a drive's motor is on the
 * machine runs as fast as it can, drawing at 60 Hz and with the speaker
 * quiet, and goes back to 1.02 MHz when the motor stops.
 *
 * A Mockingboard is in slot 4 (unless -m): its sound chips play on core 1,
 * mixed into the HDMI audio with the speaker.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "app_overlay.h"
#include "apple2e.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "a2e.h"
#include "a2e_disk2.h"
#include "a2e_mockingboard.h"
#include "as_font.h"
#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "shell.h"
#include "tusb.h"
#include "usb_keyboard.h"

#define DEFAULT_DIR "/APPLE2E"

/// The framebuffer's second half: 150 KB.
/// The framebuffer rows shown (192 picture, then the status line) and where
/// they go on the 240-line doubled screen; the rest of the framebuffer is
/// free.
#define SHOWN_ROWS 204
#define SHOWN_TOP  24
#define SPARE      (dvi_framebuf + DVI_WIDTH * SHOWN_ROWS)
#define SPARE_SIZE (DVI_WIDTH * (DVI_HEIGHT - SHOWN_ROWS))
#define RAM_SIZE   (0x10000 * 2 + 0x1000 * 2)
#define DISK_AT    RAM_SIZE
#define MACHINE_AT ((DISK_AT + sizeof(a2e_disk2) + 7) & ~(size_t)7)
_Static_assert(MACHINE_AT + sizeof(a2e) <= SPARE_SIZE, "no room for the machine");

#define FRAME_US_X1000 ((uint64_t)A2E_FRAME_CYCLES * 1000000000ull / A2E_CPU_HZ)  ///< 16688 us

/// The SDK's malloc panics when SRAM runs out, so check first (as BASIC does).
static bool sram_free(size_t n) {
    extern char __StackLimit;  // top of the heap, from the SDK's linker script
    struct mallinfo mi = mallinfo();
    return (size_t)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks >= n + 8192;
}

// ---- Files ---------------------------------------------------------------------

static void join(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static bool name_is(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if ((*a | 0x20) != (*b | 0x20))
            return false;
    return !*a && !*b;
}

/// Reads n bytes at pos of path into buf.
static bool read_part(const char *path, uint32_t pos, void *buf, UINT n) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    UINT got = 0;
    bool ok = f_lseek(&f, pos) == FR_OK && f_read(&f, buf, n, &got) == FR_OK && got == n;
    f_close(&f);
    return ok;
}

/// Finds the IIe ROM (16 KB, or 32 KB with the image in the second half;
/// APPLE2E.ROM first) and the Disk II boot ROM (any 256-byte file that is
/// one) in dir.
static bool find_roms(const char *dir, uint8_t *rom, uint8_t p5[256], bool *have_p5) {
    DIR d;
    FILINFO fi;
    char path[300];
    bool have_rom = false;
    *have_p5 = false;
    if (f_opendir(&d, dir) != FR_OK) {
        printf("No folder %s on the drive.\n", dir);
        return false;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR)
            continue;
        join(path, sizeof path, dir, fi.fname);
        if ((fi.fsize == 0x4000 || fi.fsize == 0x8000) && (!have_rom || name_is(fi.fname, "APPLE2E.ROM"))) {
            if (read_part(path, (uint32_t)fi.fsize - 0x4000, rom, 0x4000))
                have_rom = true;
        } else if (fi.fsize == 256 && !*have_p5) {
            *have_p5 = read_part(path, 0, p5, 256) && a2e_disk2_check_rom(p5);
        }
    }
    f_closedir(&d);
    if (!have_rom)
        printf("No Apple IIe ROM (16 or 32 KB, e.g. APPLE2E.ROM) in %s.\n", dir);
    return have_rom;
}

static bool ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && name_is(s + n - e, ext);
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

typedef struct {
    char path[300];
    uint8_t *image;  ///< PSRAM
    bool read_only;
} disk_file;

/// Why the last load_disk failed.
static char disk_error[120] APP_OV_DATA(apple2);

/// Loads a 140 KB image, looking for name as given and then in dir. On
/// failure, says why in disk_error (and on the serial console).
static bool load_disk(disk_file *df, const char *name, const char *dir) {
    FILINFO fi;
    const char *why = NULL;
    snprintf(df->path, sizeof df->path, "%s", name);
    if (f_stat(df->path, &fi) != FR_OK) {
        join(df->path, sizeof df->path, dir, name);
        if (f_stat(df->path, &fi) != FR_OK)
            why = "not found";
    }
    if (!why && fi.fsize != A2E_DISK_SIZE)
        why = "not a 140 KB disk image";
    if (!why && !(df->image = psram_malloc(A2E_DISK_SIZE)))
        why = "no room in the PSRAM";
    if (!why && !read_part(df->path, 0, df->image, A2E_DISK_SIZE)) {
        why = "can't read it";
        psram_free(df->image);
        df->image = NULL;
    }
    if (why) {
        snprintf(disk_error, sizeof disk_error, "%.60s: %s", base_name(name), why);
        printf("%s: %s (looked as given and in %s)\n", name, why, dir);
        return false;
    }
    df->read_only = fi.fattrib & AM_RDO;
    return true;
}

static bool save_disk(const disk_file *df) {
    FIL f;
    UINT put = 0;
    if (f_open(&f, df->path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
        return false;
    bool ok = f_write(&f, df->image, A2E_DISK_SIZE, &put) == FR_OK && put == A2E_DISK_SIZE;
    return f_close(&f) == FR_OK && ok;
}

// ---- The status line under the picture -------------------------------------------

#define STATUS_Y 195
#define C_GREY   RGB332(3, 3, 1)
#define C_RED    RGB332(7, 0, 0)

/// Text in 6x8 cells (the 5x7 font with a column and a row of background).
static void draw_text_bg(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; ++s, x += 6) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 0x20 || ch >= 0x80)
            ch = '?';
        const uint8_t *rows = as_font5x7[ch - 0x20];
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 6; ++c)
                dvi_framebuf[(y + r) * DVI_WIDTH + x + c] =
                    r < 7 && c < 5 && (rows[r] & (0x10 >> c)) ? fg : bg;
    }
}

static void draw_text(int x, int y, const char *s, uint8_t fg) {
    draw_text_bg(x, y, s, fg, 0);
}

/// The keys along the bottom, or for a few seconds a message instead.
static const char status_help[] = "F9 DISKS  F10 EXIT  F11 FAST DISK  F12 RESET  ALT/WIN: APPLE";
static uint64_t message_until;  ///< the frame count when the message goes

static void show_status(const char *text, uint8_t colour) {
    char line[66];
    snprintf(line, sizeof line, "%-64.64s", text);
    draw_text(40, STATUS_Y, line, colour);
}

static void show_message(const char *text, uint64_t frames) {
    show_status(text, C_RED);
    message_until = frames + 180;  // three seconds
}

static void draw_lights(const a2e_disk2 *disk, bool have_card, bool lights[2]) {
    if (!have_card)
        return;
    for (int d = 0; d < 2; ++d) {
        bool on = a2e_disk2_active(disk, d) && disk->drive[d].image;
        if (on == lights[d])
            continue;
        lights[d] = on;
        int x = 520 + d * 50;
        draw_text(x, STATUS_Y, d ? "D2" : "D1", C_GREY);
        for (int r = 0; r < 7; ++r)
            memset(&dvi_framebuf[(STATUS_Y + r) * DVI_WIDTH + x + 16], on ? C_RED : 0, 8);
    }
}

// ---- Keys ------------------------------------------------------------------------

/// Keys waiting to be latched at $C000, one each time the program has read the
/// last (so pasted text over the serial console isn't lost).
static uint8_t keyq[64];
static unsigned keyq_head, keyq_tail;
static int ungetc_key = -1;  ///< a character read after a lone Esc

static void key_put(uint8_t c) {
    unsigned next = (keyq_head + 1) % sizeof keyq;
    if (next != keyq_tail) {
        keyq[keyq_head] = c;
        keyq_head = next;
    }
}

/// A key as typed: ASCII, or one of these for the keys that arrive as escape
/// sequences; -1 if none is waiting.
enum {
    K_UP = 0x100, K_DOWN, K_RIGHT, K_LEFT, K_DELETE, K_PGUP, K_PGDN,
    K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
};

static int get_key(void) {
    int c = getchar_timeout_us(0);
    if (c != 0x1b)
        return c < 0 ? -1 : c;
    int c2 = getchar_timeout_us(2000);
    if (c2 != '[' && c2 != 'O') {
        if (c2 >= 0)
            ungetc_key = c2;
        return 0x1b;  // Esc on its own
    }
    int num = 0, fin;
    while ((fin = getchar_timeout_us(2000)) >= '0' && fin <= '9')
        num = num * 10 + (fin - '0');
    switch (fin) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case '~':
        switch (num) {
        case 3: return K_DELETE;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        case 18: return K_F7;
        case 19: return K_F8;
        case 20: return K_F9;
        case 21: return K_F10;
        case 23: return K_F11;
        case 24: return K_F12;
        }
        break;
    }
    return get_key();  // a sequence we don't use
}

enum { ACT_NONE, ACT_RESET, ACT_EXIT, ACT_FAST, ACT_DISKS, ACT_QUIETER, ACT_LOUDER };

/// Turns the keys typed into Apple keys, or one of the actions above.
static int read_keys(void) {
    int c;
    for (;;) {
        if (ungetc_key >= 0) {
            c = ungetc_key;
            ungetc_key = -1;
        } else if ((c = get_key()) < 0) {
            return ACT_NONE;
        }
        switch (c) {
        case K_UP: key_put(0x0b); break;
        case K_DOWN: key_put(0x0a); break;
        case K_RIGHT: key_put(0x15); break;
        case K_LEFT: key_put(0x08); break;
        case K_DELETE: key_put(0x7f); break;
        case K_F7: return ACT_QUIETER;
        case K_F8: return ACT_LOUDER;
        case K_F9: return ACT_DISKS;
        case K_F10: return ACT_EXIT;
        case K_F11: return ACT_FAST;
        case K_F12: return ACT_RESET;
        default:
            if (c < 0x80)
                key_put(a2e_caps((uint8_t)c, usb_keyboard_caps_lock()));
            break;
        }
    }
}

/// The Apple keys and a USB joystick or gamepad: its stick is paddles 0 and 1
/// and its first three buttons the IIe's buttons 0-2, together with the Alt
/// and Win keys for 0 and 1.
static void read_buttons(a2e *m) {
    uint8_t mods = usb_keyboard_modifiers(), x, y;
    uint32_t pad = 0;
    if (usb_gamepad_read(&x, &y, &pad)) {
        m->paddle[0] = x;
        m->paddle[1] = y;
    }
    m->button[0] = (mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTGUI)) || (pad & 1);
    m->button[1] = (mods & (KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTGUI)) || (pad & 2);
    m->button[2] = pad & 4;
    a2e_key_held(m, usb_keyboard_key_held() || keyq_head != keyq_tail);
}

// ---- Changing disks ------------------------------------------------------------------

#define NAME_MAX_LEN 64
#define LIST_ROWS    18
#define C_BOX        RGB332(0, 0, 1)
#define C_TEXT       RGB332(7, 7, 3)

static int name_cmp(const void *a, const void *b) {
    return strcasecmp(a, b);
}

/// The disk images in dir (140 KB .dsk, .do or .po files), sorted by name.
static int list_disks(const char *dir, char (*names)[NAME_MAX_LEN], int max) {
    DIR d;
    FILINFO fi;
    int n = 0;
    if (f_opendir(&d, dir) != FR_OK)
        return 0;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fi.fsize != A2E_DISK_SIZE ||
            strlen(fi.fname) >= NAME_MAX_LEN)
            continue;
        if (ends_with(fi.fname, ".dsk") || ends_with(fi.fname, ".do") || ends_with(fi.fname, ".po"))
            snprintf(names[n++], NAME_MAX_LEN, "%s", fi.fname);
    }
    f_closedir(&d);
    qsort(names, (size_t)n, NAME_MAX_LEN, name_cmp);
    return n;
}

enum { PICK_CANCEL, PICK_INSERT, PICK_BOOT, PICK_EJECT };

/// The disk list over the picture. Tab switches drive; Return puts the chosen
/// image in it, B puts it in drive 1 and starts the Apple from it, E takes
/// the drive's disk out, Esc goes back. The machine waits meanwhile.
static int pick_disk(const char *dir, const disk_file *df, int *drive, char *out) {
    char(*names)[NAME_MAX_LEN] = psram_malloc(128 * NAME_MAX_LEN);
    if (!names)
        return PICK_CANCEL;
    int n = list_disks(dir, names, 128), sel = 0, top = 0, result = -1;
    char line[100];
    while (result < 0) {
        for (int y = 0; y < 192; ++y)
            memset(&dvi_framebuf[y * DVI_WIDTH + 40], C_BOX, 560);
        snprintf(line, sizeof line, "DRIVE %d: %.80s", *drive + 1,
                 df[*drive].image ? base_name(df[*drive].path) : "(EMPTY)");
        draw_text_bg(52, 4, line, C_TEXT, C_BOX);
        if (!n) {
            snprintf(line, sizeof line, "NO DISK IMAGES (.DSK .DO .PO, 140 KB) IN %s", dir);
            draw_text_bg(52, 26, line, C_GREY, C_BOX);
        }
        if (sel < top)
            top = sel;
        if (sel >= top + LIST_ROWS)
            top = sel - LIST_ROWS + 1;
        for (int i = 0; i < LIST_ROWS && top + i < n; ++i) {
            bool on = top + i == sel;
            snprintf(line, sizeof line, " %-88.88s", names[top + i]);
            draw_text_bg(52, 20 + i * 9, line, on ? C_BOX : C_TEXT, on ? C_TEXT : C_BOX);
        }
        draw_text_bg(52, 183, "RETURN: INSERT  B: INSERT AND BOOT  TAB: DRIVE  E: EJECT  ESC: BACK",
                     C_GREY, C_BOX);
        int k;
        while ((k = get_key()) < 0)
            sleep_ms(5);
        switch (k) {
        case K_UP: if (sel > 0) --sel; break;
        case K_DOWN: if (sel < n - 1) ++sel; break;
        case K_PGUP: sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0; break;
        case K_PGDN: sel = sel + LIST_ROWS < n ? sel + LIST_ROWS : (n ? n - 1 : 0); break;
        case '\t': *drive ^= 1; break;
        case '\r': case 'b': case 'B':
            if (n) {
                snprintf(out, NAME_MAX_LEN, "%s", names[sel]);
                result = k == '\r' ? PICK_INSERT : PICK_BOOT;
                if (result == PICK_BOOT)
                    *drive = 0;
            }
            break;
        case 'e': case 'E': result = PICK_EJECT; break;
        case 0x1b: case K_F9: result = PICK_CANCEL; break;
        }
    }
    psram_free(names);
    return result;
}

/// Takes drive d's disk out (saving it first if it changed) and puts name in,
/// if given.
static bool change_disk(a2e_disk2 *disk, disk_file *df, int d, const char *name, const char *dir) {
    a2e_disk2_flush(disk, d);
    if (df[d].image) {
        if (disk->drive[d].dirty && !save_disk(&df[d]))
            printf("%s: couldn't save the changes!\n", df[d].path);
        a2e_disk2_insert(disk, d, NULL, false, false);
        psram_free(df[d].image);
        df[d].image = NULL;
    }
    if (!name)
        return true;
    if (!load_disk(&df[d], name, dir))
        return false;
    a2e_disk2_insert(disk, d, df[d].image, ends_with(df[d].path, ".po"), df[d].read_only);
    return true;
}

// ---- Sound -----------------------------------------------------------------------

/// Cycle counts to time_us_64() times, on the schedule the frames keep.
static uint64_t sched_us, sched_cycle;

static uint64_t cycle_time(uint64_t cycle) {
    return sched_us + (cycle - sched_cycle) * 1000000ull / A2E_CPU_HZ;
}

static bool accelerating;  ///< fast disk running: no sound

static void speaker(void *user, uint64_t cycle) {
    if (!accelerating)
        audio_speaker_click(cycle_time(cycle));
}

/// While the disk runs fast the frames' times run ahead of the clock, so the
/// sound chips get their writes straight away instead.
static void mockingboard_write(void *user, int chip, uint8_t reg, uint8_t value, uint64_t cycle) {
    audio_ay_write((unsigned)chip, reg, value, accelerating ? time_us_64() : cycle_time(cycle));
}

/// Mockingboard volume steps (audio_ay_gain's units); 6, twice the chips'
/// level, is the default.
static const unsigned volume_gain[10] = {0, 48, 96, 160, 256, 384, 512, 704, 960, 1280};

// ---- The command -------------------------------------------------------------------

void apple2e_main(int argc, char **argv) {
    const char *dir = DEFAULT_DIR;
    const char *disk_name[2] = {NULL, NULL};
    int ndisks = 0;
    bool fast_disk = true, with_mockingboard = true;
    int volume = 6;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-n"))
            fast_disk = false;
        else if (!strcmp(argv[i], "-m"))
            with_mockingboard = false;
        else if (!strcmp(argv[i], "-v") && i + 1 < argc && argv[i + 1][0] >= '0' &&
                 argv[i + 1][0] <= '9' && !argv[i + 1][1])
            volume = argv[++i][0] - '0';
        else if (argv[i][0] != '-' && ndisks < 2)
            disk_name[ndisks++] = argv[i];
        else {
            printf("usage: apple2e [-d folder] [-n] [-m] [-v 0-9] [disk1 [disk2]]\n");
            return;
        }
    }
    if (!shell_drive_mounted()) {
        printf("No USB drive. The ROMs go in %s on it.\n", dir);
        return;
    }

    if (!sram_free(2 * sizeof(disk_file))) {
        printf("Not enough memory.\n");
        return;
    }
    uint8_t *rom = psram_malloc(0x4000);
    disk_file *df = calloc(2, sizeof *df);
    uint8_t p5[256];
    bool have_p5;
    bool ok = rom && df;
    if (!ok)
        printf("Not enough memory.\n");
    ok = ok && find_roms(dir, rom, p5, &have_p5);
    for (int d = 0; ok && d < ndisks; ++d)
        ok = load_disk(&df[d], disk_name[d], dir);
    if (ok && ndisks && !have_p5) {
        printf("No Disk II boot ROM (P5, 341-0027, 256 bytes) in %s.\n", dir);
        ok = false;
    }
    if (!ok) {
        for (int d = 0; df && d < 2; ++d)
            if (df[d].image)
                psram_free(df[d].image);
        free(df);
        psram_free(rom);
        return;
    }

    // Take the screen: the console stops drawing and the scanout doubles
    // lines, which frees the framebuffer's second half for the Apple.
    console_write("\x1b[?25l", 6);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    dvi_double_top = SHOWN_TOP;
    dvi_double_rows = SHOWN_ROWS;
    dvi_line_double = true;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);

    a2e *m = (a2e *)(SPARE + MACHINE_AT);
    m->main = SPARE;
    m->aux = SPARE + 0x10000;
    m->lc1_main = SPARE + 0x20000;
    m->lc1_aux = SPARE + 0x21000;
    for (int i = 0; i < 4; ++i)
        m->paddle[i] = 127;
    m->speaker = speaker;
    a2e_init(m, rom);
    psram_free(rom);

    a2e_disk2 *disk = (a2e_disk2 *)(SPARE + DISK_AT);
    bool have_card = have_p5;
    if (have_card) {
        a2e_disk2_install(m, disk, 6, p5);
        for (int d = 0; d < ndisks; ++d)
            a2e_disk2_insert(disk, d, df[d].image, ends_with(df[d].path, ".po"), df[d].read_only);
    }
    // With drive 1 empty the IIe would wait for a disk at power-on, so its
    // boot ROM stays hidden until it has gone on to Applesoft.
    int hide_boot_frames = have_card && !df[0].image ? 60 : 0;
    if (hide_boot_frames)
        m->slot_has_rom &= (uint8_t)~(1u << 6);

    static a2e_mockingboard mockingboard APP_OV_DATA(apple2);
    if (with_mockingboard) {
        a2e_mockingboard_install(m, &mockingboard, 4);
        mockingboard.ay_write = mockingboard_write;
        audio_ay_start(A2E_CPU_HZ);
        audio_ay_gain(volume_gain[volume]);
    }

    static a2e_screen screen APP_OV_DATA(apple2);
    screen = (a2e_screen){dvi_framebuf, 0, true, {0}};
    show_status(status_help, C_GREY);
    message_until = 0;
    if (have_card)
        draw_text(440, STATUS_Y, fast_disk ? "FAST" : "    ", C_GREY);
    bool lights[2] = {true, true};
    draw_lights(disk, have_card, lights);
    keyq_head = keyq_tail = 0;
    while (getchar_timeout_us(0) >= 0) {
    }
    bool caps_was = usb_keyboard_caps_lock();
    usb_keyboard_set_caps_lock(true);

    // Power on: one frame at a time, paced to real time.
    sched_us = time_us_64();
    sched_cycle = 0;
    uint64_t target = 0, frames = 0, busy_us = 0, worst_us = 0, late = 0;
    uint64_t fast_cycles = 0, fast_us = 0, volume_shown_until = 0;
    read_buttons(m);
    a2e_reset(m);
    for (;;) {
        uint64_t start = time_us_64();
        int act = read_keys();
        if (act == ACT_EXIT)
            break;
        read_buttons(m);
        if (act == ACT_RESET)
            a2e_reset(m);
        if (act == ACT_DISKS && have_card) {
            static int drive;
            char name[NAME_MAX_LEN];
            bool was_empty = !df[0].image;
            int r = pick_disk(dir, df, &drive, name);
            bool ok = r == PICK_CANCEL ||
                      change_disk(disk, df, drive, r == PICK_EJECT ? NULL : name, dir);
            // A disk put in an empty drive 1 is started at once (nothing can
            // be running from it), as B always does: a cold start, as at
            // power-on.
            if (ok && (r == PICK_BOOT || (r == PICK_INSERT && drive == 0 && was_empty))) {
                hide_boot_frames = 0;
                m->slot_has_rom |= 1u << 6;
                a2e_cold_start(m);
            }
            if (!ok)
                show_message(disk_error, frames);
            a2e_video_invalidate(&screen);
            lights[0] = lights[1] = true;
            sched_us = time_us_64();
            sched_cycle = target;
            continue;
        }
        if (hide_boot_frames && !--hide_boot_frames)
            m->slot_has_rom |= 1u << 6;
        if ((act == ACT_QUIETER || act == ACT_LOUDER) && with_mockingboard) {
            if (act == ACT_QUIETER && volume > 0)
                --volume;
            if (act == ACT_LOUDER && volume < 9)
                ++volume;
            audio_ay_gain(volume_gain[volume]);
            char text[8];
            snprintf(text, sizeof text, "VOL %d", volume);
            draw_text(470, STATUS_Y, text, C_GREY);
            volume_shown_until = frames + 120;  // two seconds
        }
        if (message_until && frames >= message_until) {
            show_status(status_help, C_GREY);
            message_until = 0;
        }
        if (volume_shown_until && frames >= volume_shown_until) {
            draw_text(470, STATUS_Y, "     ", C_GREY);
            volume_shown_until = 0;
        }
        if (act == ACT_FAST && have_card) {
            fast_disk = !fast_disk;
            draw_text(440, STATUS_Y, fast_disk ? "FAST" : "    ", C_GREY);
        }
        if (!m->strobe && keyq_head != keyq_tail) {
            a2e_key(m, keyq[keyq_tail]);
            keyq_tail = (keyq_tail + 1) % sizeof keyq;
        }

        if (fast_disk && have_card && disk->motor) {
            // Fast disk: frames back to back for one 60 Hz tick, then draw
            // and look at the keys; the schedule starts again from now.
            uint64_t c0 = m->cycles, until = start + FRAME_US_X1000 / 1000;
            accelerating = true;
            do {
                target += A2E_FRAME_CYCLES;
                if (m->cycles < target)
                    a2e_run(m, (uint32_t)(target - m->cycles));
            } while (disk->motor && time_us_64() < until);
            accelerating = false;
            a2e_video_update(m, &screen);
            draw_lights(disk, have_card, lights);
            uint64_t now = time_us_64();
            fast_cycles += m->cycles - c0;
            fast_us += now - start;
            sched_us = now;
            sched_cycle = target;
            continue;
        }

        target += A2E_FRAME_CYCLES;
        if (m->cycles < target)
            a2e_run(m, (uint32_t)(target - m->cycles));
        a2e_video_update(m, &screen);
        draw_lights(disk, have_card, lights);

        uint64_t now = time_us_64(), used = now - start;
        busy_us += used;
        if (used > worst_us)
            worst_us = used;
        ++frames;
        uint64_t due = cycle_time(target);
        if (now > due + 100000) {
            // Far behind (a long drive access, say): start the schedule again.
            ++late;
            sched_us = now;
            sched_cycle = target;
            due = now;
        } else if (target - sched_cycle > 1000000000ull) {
            sched_us = cycle_time(target);
            sched_cycle = target;
        }
        if (due > now)
            sleep_until(from_us_since_boot(due));
    }

    // Save what changed on the disks while the card (in the framebuffer)
    // is still there, then give the screen back and say what happened.
    audio_ay_stop();
    usb_keyboard_set_caps_lock(caps_was);
    int saved[2] = {0, 0};  // 1 saved, -1 failed
    for (int d = 0; d < 2; ++d) {
        if (!df[d].image)
            continue;
        a2e_disk2_flush(disk, d);
        if (disk->drive[d].dirty)
            saved[d] = save_disk(&df[d]) ? 1 : -1;
        psram_free(df[d].image);
    }
    dvi_line_double = false;
    dvi_double_top = 0;
    dvi_double_rows = DVI_HEIGHT / 2;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    console_write("\x1b" "c", 2);
    for (int d = 0; d < 2; ++d) {
        if (saved[d] > 0)
            printf("Saved %s.\n", df[d].path);
        else if (saved[d] < 0)
            printf("%s: couldn't save the changes!\n", df[d].path);
    }
    if (frames)
        printf("Apple IIe: %llu frames; emulation took %llu%% of real time (worst frame %llu us of "
               "%llu)%s.\n",
               (unsigned long long)frames,
               (unsigned long long)(busy_us * 100000 / (frames * FRAME_US_X1000)),
               (unsigned long long)worst_us, (unsigned long long)(FRAME_US_X1000 / 1000),
               late ? ", fell behind sometimes" : "");
    if (fast_us)
        printf("Fast disk ran the machine at %llu.%llu times real speed.\n",
               (unsigned long long)(fast_cycles * 10000000ull / ((uint64_t)A2E_CPU_HZ * fast_us) / 10),
               (unsigned long long)(fast_cycles * 10000000ull / ((uint64_t)A2E_CPU_HZ * fast_us) % 10));
    free(df);
}
