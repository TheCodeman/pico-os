/**
 * @file c64_pico.c
 * @brief The c64 command on the Pico 2 mini OS.
 *
 * The C64's 64 KB of RAM, its colour RAM and its ROMs live in the second
 * half of the framebuffer, which is free while the scanout shows each row
 * twice (640x240); the machine runs a PAL frame (19656 cycles) at a time on
 * core 0, paced to 50 Hz, and each raster line is drawn straight into the
 * framebuffer as the VIC-II finishes it.
 *
 * The picture is the 320x200 screen doubled both ways (640x400) with 20
 * lines of border above and below; the side borders are off the edge.
 *
 * The USB keyboard is the C64's: the keys held make up its matrix every
 * frame. Letters and digits are where the C64 has them; the other symbols
 * type what the PC key says (Shift-2 is @, Shift-; is :), arrows, Home, End
 * (CLR), Insert, Backspace and Delete are the cursor and editing keys, Esc
 * is RUN/STOP, Tab CTRL, Ctrl or Win the C= key, F1-F8 the function keys.
 * F12 is RESTORE, F11 switches it off and on, F10 goes back to the shell,
 * F9 is the menu (disks, programs, joystick port). The keypad's 8 4 6 2 (and 7 9 1 3) with
 * 0 or 5 for fire are a joystick, as is a USB joystick or gamepad; both are
 * in port 2 unless -j 1 (F1 in the menu switches). A second gamepad is the
 * joystick in the other port. Without a USB keyboard the serial console types,
 * a key at a time.
 *
 * A .crt cartridge (c64_cart.c: normal, Ultimax, Ocean, System 3, Magic
 * Desk, EasyFlash) plugs in from the menu or the command line; F11 and the
 * menu's power-on keep it in, F3 in the menu takes it out.
 *
 * Drive 8 is answered at once by default; F4 in the menu makes it a true
 * 1541 (its ROM in the folder), for programs with their own loaders.
 *
 * Drive 8 is a .d64 image (in PSRAM) or, with none in it, the
 * folder: LOAD"$",8 lists it and LOAD"NAME",8 loads NAME.PRG. The KERNAL's
 * LOAD is answered at once (c64.c), so nothing needs a 1541.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_app.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "as_font.h"
#include "audio.h"
#include "app_overlay.h"
#include "c64.h"
#include "c64_cart.h"
#include "c64_d64.h"
#include "c64_drive.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "serial_console.h"
#include "shell.h"
#include "tusb.h"
#include "usb_keyboard.h"

#define DEFAULT_DIR "/C64"
#define PATH_LEN    320

/// The framebuffer's second half: 150 KB, for the C64's memory and ROMs.
#define PIC_ROWS   240
#define SPARE      (dvi_framebuf + DVI_WIDTH * PIC_ROWS)
#define SPARE_SIZE (DVI_WIDTH * (DVI_HEIGHT - PIC_ROWS))
#define RAM_AT     0
#define COLOR_AT   0x10000
#define KERNAL_AT  0x10400
#define BASIC_AT   0x12400
#define CHAR_AT    0x14400
#define ROMS_SIZE  0x5000  ///< KERNAL, BASIC and the character ROM, in that order
#define D1541_AT     0x15400  ///< the true 1541: its state,
#define D1541_RAM_AT 0x16400  ///< its 2 KB of RAM
#define D1541_ROM_AT 0x16c00  ///< and its 16 KB ROM
_Static_assert(sizeof(c64_drive) <= D1541_RAM_AT - D1541_AT, "no room for the 1541");
_Static_assert(D1541_ROM_AT + 0x4000 <= SPARE_SIZE, "no room for the C64");

/// The VIC rows shown: 240 of the 272, normally raster lines 31-270 (200
/// lines of screen, 20 of border above and below; window_row), panned up or
/// down when a game draws further out (see pan); and the 320 pixels of the
/// screen at x 24-343.
#define FIRST_ROW (31 - C64_FIRST_LINE)
#define FIRST_COL (24 - C64_FIRST_X)

#define FRAME_US_X1000 ((uint64_t)C64_FRAME_CYCLES * 1000000000ull / C64_CPU_HZ)  ///< 19951 us

/// The SDK's malloc panics when SRAM runs out, so check first (as BASIC does).
static bool sram_free(size_t n) {
    extern char __StackLimit;  // top of the heap, from the SDK's linker script
    struct mallinfo mi = mallinfo();
    return (size_t)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks >= n + 8192;
}

// ---- Files ---------------------------------------------------------------------

static bool name_has(const char *name, const char *a, const char *b) {
    char low[256];
    size_t i;
    for (i = 0; name[i] && i < sizeof low - 1; ++i)
        low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    return strstr(low, a) || (b && strstr(low, b));
}

static bool ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && !strcasecmp(s + n - e, ext);
}

static bool read_all(const char *path, void *buf, UINT n) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    UINT got = 0;
    bool ok = f_read(&f, buf, n, &got) == FR_OK && got == n;
    f_close(&f);
    return ok;
}

static void path_join(char *out, size_t n, const char *dir, const char *name);
extern volatile uint32_t c64_rom_entries, c64_menu_ms;

/// The ROMs, in PSRAM: the KERNAL, BASIC and character ROMs (ROMS_SIZE
/// bytes, in that order) and the 1541's 16 KB after them. Read from the
/// folder once and kept for the next time c64 runs from the same one.
static struct {
    uint8_t *data;  ///< NULL: not read yet
    char dir[64];
    bool have_1541;
} roms;

/// Reads the ROMs from dir in one look through it, stopping once it has them
/// all: the KERNAL (8 KB, "kernal", "kernel" or 901227 in its name), BASIC
/// (8 KB, "basic" or 901226), the character ROM (4 KB, "char" or 901225),
/// and the 1541's (16 KB with "1541" or "dos" in its name, or the 8 KB
/// halves 325302-01 for $C000 and 901229-0x for $E000), which may be missing.
/// (A big folder takes long: see get_roms.)
static bool read_roms(const char *dir, uint8_t *buf, bool *have_1541) {
    static const struct {
        const char *a, *b, *what;
        uint32_t size, at;
    } want[6] = {
        {"kern", "901227", "KERNAL (8 KB, e.g. kernal.bin)", 0x2000, 0},
        {"basic", "901226", "BASIC (8 KB, e.g. basic.bin)", 0x2000, 0x2000},
        {"char", "901225", "character ROM (4 KB, e.g. char.bin)", 0x1000, 0x4000},
        {"1541", "dos", NULL, 0x4000, ROMS_SIZE},
        {"325302", "c000", NULL, 0x2000, ROMS_SIZE},
        {"901229", "e000", NULL, 0x2000, ROMS_SIZE + 0x2000},
    };
    bool have[6] = {false};
    DIR d;
    FILINFO fi;
    char path[PATH_LEN];
    if (f_opendir(&d, dir) != FR_OK) {
        printf("No folder %s on the drive.\n", dir);
        return false;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        ++c64_rom_entries;
        if (fi.fattrib & AM_DIR)
            continue;
        for (int i = 0; i < 6; ++i) {
            if (have[i] || fi.fsize != want[i].size || !name_has(fi.fname, want[i].a, want[i].b))
                continue;
            path_join(path, sizeof path, dir, fi.fname);
            have[i] = read_all(path, buf + want[i].at, want[i].size);
            break;
        }
        if (have[0] && have[1] && have[2] && (have[3] || (have[4] && have[5])))
            break;  // all of them: no need to look further
    }
    f_closedir(&d);
    bool ok = true;
    for (int i = 0; i < 3; ++i)
        if (!have[i]) {
            printf("No %s in %s.\n", want[i].what, dir);
            ok = false;
        }
    *have_1541 = have[3] || (have[4] && have[5]);
    return ok;
}

/// The ROMs found in a folder are also kept in one file in the drive's
/// root: finding them means reading through the folder, and with a game
/// collection in it that takes long (2152 entries: 13 s on the board).
/// The file holds this header, the ROMs and the folder they came from.
#define ROM_CACHE "/C64ROMS.BIN"

typedef struct {
    char magic[8];  ///< "C64ROMS1"
    uint32_t have_1541;
    char dir[64];
} rom_cache_head;

static bool write_file(const char *path, const void *data, UINT n);
static void list_add(const char *dir, const char *name);

static bool load_rom_cache(const char *dir) {
    FIL f;
    rom_cache_head h;
    UINT got = 0;
    if (f_open(&f, ROM_CACHE, FA_READ) != FR_OK)
        return false;
    bool ok = f_read(&f, &h, sizeof h, &got) == FR_OK && got == sizeof h && !memcmp(h.magic, "C64ROMS1", 8) &&
              !strncmp(h.dir, dir, sizeof h.dir) && f_read(&f, roms.data, ROMS_SIZE + 0x4000, &got) == FR_OK &&
              got == ROMS_SIZE + 0x4000;
    f_close(&f);
    if (ok)
        roms.have_1541 = h.have_1541;
    return ok;
}

static void save_rom_cache(const char *dir) {
    uint8_t *buf = psram_buffer_alloc(sizeof(rom_cache_head) + ROMS_SIZE + 0x4000);
    if (!buf)
        return;
    rom_cache_head h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "C64ROMS1", 8);
    h.have_1541 = roms.have_1541;
    snprintf(h.dir, sizeof h.dir, "%s", dir);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, roms.data, ROMS_SIZE + 0x4000);
    write_file(ROM_CACHE, buf, sizeof h + ROMS_SIZE + 0x4000);
    psram_buffer_free(buf);
}

/// The ROMs: kept in PSRAM from the last run, else from the drive's
/// /C64ROMS.BIN, else read from the folder (and /C64ROMS.BIN written);
/// reread (c64 -r) goes to the folder. False, with a message, if the
/// C64's aren't there.
static bool get_roms(const char *dir, bool reread) {
    if (roms.data && !reread && !strcmp(roms.dir, dir))
        return true;
    if (!roms.data && !(roms.data = psram_buffer_alloc(ROMS_SIZE + 0x4000))) {
        printf("Not enough memory.\n");
        return false;
    }
    roms.dir[0] = 0;
    if (reread || !load_rom_cache(dir)) {
        if (!read_roms(dir, roms.data, &roms.have_1541))
            return false;
        save_rom_cache(dir);
    }
    snprintf(roms.dir, sizeof roms.dir, "%s", dir);
    return true;
}

/// dir/name in out (empty if it doesn't fit).
static void path_join(char *out, size_t n, const char *dir, const char *name) {
    size_t d = strlen(dir), k = strlen(name);
    bool slash = d && dir[d - 1] == '/';
    if (d + !slash + k + 1 > n) {
        out[0] = 0;
        return;
    }
    memcpy(out, dir, d);
    if (!slash)
        out[d++] = '/';
    memcpy(out + d, name, k + 1);
}

/// Finds name as given, then in dir; the path found in path.
static bool find_file(const char *name, const char *dir, char *path, size_t n, FILINFO *fi) {
    snprintf(path, n, "%s", name);
    if (f_stat(path, fi) == FR_OK)
        return true;
    path_join(path, n, dir, name);
    return path[0] && f_stat(path, fi) == FR_OK;
}

/// Reads a whole file into PSRAM (NULL, with a message, if it can't).
static uint8_t *read_to_psram(const char *path, FSIZE_t size) {
    uint8_t *buf = psram_buffer_alloc((size_t)size);
    if (!buf) {
        printf("%s: no room for it\n", path);
        return NULL;
    }
    if (!read_all(path, buf, (UINT)size)) {
        printf("%s: can't read it\n", path);
        psram_buffer_free(buf);
        return NULL;
    }
    return buf;
}

// ---- Drive 8 -----------------------------------------------------------------------

static struct {
    const char *dir;
    uint8_t *image;  ///< a .d64 in PSRAM, or NULL: the folder
    size_t size;
    char name[64];
    char path[PATH_LEN];  ///< the .d64's file, where SAVE writes it back
} drive APP_OV_DATA(c64);  ///< (the apps' SRAM area, while c64 runs)

/// The last folder read, kept (in PSRAM) while c64 runs so the menu opens at
/// once the next time, and in /C64LIST.BIN for after a reset (see
/// cached_list); F5 reads it again.
static struct {
    struct menu_entry *list;
    int n;
    char *dir;  ///< PATH_LEN bytes after the list
} cache;

static void d1541_save(void);
static void d1541_insert(void);

static void eject(void) {
    d1541_save();  // what the true 1541 wrote, first
    if (drive.image)
        psram_buffer_free(drive.image);
    drive.image = NULL;
    drive.name[0] = 0;
    d1541_insert();
}

static bool insert(const char *name, bool quiet) {
    FILINFO fi;
    char path[PATH_LEN];
    if (!find_file(name, drive.dir, path, sizeof path, &fi)) {
        if (!quiet)
            printf("%s: not found (looked here and in %s)\n", name, drive.dir);
        return false;
    }
    if (!c64_d64_size_ok((size_t)fi.fsize)) {
        if (!quiet)
            printf("%s: not a .d64 image\n", path);
        return false;
    }
    uint8_t *img = read_to_psram(path, fi.fsize);
    if (!img)
        return false;
    eject();
    drive.image = img;
    drive.size = (size_t)fi.fsize;
    const char *base = strrchr(path, '/');
    snprintf(drive.name, sizeof drive.name, "%.63s", base ? base + 1 : path);
    memcpy(drive.path, path, sizeof drive.path);
    d1541_insert();
    return true;
}

/// A file name as a 16-character C64 name: upper case, ".prg" dropped.
static bool prg_name(const char *file, uint8_t name[16]) {
    size_t n = strlen(file);
    if (n < 5 || !ends_with(file, ".prg") || n - 4 > 16)
        return false;
    memset(name, 0xa0, 16);
    for (size_t i = 0; i < n - 4; ++i)
        name[i] = (uint8_t)(file[i] >= 'a' && file[i] <= 'z' ? file[i] - 32 : file[i]);
    return true;
}

/// The folder as a disk: its .prg files.
static int folder_load(const uint8_t *typed, int len, uint8_t *buf) {
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, drive.dir) != FR_OK)
        return C64_LOAD_NO_DEVICE;
    bool dir = len == 1 && typed[0] == '$';
    c64_listing l;
    uint8_t title[16], id[5] = {'P', 'I', 0xa0, '2', 'A'};
    memset(title, 0xa0, sizeof title);
    const char *t = drive.dir + (drive.dir[0] == '/');
    for (int i = 0; i < 16 && t[i]; ++i)
        title[i] = (uint8_t)(t[i] >= 'a' && t[i] <= 'z' ? t[i] - 32 : t[i]);
    if (dir)
        c64_listing_start(&l, buf, title, id);
    int n = C64_LOAD_NOT_FOUND;
    uint8_t name[16];
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & AM_DIR) || fi.fsize < 2 || fi.fsize > 0x10002 || !prg_name(fi.fname, name))
            continue;
        if (dir) {
            c64_listing_file(&l, (unsigned)(fi.fsize + 253) / 254, name, "PRG");
        } else if (c64_name_match(typed, len, name)) {
            char path[PATH_LEN];
            snprintf(path, sizeof path, "%s/%s", drive.dir, fi.fname);
            n = read_all(path, buf, (UINT)fi.fsize) ? (int)fi.fsize : C64_LOAD_NOT_FOUND;
            break;
        }
    }
    f_closedir(&d);
    return dir ? c64_listing_end(&l, 664) : n;
}

static int drive_load(void *user, unsigned device, const uint8_t *name, int len, uint8_t *buf) {
    if (device != 8)
        return C64_LOAD_NO_DEVICE;
    if (!drive.image)
        return folder_load(name, len, buf);
    if (len == 1 && name[0] == '$')
        return c64_d64_directory(drive.image, drive.size, buf);
    int n = c64_d64_load(drive.image, drive.size, name, len, buf);
    return n < 0 ? C64_LOAD_NOT_FOUND : n;
}

/// Writes a file whole and safely: to path.tmp first, then in place of path.
static bool write_file(const char *path, const void *data, UINT n) {
    char tmp[PATH_LEN + 4];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT put = 0;
    bool ok = f_write(&f, data, n, &put) == FR_OK && put == n;
    ok = f_close(&f) == FR_OK && ok;
    if (ok) {
        f_unlink(path);
        ok = f_rename(tmp, path) == FR_OK;
    }
    if (!ok)
        f_unlink(tmp);
    return ok;
}

// ---- The true 1541 -------------------------------------------------------------------

/// Off unless F4 in the menu turns it on (and its ROM is in the folder):
/// then drive 8 is a 1541 running its own DOS on the serial bus, for
/// programs with their own loaders, and the disk in it is GCR in PSRAM,
/// written back to the .d64 file each time the motor stops after a write.
static struct {
    c64_drive *d;    ///< NULL: off (the KERNAL's LOAD and SAVE answered at once)
    uint8_t *gcr;    ///< PSRAM
    bool have_rom;
} d1541;


/// What the drive wrote goes into the .d64, and the .d64 to its file.
static void d1541_save(void) {
    if (d1541.d && c64_drive_flush(d1541.d) && drive.image)
        write_file(drive.path, drive.image, (UINT)drive.size);
}

static void d1541_insert(void) {
    if (d1541.d)
        c64_drive_insert(d1541.d, drive.image, drive.image ? drive.size : 0, false);
}

static bool d1541_on(c64 *m) {
    if (d1541.d || !d1541.have_rom)
        return d1541.d != NULL;
    if (!(d1541.gcr = psram_buffer_alloc(C64_DRIVE_TRACKS * C64_GCR_TRACK_MAX)))
        return false;
    c64_drive *d = (c64_drive *)(SPARE + D1541_AT);
    memset(d, 0, sizeof *d);
    d->ram = SPARE + D1541_RAM_AT;
    d->rom = SPARE + D1541_ROM_AT;
    d->gcr = d1541.gcr;
    c64_drive_init(d, &m->cia2);
    d1541.d = d;
    d1541_insert();
    c64_attach_drive(m, d);
    return true;
}

static void d1541_off(c64 *m) {
    if (!d1541.d)
        return;
    d1541_save();
    c64_attach_drive(m, NULL);
    psram_buffer_free(d1541.gcr);
    d1541.d = NULL;
    d1541.gcr = NULL;
}

/// Once a frame: the LED in the top border, and the disk saved once the
/// motor stops after a write.
static void d1541_frame(void) {
    if (!d1541.d)
        return;
    if (d1541.d->changed && !c64_drive_motor(d1541.d))
        d1541_save();
    uint8_t led = c64_drive_led(d1541.d) ? RGB332(7, 0, 0) : RGB332(1, 0, 0);
    for (int y = 6; y < 10; ++y)
        memset(&dvi_framebuf[y * DVI_WIDTH + 600], led, 16);
}

// ---- Cartridges ------------------------------------------------------------------

/// The cartridge plugged in: its state and its whole .crt file, in PSRAM.
static struct {
    c64_cart *cart;  ///< NULL: none
    char name[64];
} slot;

static void unplug(c64 *m) {
    c64_cart_attach(m, NULL);
    if (slot.cart)
        psram_buffer_free(slot.cart);
    slot.cart = NULL;
    slot.name[0] = 0;
}

/// Loads a .crt and plugs it in (the caller switches off and on after).
static bool plug(c64 *m, const char *name, bool quiet) {
    FILINFO fi;
    char path[PATH_LEN];
    if (!find_file(name, drive.dir, path, sizeof path, &fi) || fi.fsize < 0x40 || fi.fsize > 0x200000) {
        if (!quiet)
            printf("%s: not found, or not a .crt (looked here and in %s)\n", name, drive.dir);
        return false;
    }
    uint8_t *buf = psram_buffer_alloc(sizeof(c64_cart) + (size_t)fi.fsize);
    if (!buf) {
        if (!quiet)
            printf("%s: no room for it\n", path);
        return false;
    }
    c64_cart *c = (c64_cart *)buf;
    uint8_t *data = buf + sizeof(c64_cart);
    int type = 0, r = -1;
    if (read_all(path, data, (UINT)fi.fsize))
        r = c64_cart_parse(c, data, (size_t)fi.fsize, &type);
    if (r) {
        if (!quiet)
            printf(r == C64_CART_UNSUPPORTED ? "%s: cartridge type %d isn't one this does\n"
                                             : "%s: not a .crt cartridge\n",
                   path, type);
        psram_buffer_free(buf);
        return false;
    }
    if (m)
        unplug(m);
    slot.cart = c;
    const char *base = strrchr(path, '/');
    snprintf(slot.name, sizeof slot.name, "%.63s", base ? base + 1 : path);
    if (m)
        c64_cart_attach(m, c);
    return true;
}

/// SAVE on drive 8: into the .d64, written back to the drive at once, or as
/// NAME.PRG in the folder (the characters a file name can't hold as _).
static int drive_save(void *user, unsigned device, const uint8_t *name, int len, const uint8_t *data, int n) {
    if (device != 8)
        return C64_LOAD_NO_DEVICE;
    if (drive.image) {
        if (c64_d64_save(drive.image, drive.size, name, len, data, n) < 0)
            return C64_SAVE_FAILED;
        return write_file(drive.path, drive.image, (UINT)drive.size) ? 0 : C64_SAVE_FAILED;
    }
    if (len >= 1 && name[0] == '@')
        ++name, --len;
    if (len >= 2 && name[1] == ':')
        name += 2, len -= 2;
    char file[24], path[PATH_LEN];
    int o = 0;
    for (int i = 0; i < len && i < 16; ++i) {
        uint8_t c = name[i];
        file[o++] = (c < 0x20 || c >= 0x7f || strchr("/\\:*?\"<>|", c)) ? '_' : (char)c;
    }
    if (!o)
        return C64_SAVE_FAILED;
    memcpy(file + o, ".PRG", 5);
    path_join(path, sizeof path, drive.dir, file);
    if (!path[0] || !write_file(path, data, (UINT)n))
        return C64_SAVE_FAILED;
    list_add(drive.dir, file);  // the menu shows it from now on
    return 0;
}

/// What to start once BASIC is up after a power-on: a .prg file, or the
/// first file on the disk in drive 8.
static struct {
    enum { START_NONE, START_PRG, START_DISK } what;
    char name[PATH_LEN];
    uint64_t give_up;  ///< the frame after which it no longer waits
} pending APP_OV_DATA(c64);

static void start_when_ready(int what, const char *name, uint64_t frame) {
    pending.what = what;
    size_t n = name ? strlen(name) : 0;
    if (n >= sizeof pending.name)
        n = 0;  // too long to find again
    memcpy(pending.name, name ? name : "", n);
    pending.name[n] = 0;
    pending.give_up = frame + 500;  // 10 seconds
}

/// Once a frame: starts what is pending once BASIC says READY.
static void start_pending(c64 *m, uint64_t frame) {
    if (pending.what == START_NONE)
        return;
    if (frame > pending.give_up) {
        pending.what = START_NONE;
        return;
    }
    if (!c64_ready(m))
        return;
    int n = -1;
    if (pending.what == START_DISK && drive.image) {
        n = c64_d64_load(drive.image, drive.size, (const uint8_t *)"*", 1, m->load_buf);
    } else if (pending.what == START_PRG) {
        FILINFO fi;
        char path[PATH_LEN];
        if (find_file(pending.name, drive.dir, path, sizeof path, &fi) && fi.fsize >= 3 && fi.fsize <= 0x10002 &&
            read_all(path, m->load_buf, (UINT)fi.fsize))
            n = (int)fi.fsize;
    }
    if (n >= 3)
        c64_start_prg(m, m->load_buf, n);
    pending.what = START_NONE;
}

// ---- Sound ---------------------------------------------------------------------

/// Cycle counts to time_us_64() times, on the schedule the frames keep.
static uint64_t sched_us, sched_cycle;

/// The SID's register writes go to core 1 with the time they were made,
/// where they are played 40 ms later, as exactly spaced as they came.
static void sid_write(void *user, uint8_t reg, uint8_t value, uint64_t cycle) {
    uint64_t when = cycle >= sched_cycle ? sched_us + (cycle - sched_cycle) * 1000000ull / C64_CPU_HZ
                                         : time_us_64();
    audio_sid_write(reg, value, when);
}

// ---- The picture -------------------------------------------------------------------

static uint16_t pal16[16];  ///< each colour as two RGB332 pixels
static uint8_t pal8[16];    ///< and as one

static void make_palette(void) {
    for (int i = 0; i < 16; ++i) {
        uint32_t c = c64_palette[i];
        unsigned r = ((c >> 16) * 7 + 127) / 255, g = (((c >> 8) & 0xff) * 7 + 127) / 255,
                 b = ((c & 0xff) * 3 + 127) / 255;
        uint8_t p = RGB332(r, g, b);
        pal8[i] = p;
        pal16[i] = (uint16_t)(p | p << 8);
    }
}

/// The window: the VIC row (raster line - 16) shown at the top. Every row of
/// the 272 is drawn, and the ones with something on them (not all one
/// colour) are noted for pan.
static int window_row = FIRST_ROW, window_rows = PIC_ROWS;  ///< more than 240: some left out
static int busy_top, busy_bottom;  ///< this frame's first and last such rows
/// With more than 240 rows: the framebuffer row for each VIC row (-1: left
/// out), and which rows were all one colour last frame (those go first).
static int16_t squeeze_map[C64_SCREEN_H] APP_OV_DATA(c64);
static uint32_t plain[(C64_SCREEN_H + 31) / 32];

/// The framebuffer row for a VIC row, or -1 if it isn't shown. With more
/// than 240 rows in the window some share a framebuffer row: the later wins.
static inline int __attribute__((section("app_ov_c64.take_row"))) take_row(int row, const uint8_t *pix) {
    bool squeezed = window_rows != PIC_ROWS;
    if (squeezed || row < busy_top || row > busy_bottom) {
        bool one_colour = !memcmp(pix, pix + 1, C64_SCREEN_W - 1);
        if (!one_colour) {
            if (row < busy_top)
                busy_top = row;
            if (row > busy_bottom)
                busy_bottom = row;
        }
        if (one_colour)
            plain[row >> 5] |= 1u << (row & 31);
        else
            plain[row >> 5] &= ~(1u << (row & 31));
    }
    int r = row - window_row;
    if (r < 0 || r >= window_rows)
        return -1;
    return squeezed ? squeeze_map[row] : r;
}

/// Which rows of a window taller than 240 are left out: plain ones (all one
/// colour last frame) first, spread out, then others, evenly.
static void make_squeeze_map(void) {
    int drop = window_rows - PIC_ROWS, plains = 0;
    static bool out[C64_SCREEN_H] APP_OV_DATA(c64);
    for (int r = window_row; r < window_row + window_rows; ++r) {
        out[r] = false;
        plains += (plain[r >> 5] >> (r & 31)) & 1;
    }
    int from_plain = plains < drop ? plains : drop, k = 0;
    for (int r = window_row, seen = 0; r < window_row + window_rows && k < from_plain; ++r)
        if ((plain[r >> 5] >> (r & 31)) & 1) {
            // the (k+1)-th of from_plain picks, spread over the plain rows
            if (seen++ == k * plains / from_plain) {
                out[r] = true;
                ++k;
            }
        }
    int rest = drop - k, kept = window_rows - k;
    for (int r = window_row, j = 0, n = 0; r < window_row + window_rows && n < rest; ++r)
        if (!out[r] && j++ == n * kept / rest) {
            out[r] = true;
            ++n;
        }
    for (int r = 0, fb = 0; r < C64_SCREEN_H; ++r)
        squeeze_map[r] = (r >= window_row && r < window_row + window_rows && !out[r] && fb < PIC_ROWS) ? fb++ : -1;
}

static void __attribute__((section("app_ov_c64.put_line"))) put_line(void *ctx, int row, const uint8_t *pix) {
    int fb = take_row(row, pix);
    if (fb < 0)
        return;
    uint32_t *out = (uint32_t *)(dvi_framebuf + fb * DVI_WIDTH);
    pix += FIRST_COL;
    for (int i = 0; i < 160; ++i, pix += 2)
        out[i] = pal16[pix[0]] | (uint32_t)pal16[pix[1]] << 16;
}

/// With the side borders: all 384 pixels across the 640, every 3 C64 pixels
/// as 5 (2, 2 and 1 wide). Slightly uneven, which is why the sharper 2x view
/// without them is the default.
static void __attribute__((section("app_ov_c64.put_line_wide"))) put_line_wide(void *ctx, int row, const uint8_t *pix) {
    int fb = take_row(row, pix);
    if (fb < 0)
        return;
    uint8_t *out = dvi_framebuf + fb * DVI_WIDTH;
    for (int i = 0; i < C64_SCREEN_W; i += 3, out += 5) {
        uint8_t a = pal8[pix[i]], b = pal8[pix[i + 1]], c = pal8[pix[i + 2]];
        out[0] = out[1] = a;
        out[2] = out[3] = b;
        out[4] = c;
    }
}

static bool wide;  ///< the side borders shown (F2 in the menu switches)
static bool want_1541;  ///< the true 1541 (F4 in the menu switches)

static void set_view(c64 *m) {
    m->vic.line_out = wide ? put_line_wide : put_line;
}

/// Once a frame: the window moves to show what a game draws above raster
/// line 31 or below 270 (an open border, a score panel there), and back to
/// the usual place once nothing has been drawn there for two seconds. When
/// what is drawn is taller than 240 lines (Frogger Arcade's, from its 1-UP
/// to its CREDIT line, is 260; 268 with the margins) the window takes all of it and some lines
/// are left out: lines all of one colour first (there are plenty in
/// Frogger's), else spread out evenly. Four lines are kept round what is drawn,
/// as some screens cut their edges. It changes only when the new window has
/// been wanted for 12 frames, so it doesn't jitter.
static void pan(void) {
    static int want_frames, idle_frames, squeeze_frames;
    int top = busy_top, bottom = busy_bottom, start = FIRST_ROW, rows = PIC_ROWS;
    busy_top = C64_SCREEN_H;
    busy_bottom = -1;
    if (top <= bottom) {  // a few lines' margin, for screens that cut the edges
        top = top >= 4 ? top - 4 : 0;
        bottom = bottom + 4 < C64_SCREEN_H ? bottom + 4 : C64_SCREEN_H - 1;
    }
    if (top <= bottom && (top < FIRST_ROW || bottom >= FIRST_ROW + PIC_ROWS)) {
        int lo = top < FIRST_ROW ? top : FIRST_ROW;
        int hi = bottom >= FIRST_ROW + PIC_ROWS ? bottom : FIRST_ROW + PIC_ROWS - 1;
        if (hi - lo + 1 <= PIC_ROWS) {
            start = top < FIRST_ROW ? lo : hi - PIC_ROWS + 1;
        } else {
            start = lo;
            rows = hi - lo + 1;
        }
    }
    if (start != FIRST_ROW || rows != PIC_ROWS) {
        idle_frames = 0;
        if ((start != window_row || rows != window_rows) && ++want_frames >= 12) {
            window_row = start;
            window_rows = rows;
            want_frames = 0;
            if (rows != PIC_ROWS) {
                make_squeeze_map();
                memset(dvi_framebuf, 0, DVI_WIDTH * PIC_ROWS);
            }
        } else if (rows != PIC_ROWS && rows == window_rows && start == window_row &&
                   ++squeeze_frames >= 25) {
            make_squeeze_map();  // what is plain changes as the game goes on
            squeeze_frames = 0;
        }
    } else {
        want_frames = 0;
        if ((window_row != FIRST_ROW || window_rows != PIC_ROWS) && ++idle_frames >= 100) {
            window_row = FIRST_ROW;
            window_rows = PIC_ROWS;
            idle_frames = 0;
        }
    }
}

static uint32_t clock_us(void) {
    return time_us_32();
}

/// Text in 6x8 cells (the 5x7 font with a column and a row of background).
static void draw_text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s && x + 6 <= DVI_WIDTH; ++s, x += 6) {
        uint8_t ch = (uint8_t)*s;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (ch < 0x20 || ch >= 0x80)
            ch = '?';
        const uint8_t *rows = as_font5x7[ch - 0x20];
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 6; ++c)
                dvi_framebuf[(y + r) * DVI_WIDTH + x + c] = r < 7 && c < 5 && (rows[r] & (0x10 >> c)) ? fg : bg;
    }
}

// ---- Keys --------------------------------------------------------------------------

static int ungetc_key = -1;  ///< a character read after a lone Esc

enum {
    K_UP = 0x100, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_INSERT, K_DELETE, K_PGUP, K_PGDN,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
};

/// A key as typed on the console: ASCII, or one of the above; -1 if none is
/// waiting.
/// While a USB keyboard drives the matrix, keys still come in on the serial
/// console too (taps, F9-F12): get_key then reads only that.
static bool serial_only;

static int read_char(uint32_t timeout_us) {
    if (!serial_only)
        return getchar_timeout_us(timeout_us);
    uint64_t until = time_us_64() + timeout_us;
    do {
        char c;
        if (serial_stdio_driver.in_chars(&c, 1) == 1)
            return (uint8_t)c;
    } while (time_us_64() < until);
    return -1;
}

static int get_key(void) {
    int c;
    if (ungetc_key >= 0) {
        c = ungetc_key;
        ungetc_key = -1;
        return c;
    }
    c = read_char(0);
    if (c != 0x1b)
        return c < 0 ? -1 : c;
    int c2 = read_char(2000);
    if (c2 != '[' && c2 != 'O') {
        if (c2 >= 0)
            ungetc_key = c2;
        return 0x1b;  // Esc on its own
    }
    int num = 0, fin;
    while ((fin = read_char(2000)) >= '0' && fin <= '9')
        num = num * 10 + (fin - '0');
    switch (fin) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'P': return K_F1;
    case 'Q': return K_F2;
    case 'R': return K_F3;
    case 'S': return K_F4;
    case '~':
        switch (num) {
        case 2: return K_INSERT;
        case 3: return K_DELETE;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        case 15: return K_F5;
        case 17: return K_F6;
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

static void drain_keys(void) {
    ungetc_key = -1;
    while (getchar_timeout_us(0) >= 0) {
    }
}

enum { ACT_NONE, ACT_RESET, ACT_EXIT, ACT_MENU, ACT_LOUDER, ACT_QUIETER };

/// A matrix key and whether Shift must be down (1), up (0) or as it is (-1).
typedef struct {
    uint8_t key;
    int8_t shift;
} c64key;

#define NO_KEY 0xff

/// The C64 key for a PC key that types a symbol: the C64 key with that symbol
/// on it (so Shift may change).
static c64key symbol_key(uint8_t hid, bool shifted) {
    static const uint8_t to_ascii[128][2] = {HID_KEYCODE_TO_ASCII};
    if (hid == HID_KEY_BACKSLASH || hid == HID_KEY_EUROPE_1)
        return (c64key){C64K_POUND, 0};
    if (hid == HID_KEY_GRAVE)
        return (c64key){C64K_LEFTARROW, 0};
    if (hid >= 128)
        return (c64key){NO_KEY, -1};
    unsigned key;
    bool shift;
    if (!c64_ascii_key((char)to_ascii[hid][shifted], &key, &shift))
        return (c64key){NO_KEY, -1};
    return (c64key){(uint8_t)key, shift};
}

/// The C64 key for a PC key, or NO_KEY.
static c64key pc_key(uint8_t hid, bool shifted) {
    static const uint8_t fkeys[4] = {C64K_F1, C64K_F3, C64K_F5, C64K_F7};
    unsigned key;
    bool shift;
    if (hid >= HID_KEY_A && hid <= HID_KEY_Z) {
        c64_ascii_key((char)('a' + hid - HID_KEY_A), &key, &shift);
        return (c64key){(uint8_t)key, -1};  // Shift as it is: letters or graphics
    }
    if (hid >= HID_KEY_1 && hid <= HID_KEY_0 && !shifted) {
        c64_ascii_key(hid == HID_KEY_0 ? '0' : (char)('1' + hid - HID_KEY_1), &key, &shift);
        return (c64key){(uint8_t)key, -1};
    }
    if (hid >= HID_KEY_F1 && hid <= HID_KEY_F8)
        return (c64key){fkeys[(hid - HID_KEY_F1) / 2], (hid - HID_KEY_F1) & 1};
    switch (hid) {
    case HID_KEY_ENTER: return (c64key){C64K_RETURN, -1};
    case HID_KEY_SPACE: return (c64key){C64K_SPACE, -1};
    case HID_KEY_BACKSPACE: case HID_KEY_DELETE: return (c64key){C64K_DEL, 0};
    case HID_KEY_INSERT: return (c64key){C64K_DEL, 1};
    case HID_KEY_HOME: return (c64key){C64K_HOME, -1};
    case HID_KEY_END: return (c64key){C64K_HOME, 1};
    case HID_KEY_ARROW_DOWN: return (c64key){C64K_CRSR_UD, 0};
    case HID_KEY_ARROW_UP: return (c64key){C64K_CRSR_UD, 1};
    case HID_KEY_ARROW_RIGHT: return (c64key){C64K_CRSR_LR, 0};
    case HID_KEY_ARROW_LEFT: return (c64key){C64K_CRSR_LR, 1};
    case HID_KEY_ESCAPE: return (c64key){C64K_RUNSTOP, -1};
    case HID_KEY_TAB: return (c64key){C64K_CTRL, -1};
    case HID_KEY_PAGE_UP: return (c64key){C64K_UPARROW, 0};
    }
    return symbol_key(hid, shifted);
}

/// The keypad as a joystick: up, down, left, right, fire.
static uint8_t keypad_joy(uint8_t hid) {
    switch (hid) {
    case HID_KEY_KEYPAD_8: return 0x01;
    case HID_KEY_KEYPAD_2: return 0x02;
    case HID_KEY_KEYPAD_4: return 0x04;
    case HID_KEY_KEYPAD_6: return 0x08;
    case HID_KEY_KEYPAD_7: return 0x05;
    case HID_KEY_KEYPAD_9: return 0x09;
    case HID_KEY_KEYPAD_1: return 0x06;
    case HID_KEY_KEYPAD_3: return 0x0a;
    case HID_KEY_KEYPAD_0: case HID_KEY_KEYPAD_5: case HID_KEY_KEYPAD_ENTER: return 0x10;
    }
    return 0;
}

static uint8_t prev_keys[6];
static uint8_t ignored[6];  ///< keys held when the menu closed, until let go

static bool was_held(uint8_t hid) {
    return memchr(prev_keys, hid, sizeof prev_keys) != NULL;
}

/// The USB keyboard as the C64's: the matrix from the keys held now; F9-F12
/// act when pressed. *joy gets the keypad's joystick.
static int usb_matrix(c64 *m, const uint8_t keys[6], uint8_t mods, uint8_t *joy) {
    int act = ACT_NONE;
    bool shifted = mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    int force_shift = -1;
    memset(m->keys, 0, sizeof m->keys);
    m->restore = false;
    *joy = 0;
    for (int i = 0; i < 6; ++i)
        if (ignored[i] && !memchr(keys, ignored[i], 6))
            ignored[i] = 0;
    for (int i = 0; i < 6; ++i) {
        uint8_t hid = keys[i];
        if (hid == HID_KEY_NONE || memchr(ignored, hid, sizeof ignored))
            continue;
        bool pressed = !was_held(hid);
        switch (hid) {
        case HID_KEY_F9: if (pressed) act = ACT_MENU; continue;
        case HID_KEY_F10: if (pressed) act = ACT_EXIT; continue;
        case HID_KEY_F11: if (pressed) act = ACT_RESET; continue;
        case HID_KEY_F12: m->restore = true; continue;
        case HID_KEY_KEYPAD_ADD: if (pressed) act = ACT_LOUDER; continue;
        case HID_KEY_KEYPAD_SUBTRACT: if (pressed) act = ACT_QUIETER; continue;
        }
        uint8_t j = keypad_joy(hid);
        if (j) {
            *joy |= j;
            continue;
        }
        c64key k = pc_key(hid, shifted);
        if (k.key == NO_KEY)
            continue;
        c64_key(m, k.key, true);
        if (k.shift >= 0)
            force_shift = k.shift;
    }
    memcpy(prev_keys, keys, sizeof prev_keys);
    if (force_shift == 1) {
        c64_key(m, C64K_LSHIFT, true);
    } else if (force_shift < 0) {
        c64_key(m, C64K_LSHIFT, mods & KEYBOARD_MODIFIER_LEFTSHIFT);
        c64_key(m, C64K_RSHIFT, mods & KEYBOARD_MODIFIER_RIGHTSHIFT);
    }
    if (mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL | KEYBOARD_MODIFIER_LEFTGUI |
                KEYBOARD_MODIFIER_RIGHTGUI))
        c64_key(m, C64K_CBM, true);
    return act;
}

/// Without a USB keyboard, the serial console types: each key is a tap,
/// pressed for two frames and let go for two.
typedef struct {
    uint8_t key;
    int8_t shift;  ///< 1: with Shift, 2: with CTRL
    bool restore;
} tap;

#define TAP_DOWN 2
#define TAP_UP   2
#define TAPQ_LEN 64

static tap tapq[TAPQ_LEN];
static unsigned tapq_head, tapq_tail;
static int tap_frames;  ///< >0: the tap at tapq_tail is down; <0: the gap after it

static void tap_put(uint8_t key, int shift, bool restore) {
    unsigned next = (tapq_head + 1) % TAPQ_LEN;
    if (next != tapq_tail) {
        tapq[tapq_head] = (tap){key, (int8_t)shift, restore};
        tapq_head = next;
    }
}

static int serial_keys(void) {
    static const uint8_t fkeys[4] = {C64K_F1, C64K_F3, C64K_F5, C64K_F7};
    int c;
    while ((c = get_key()) >= 0) {
        unsigned key;
        bool shift;
        switch (c) {
        case K_UP: tap_put(C64K_CRSR_UD, 1, false); break;
        case K_DOWN: tap_put(C64K_CRSR_UD, 0, false); break;
        case K_RIGHT: tap_put(C64K_CRSR_LR, 0, false); break;
        case K_LEFT: tap_put(C64K_CRSR_LR, 1, false); break;
        case K_HOME: tap_put(C64K_HOME, 0, false); break;
        case K_END: tap_put(C64K_HOME, 1, false); break;
        case K_INSERT: tap_put(C64K_DEL, 1, false); break;
        case K_DELETE: case 0x7f: case 0x08: tap_put(C64K_DEL, 0, false); break;
        case 0x1b: tap_put(C64K_RUNSTOP, 0, false); break;
        case '\\': tap_put(C64K_POUND, 0, false); break;
        case K_F1: case K_F2: case K_F3: case K_F4: case K_F5: case K_F6: case K_F7: case K_F8:
            tap_put(fkeys[(c - K_F1) / 2], (c - K_F1) & 1, false);
            break;
        case K_F9: return ACT_MENU;
        case K_F10: return ACT_EXIT;
        case K_F11: return ACT_RESET;
        case K_F12: tap_put(C64K_RUNSTOP, 0, true); break;  // RUN/STOP-RESTORE
        default:
            if (c >= 1 && c <= 26 && c != '\r' && c != '\t' && c != '\n') {
                c64_ascii_key((char)('a' + c - 1), &key, &shift);  // Ctrl and a letter
                tap_put((uint8_t)key, 2, false);
            } else if (c < 0x80 && c64_ascii_key((char)c, &key, &shift)) {
                tap_put((uint8_t)key, shift, false);
            }
            break;
        }
    }
    return ACT_NONE;
}

/// fresh: the matrix starts empty (else the tap joins the USB keyboard's keys).
static void tap_frame(c64 *m, bool fresh) {
    if (fresh) {
        memset(m->keys, 0, sizeof m->keys);
        m->restore = false;
    }
    if (tap_frames < 0) {
        ++tap_frames;
        return;
    }
    if (tapq_head == tapq_tail)
        return;
    const tap *t = &tapq[tapq_tail];
    if (tap_frames == 0)
        tap_frames = TAP_DOWN;
    c64_key(m, t->key, true);
    if (t->shift == 1)
        c64_key(m, C64K_LSHIFT, true);
    if (t->shift == 2)
        c64_key(m, C64K_CTRL, true);
    m->restore |= t->restore;
    if (--tap_frames == 0) {
        tap_frames = -TAP_UP;
        tapq_tail = (tapq_tail + 1) % TAPQ_LEN;
    }
}

/// USB joystick or gamepad n (0 or 1): up, down, left, right, fire.
static uint8_t read_gamepad(unsigned n) {
    uint8_t x, y, j = 0;
    uint32_t buttons;
    if (!usb_gamepad_read_n(n, &x, &y, &buttons))
        return 0;
    if (y < 64) j |= 0x01;
    if (y > 191) j |= 0x02;
    if (x < 64) j |= 0x04;
    if (x > 191) j |= 0x08;
    if (buttons) j |= 0x10;
    return j;
}

// ---- The menu (F9) -------------------------------------------------------------------

#define MAX_FILES 4096  ///< in PSRAM: 4096 x 257 bytes, 1 MB
#define COL_CHARS 49
#define ROWS      23
#define COLS      2
#define PAGE      (ROWS * COLS)
#define C_BOX     RGB332(0, 0, 1)
#define C_TEXT    RGB332(7, 7, 3)
#define C_DIR     RGB332(3, 7, 3)
#define C_GREY    RGB332(3, 3, 1)

typedef struct menu_entry {
    char name[256];
    bool dir;
} entry;

/// The folder the menu shows (it remembers it): in PSRAM, made the first
/// time c64 runs (SRAM is for what's running).
static char *menu_dir;  ///< PATH_LEN bytes

static int entry_cmp(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;  // folders first, ".." first of all
    if (!strcmp(x->name, ".."))
        return -1;
    if (!strcmp(y->name, ".."))
        return 1;
    return strcasecmp(x->name, y->name);
}

/// The scan's progress over the picture: what has been read so far, and a
/// bar sweeping across (FAT doesn't say how many entries a folder has without
/// reading them all). Drawn at most every 40 ms.
static void scan_progress(const char *dir, int seen, int kept, const char *what, bool now) {
    static uint64_t last;
    uint64_t t = time_us_64();
    if (!now && t - last < 40000)
        return;
    last = t;
    char line[110];
    for (int y = 96; y < 146; ++y)
        memset(&dvi_framebuf[y * DVI_WIDTH + 110], C_BOX, 420);
    snprintf(line, sizeof line, "%s %.50s", what, dir);
    draw_text(122, 101, line, C_TEXT, C_BOX);
    snprintf(line, sizeof line, "%d ENTRIES READ, %d FOLDERS, DISKS AND PROGRAMS", seen, kept);
    draw_text(122, 113, line, C_GREY, C_BOX);
    int pos = (int)((t / 4000) % 456) - 60;  // a 60-pixel block across 396
    for (int y = 127; y < 137; ++y) {
        uint8_t *row = &dvi_framebuf[y * DVI_WIDTH + 122];
        memset(row, C_GREY, 396);
        for (int x = pos < 0 ? 0 : pos; x < pos + 60 && x < 396; ++x)
            row[x] = C_DIR;
    }
}

/// The folders and the .d64 and .prg files in dir, sorted, with the progress
/// on the screen while it reads.
static int list_files(const char *dir, entry *list, int max) {
    DIR d;
    FILINFO fi;
    int n = 0, seen = 0;
    scan_progress(dir, 0, 0, "SCANNING", true);
    if (strcmp(dir, "/") && strcmp(dir, "")) {
        snprintf(list[n].name, sizeof list[n].name, "..");
        list[n++].dir = true;
    }
    if (f_opendir(&d, dir) != FR_OK)
        return n;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        scan_progress(dir, ++seen, n, "SCANNING", false);
        if (fi.fattrib & (AM_HID | AM_SYS))
            continue;
        bool is_dir = fi.fattrib & AM_DIR;
        if (is_dir || (ends_with(fi.fname, ".d64") && c64_d64_size_ok((size_t)fi.fsize)) ||
            (ends_with(fi.fname, ".prg") && fi.fsize >= 3 && fi.fsize <= 0x10002) ||
            (ends_with(fi.fname, ".crt") && fi.fsize >= 0x40 && fi.fsize <= 0x200000)) {
            snprintf(list[n].name, sizeof list[n].name, "%s", fi.fname);
            list[n++].dir = is_dir;
        }
    }
    f_closedir(&d);
    scan_progress(dir, seen, n, "SORTING", true);
    qsort(list, (size_t)n, sizeof *list, entry_cmp);
    return n;
}


/// The last folder the menu read is also kept in /C64LIST.BIN in the
/// drive's root, as reading a folder with a game collection in it takes
/// long (13 s for 2152 entries): after a reset the menu opens from the file.
/// F5 reads the folder again (after copying games in from a PC, say); a
/// SAVE into the folder adds its file. The file: this header, then each
/// entry as a byte (1: a folder) and its name with a 0 after it.
#define LIST_CACHE "/C64LIST.BIN"

typedef struct {
    char magic[8];  ///< "C64LIST1"
    uint32_t n;
    char dir[PATH_LEN];
} list_cache_head;

static int load_list_cache(const char *dir, entry *list, int max) {
    FIL f;
    list_cache_head h;
    UINT got = 0;
    if (f_open(&f, LIST_CACHE, FA_READ) != FR_OK)
        return -1;
    int n = -1;
    if (f_read(&f, &h, sizeof h, &got) == FR_OK && got == sizeof h && !memcmp(h.magic, "C64LIST1", 8) &&
        !strncmp(h.dir, dir, PATH_LEN) && h.n <= (uint32_t)max) {
        size_t size = (size_t)f_size(&f) - sizeof h;
        char *buf = psram_buffer_alloc(size ? size : 1);
        if (buf && f_read(&f, buf, (UINT)size, &got) == FR_OK && got == size) {
            size_t at = 0;
            for (n = 0; n < (int)h.n && at < size; ++n) {
                list[n].dir = buf[at++];
                size_t len = strnlen(buf + at, size - at);
                if (len >= sizeof list[n].name || at + len >= size)
                    break;
                memcpy(list[n].name, buf + at, len + 1);
                at += len + 1;
            }
            if (n != (int)h.n)
                n = -1;  // cut short: read the folder instead
        }
        if (buf)
            psram_buffer_free(buf);
    }
    f_close(&f);
    return n;
}

static void save_list_cache(const char *dir, const entry *list, int n) {
    size_t size = sizeof(list_cache_head);
    for (int i = 0; i < n; ++i)
        size += 2 + strlen(list[i].name);
    uint8_t *buf = psram_buffer_alloc(size);
    if (!buf)
        return;
    list_cache_head *h = (list_cache_head *)buf;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "C64LIST1", 8);
    h->n = (uint32_t)n;
    snprintf(h->dir, PATH_LEN, "%s", dir);
    size_t at = sizeof *h;
    for (int i = 0; i < n; ++i) {
        size_t len = strlen(list[i].name);
        buf[at++] = list[i].dir;
        memcpy(buf + at, list[i].name, len + 1);
        at += len + 1;
    }
    write_file(LIST_CACHE, buf, (UINT)size);
    psram_buffer_free(buf);
}

static bool cache_ready(void) {
    if (!cache.list) {
        if (!(cache.list = psram_malloc(MAX_FILES * sizeof(entry) + PATH_LEN)))
            return false;
        cache.dir = (char *)(cache.list + MAX_FILES);
        cache.dir[0] = 0;
    }
    return true;
}

static int cached_list(const char *dir, bool reread) {
    if (!cache_ready())
        return 0;
    if (reread || strcmp(cache.dir, dir)) {
        int n = reread ? -1 : load_list_cache(dir, cache.list, MAX_FILES);
        if (n < 0) {
            n = list_files(dir, cache.list, MAX_FILES);
            save_list_cache(dir, cache.list, n);
        }
        cache.n = n;
        snprintf(cache.dir, PATH_LEN, "%s", dir);
    }
    return cache.n;
}

/// A file SAVE made in dir joins the list (in PSRAM and in the file), if
/// the list is of that folder.
static void list_add(const char *dir, const char *name) {
    if (!cache_ready() || strcmp(cache.dir, dir)) {
        FIL f;
        list_cache_head h;
        UINT got = 0;
        bool same = false;
        if (f_open(&f, LIST_CACHE, FA_READ) == FR_OK) {
            same = f_read(&f, &h, sizeof h, &got) == FR_OK && got == sizeof h && !strncmp(h.dir, dir, PATH_LEN);
            f_close(&f);
        }
        if (!same || !cache.list)
            return;  // the file is of another folder: nothing to change
        cached_list(dir, false);  // the folder's list, from the file
    }
    for (int i = 0; i < cache.n; ++i)
        if (!strcasecmp(cache.list[i].name, name))
            return;
    if (cache.n >= MAX_FILES)
        return;
    snprintf(cache.list[cache.n].name, sizeof cache.list[0].name, "%s", name);
    cache.list[cache.n++].dir = false;
    qsort(cache.list, (size_t)cache.n, sizeof *cache.list, entry_cmp);
    save_list_cache(dir, cache.list, cache.n);
}

/// A name in at most COL_CHARS - 1 characters: a long one keeps its start and
/// its end (with the extension), with a ~ between.
static void short_name(char *out, const entry *e) {
    char full[260];
    snprintf(full, sizeof full, "%s%s", e->name, e->dir && strcmp(e->name, "..") ? "/" : "");
    size_t n = strlen(full), max = COL_CHARS - 1;
    if (n <= max) {
        strcpy(out, full);
        return;
    }
    size_t tail = 14, head = max - tail - 1;
    memcpy(out, full, head);
    out[head] = '~';
    memcpy(out + head + 1, full + n - tail, tail + 1);
}

static int jump_to(const entry *list, int n, int sel, int c) {
    if (c >= 'A' && c <= 'Z')
        c += 32;
    for (int i = 1; i <= n; ++i) {
        int j = (sel + i) % n;
        int f = list[j].name[0];
        if (f >= 'A' && f <= 'Z')
            f += 32;
        if (f == c)
            return j;
    }
    return sel;
}

/// The list over the picture, the machine waiting meanwhile: folders (Return
/// goes in, ".." back up) and .d64 and .prg files, two columns of 23 a page.
/// Return on a file switches the C64 off and on and starts the .prg, or puts
/// the .d64 in drive 8 and starts its first file; Tab only puts the disk in;
/// Delete takes it out; F1 switches the joystick port; F2 the side borders
/// (all 384 pixels, or the sharper 2x view of the screen); F5 reads the folder
/// again; F11 switches off and on; a letter or digit jumps to the next name starting with it; Esc or F9
/// goes back.
static void menu(c64 *m, int *port, uint64_t frame) {
    drain_keys();
    if (!menu_dir[0])
        snprintf(menu_dir, PATH_LEN, "%s", drive.dir);
    static int sel;
    uint64_t t0 = time_us_64();
    int n = cached_list(menu_dir, false);
    c64_menu_ms = (uint32_t)((time_us_64() - t0) / 1000);
    entry *list = cache.list;
    if (!list)
        return;
    if (sel >= n)
        sel = 0;
    char line[160], name[COL_CHARS + 1], path[PATH_LEN];
    for (bool done = false; !done;) {
        for (int y = 2; y < 238; ++y)
            memset(&dvi_framebuf[y * DVI_WIDTH + 12], C_BOX, 616);
        snprintf(line, sizeof line, "%.30s (%d)  DRIVE 8: %.22s %s  CART: %.18s  JOY %d %s", menu_dir, n,
                 drive.image ? drive.name : "FOLDER", d1541.d ? "(1541)" : "(FAST)",
                 slot.cart ? slot.name : "NONE", *port, wide ? "BORDERS" : "2X");
        draw_text(20, 5, line, C_TEXT, C_BOX);
        if (n == 0 || (n == 1 && list[0].dir && !strcmp(list[0].name, "..")))
            draw_text(20, 20, "NO FOLDERS, .D64 OR .PRG FILES HERE", C_GREY, C_BOX);
        int top = sel / PAGE * PAGE;
        for (int i = 0; i < PAGE && top + i < n; ++i) {
            const entry *e = &list[top + i];
            bool on = top + i == sel;
            short_name(name, e);
            snprintf(line, sizeof line, " %-*s", COL_CHARS - 1, name);
            uint8_t fg = e->dir ? C_DIR : C_TEXT;
            draw_text(20 + (i / ROWS) * (COL_CHARS * 6 + 4), 17 + (i % ROWS) * 9, line, on ? C_BOX : fg,
                      on ? fg : C_BOX);
        }
        snprintf(line, sizeof line, "PAGE %d/%d", sel / PAGE + 1, (n + PAGE - 1) / PAGE > 0 ? (n + PAGE - 1) / PAGE : 1);
        draw_text(20, 226, line, C_GREY, C_BOX);
        draw_text(110, 226, d1541.have_rom ? "RET RUN TAB INSERT DEL EJECT F1 JOY F2 BORDERS F3 UNPLUG F4 1541 F5 REREAD"
                                    : "RET RUN TAB INSERT DEL EJECT F1 JOY F2 BORDERS F3 UNPLUG F5 REREAD (NO 1541 ROM)", C_GREY,
                  C_BOX);
        int k;
        while ((k = get_key()) < 0)
            sleep_ms(5);
        switch (k) {
        case K_UP: if (sel > 0) --sel; break;
        case K_DOWN: if (sel < n - 1) ++sel; break;
        case K_LEFT: sel = sel >= ROWS ? sel - ROWS : 0; break;
        case K_RIGHT: sel = sel + ROWS < n ? sel + ROWS : (n ? n - 1 : 0); break;
        case K_PGUP: sel = sel >= PAGE ? sel - PAGE : 0; break;
        case K_PGDN: sel = sel + PAGE < n ? sel + PAGE : (n ? n - 1 : 0); break;
        case K_HOME: sel = 0; break;
        case K_END: sel = n ? n - 1 : 0; break;
        case '\r':
            if (!n)
                break;
            if (list[sel].dir) {  // into a folder, or back up
                if (!strcmp(list[sel].name, "..")) {
                    char *slash = strrchr(menu_dir, '/');
                    if (slash && slash != menu_dir)
                        *slash = 0;
                    else
                        strcpy(menu_dir, "/");
                } else {
                    path_join(path, sizeof path, menu_dir, list[sel].name);
                    snprintf(menu_dir, PATH_LEN, "%s", path);
                }
                n = cached_list(menu_dir, false);
                sel = 0;
                break;
            }
            path_join(path, sizeof path, menu_dir, list[sel].name);
            if (ends_with(path, ".crt")) {  // plugged in, then a fresh machine
                if (!plug(m, path, true))
                    break;
                pending.what = START_NONE;
                c64_power_on(m);
                done = true;
                break;
            }
            if (ends_with(path, ".d64")) {  // a fresh machine, then the disk's first file
                if (!insert(path, true))
                    break;
                start_when_ready(START_DISK, NULL, frame);
            } else {
                start_when_ready(START_PRG, path, frame);
            }
            c64_power_on(m);
            done = true;
            break;
        case '\t':  // just put the disk in
            path_join(path, sizeof path, menu_dir, n ? list[sel].name : "");
            if (n && !list[sel].dir && ends_with(path, ".d64") && insert(path, true))
                done = true;
            break;
        case K_DELETE: case 0x7f: case 0x08: eject(); done = true; break;
        case K_F1: *port = 3 - *port; break;
        case K_F2: wide = !wide; set_view(m); break;
        case K_F3:  // the cartridge out, and a fresh machine
            if (slot.cart) {
                unplug(m);
                pending.what = START_NONE;
                c64_power_on(m);
                done = true;
            }
            break;
        case K_F5: n = cached_list(menu_dir, true); sel = 0; break;
        case K_F4:  // the true 1541 on or off
            want_1541 = !d1541.d;
            if (want_1541 && !d1541_on(m))
                want_1541 = false;
            else if (!want_1541)
                d1541_off(m);
            break;
        case K_F11: pending.what = START_NONE; c64_power_on(m); done = true; break;
        case 0x1b: case K_F9: done = true; break;
        default:
            if (n && k < 0x80 && ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')))
                sel = jump_to(list, n, sel, k);
            break;
        }
    }
    drain_keys();
    // Keys still held as the menu goes (Return, Esc) don't reach the C64.
    uint8_t mods;
    if (!usb_keyboard_held_keys(ignored, &mods))
        memset(ignored, 0, sizeof ignored);
}

// ---- Volume ---------------------------------------------------------------------

/// Keypad + and -: 0-9, 6 as the chip comes; the level shows in the top
/// border for two seconds.
static const unsigned volume_gain[10] = {0, 40, 70, 110, 160, 210, 256, 330, 420, 520};
static int volume = 6;
static uint64_t volume_shown_until;

static void change_volume(int step, uint64_t frame) {
    volume += step;
    volume = volume < 0 ? 0 : volume > 9 ? 9 : volume;
    audio_sid_gain(volume_gain[volume]);
    volume_shown_until = frame + 100;
}

/// After each frame (the VIC has just redrawn the border under it).
static void show_volume(uint64_t frame) {
    if (frame >= volume_shown_until)
        return;
    char text[16];
    snprintf(text, sizeof text, " VOLUME %d ", volume);
    draw_text(DVI_WIDTH / 2 - 30, 4, text, C_TEXT, C_BOX);
}

// ---- The command -------------------------------------------------------------------

/// How long starting took, in ms from the command: after the checks and
/// memory, the ROMs (and how many folder entries that read), the file given,
/// the screen taken, the power-on, the first frame and READY. Printed on the
/// console (the serial one too) when c64 starts and ends; c64_start_ms can be
/// read over SWD as well.
enum { T_CHECKS, T_ROMS, T_FILE, T_SCREEN, T_POWER, T_FRAME1, T_READY, T_PHASES };
volatile uint32_t c64_start_ms[T_PHASES], c64_rom_entries;
volatile uint32_t c64_menu_ms;  ///< how long F9 took to get its list, last time
static uint64_t start_us;

static void mark(int phase) {
    c64_start_ms[phase] = (uint32_t)((time_us_64() - start_us) / 1000);
}

static void print_start_times(void) {
    printf("c64 start (ms): checks %lu, ROMs %lu (%lu folder entries read%s), file %lu, screen %lu, "
           "power-on %lu, first frame %lu, READY. %lu\n",
           (unsigned long)c64_start_ms[T_CHECKS], (unsigned long)c64_start_ms[T_ROMS],
           (unsigned long)c64_rom_entries, c64_rom_entries ? "" : ", from the PSRAM copy",
           (unsigned long)c64_start_ms[T_FILE], (unsigned long)c64_start_ms[T_SCREEN],
           (unsigned long)c64_start_ms[T_POWER], (unsigned long)c64_start_ms[T_FRAME1],
           (unsigned long)c64_start_ms[T_READY]);
}

void c64_main(int argc, char **argv) {
    start_us = time_us_64();
    memset((void *)c64_start_ms, 0, sizeof c64_start_ms);
    c64_rom_entries = 0;
    const char *dir = DEFAULT_DIR, *file = NULL;
    int port = 2;
    bool reread = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-r"))
            reread = true;
        else if (!strcmp(argv[i], "-j") && i + 1 < argc && (!strcmp(argv[i + 1], "1") || !strcmp(argv[i + 1], "2")))
            port = argv[++i][0] - '0';
        else if (argv[i][0] != '-' && !file)
            file = argv[i];
        else {
            printf("usage: c64 [-d folder] [-j 1|2] [-r] [file.prg | disk.d64 | cart.crt]\n"
                   "  -r reads the ROMs from the folder again (after changing them)\n");
            return;
        }
    }
    if (!shell_drive_mounted()) {
        printf("No USB drive. The ROMs go in %s on it.\n", dir);
        return;
    }
    if (!sram_free(sizeof(c64))) {
        printf("Not enough memory.\n");
        return;
    }
    memset(&drive, 0, sizeof drive);
    memset(&slot, 0, sizeof slot);
    drive.dir = dir;
    if (!menu_dir && (menu_dir = psram_malloc(PATH_LEN)) != NULL)
        menu_dir[0] = 0;
    if (!menu_dir) {
        printf("Not enough memory.\n");
        return;
    }
    if (strcmp(menu_dir, dir) && strncmp(menu_dir, dir, strlen(dir)))
        menu_dir[0] = 0;  // another folder this time: the menu starts there
    c64 *m = calloc(1, sizeof *m);
    uint8_t *load_buf = psram_buffer_alloc(0x10002);
    bool ok = m && load_buf;
    if (!ok)
        printf("Not enough memory.\n");
    mark(T_CHECKS);
    ok = ok && get_roms(dir, reread);
    mark(T_ROMS);
    memset(&d1541, 0, sizeof d1541);
    d1541.have_rom = ok && roms.have_1541;
    memset(&pending, 0, sizeof pending);
    if (ok && file) {
        FILINFO fi;
        char path[PATH_LEN];
        if (ends_with(file, ".d64")) {
            ok = insert(file, false);
        } else if (ends_with(file, ".crt")) {
            ok = plug(NULL, file, false);
        } else if (!find_file(file, dir, path, sizeof path, &fi) || fi.fsize < 3 || fi.fsize > 0x10002) {
            printf("%s: not found (or not a PRG), looked here and in %s\n", file, dir);
            ok = false;
        }
        if (ok && !slot.cart)
            start_when_ready(drive.image ? START_DISK : START_PRG, file, 0);
    }
    mark(T_FILE);
    if (!ok) {
        eject();
        psram_buffer_free(load_buf);
        free(m);
        return;
    }
    print_start_times();

    // Take the screen: the console stops drawing and the scanout doubles
    // lines, which frees the framebuffer's second half for the C64.
    console_write("\x1b[?25l", 6);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    dvi_line_double = true;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    memcpy(SPARE + KERNAL_AT, roms.data, ROMS_SIZE);
    if (d1541.have_rom)
        memcpy(SPARE + D1541_ROM_AT, roms.data + ROMS_SIZE, 0x4000);

    mark(T_SCREEN);
    m->ram = SPARE + RAM_AT;
    m->color = SPARE + COLOR_AT;
    m->kernal = SPARE + KERNAL_AT;
    m->basic = SPARE + BASIC_AT;
    m->charrom = SPARE + CHAR_AT;
    set_view(m);
    m->vic.draw_from = 0;  // all of them, for pan to see
    m->vic.draw_to = C64_SCREEN_H;
    window_row = FIRST_ROW;
    window_rows = PIC_ROWS;
    busy_top = C64_SCREEN_H;
    busy_bottom = -1;
    m->load = drive_load;
    m->save = drive_save;
    m->load_buf = load_buf;
    m->sid_write = sid_write;
    m->time_us = clock_us;
    sched_us = time_us_64();
    sched_cycle = 0;
    audio_sid_start(C64_CPU_HZ);
    audio_sid_gain(volume_gain[volume]);
    volume_shown_until = 0;
    make_palette();
    m->fast_boot = true;  // no RAM test: READY. at once
    c64_init(m);
    if (slot.cart)
        c64_cart_attach(m, slot.cart);
    if (want_1541 && !d1541_on(m))
        want_1541 = false;
    c64_power_on(m);
    mark(T_POWER);

    tapq_head = tapq_tail = 0;
    tap_frames = 0;
    memset(prev_keys, 0, sizeof prev_keys);
    memset(ignored, 0, sizeof ignored);
    drain_keys();

    // One frame at a time, paced to real time.
    sched_us = time_us_64();
    sched_cycle = m->cycles;
    uint64_t target = m->cycles, frames = 0, busy_us = 0, worst_us = 0, late = 0;
    for (;;) {
        uint64_t start = time_us_64();
        uint8_t keys[6], mods, keypad = 0;
        int act;
        if (usb_keyboard_held_keys(keys, &mods)) {
            // The USB keyboard's characters: the matrix has those keys already.
            char junk[16];
            while (usb_keyboard_stdio_driver.in_chars(junk, sizeof junk) > 0) {
            }
            act = usb_matrix(m, keys, mods, &keypad);
            serial_only = true;
            int serial = serial_keys();
            serial_only = false;
            if (act == ACT_NONE)
                act = serial;
            tap_frame(m, false);
        } else {
            act = serial_keys();
            tap_frame(m, true);
        }
        if (act == ACT_EXIT)
            break;
        if (act == ACT_LOUDER || act == ACT_QUIETER)
            change_volume(act == ACT_LOUDER ? 1 : -1, frames);
        if (act == ACT_RESET) {  // F11: off and on
            pending.what = START_NONE;
            c64_power_on(m);
        }
        if (act == ACT_MENU) {
            menu(m, &port, frames);
            sched_us = time_us_64();
            sched_cycle = target;
            continue;
        }
        // The first gamepad (and the keypad) in the chosen port, the second
        // in the other.
        m->joy[port - 1] = read_gamepad(0) | keypad;
        m->joy[2 - port] = read_gamepad(1);
        start_pending(m, frames);

        // While the true 1541's motor runs (a load, which takes both CPUs),
        // every other frame isn't drawn.
        m->vic.no_draw = d1541.d && c64_drive_motor(d1541.d) && (frames & 1);
        target += C64_FRAME_CYCLES;
        c64_run_frame(m);
        if (!frames)
            mark(T_FRAME1);
        if (!c64_start_ms[T_READY] && c64_ready(m))
            mark(T_READY);
        if (!m->vic.no_draw)
            pan();
        show_volume(frames);
        d1541_frame();

        uint64_t now = time_us_64(), used = now - start;
        busy_us += used;
        if (used > worst_us)
            worst_us = used;
        ++frames;
        uint64_t due = sched_us + (target - sched_cycle) * 1000000ull / C64_CPU_HZ;
        if (now > due + 100000) {
            // Far behind (a disk read, say): start the schedule again.
            ++late;
            sched_us = now;
            sched_cycle = target;
            due = now;
        }
        if (due > now)
            sleep_until(from_us_since_boot(due));
    }

    unsigned sid_load = audio_sid_load();
    audio_sid_stop();

    // Give the screen back.
    d1541_off(m);
    eject();
    unplug(m);
    if (cache.list)
        psram_free(cache.list);
    memset(&cache, 0, sizeof cache);
    psram_buffer_free(load_buf);
    dvi_line_double = false;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    console_write("\x1b" "c", 2);
    drain_keys();
    if (frames)
        printf("C64: %llu frames; emulation took %llu%% of real time (worst frame %llu us of "
               "%llu)%s.\n",
               (unsigned long long)frames,
               (unsigned long long)(busy_us * 100000 / (frames * FRAME_US_X1000)),
               (unsigned long long)worst_us, (unsigned long long)(FRAME_US_X1000 / 1000),
               late ? ", fell behind sometimes" : "");
    print_start_times();
    if (busy_us)
        printf("Of that, drawing the picture took %llu%% and the CPU and the rest %llu%%.\n",
               (unsigned long long)(m->video_us * 100 / busy_us),
               (unsigned long long)(100 - m->video_us * 100 / busy_us));
    printf("SID sound took %u.%u%% of core 1.\n", sid_load / 10, sid_load % 10);
    free(m);
}
