/**
 * @file c64_host.c
 * @brief The C64 on a PC, for testing: boots the ROMs, types keys at it through the keyboard matrix, and prints the text screen at the end (and optionally saves the picture as a PPM).
 *
 *   c64 ROMDIR [-k keys] [-K keyfile] [-d frames] [-f frames] [-L file.prg]
 *              [-8 disk.d64|folder [-W] [-T]] [-c cart.crt] [-p out.ppm] [-w out.wav] [-t]
 *
 * ROMDIR holds the KERNAL (8 KB, a name with "kernal", "kernel" or 901227 in it), BASIC
 * (8 KB, "basic" or 901226) and character ROM (4 KB, "char" or 901225).
 *   -k keys   typed once the machine has run `delay` frames; C escapes work
 *             (\r or \n is Return, \w waits a second); letters of either case
 *             are the plain keys
 *   -K file   the same, from a file (a new line is Return; escapes work)
 *   -d n      frames before the first key (default 150, 3 seconds)
 *   -f n      frames to run after the last key (default 50)
 *   -L file   a PRG put into memory before the keys are typed (at the address
 *             in its first two bytes; at $0801 BASIC's pointers are set, as LOAD
 *             does, so RUN runs it)
 *   -8 path   drive 8: a .d64 image, or a folder whose .prg files LOAD finds
 *             (LOAD"$",8 lists either) and SAVE writes
 *   -W        write what SAVE put on the .d64 back to its file at the end
 *   -c file   a .crt cartridge plugged in
 *   -T        drive 8 is a true 1541 running its own ROM (from ROMDIR: 16 KB
 *             with "1541" in its name, or the 8 KB halves 325302-01 and
 *             901229-0x) with the -8 .d64 in it; the KERNAL talks to it on
 *             the serial bus
 *   -S name   after the keys, switch the C64 off and on and, once BASIC is
 *             up, start the file called name on drive 8 (as the board's menu
 *             does)
 *   -p file   write the picture as a 384x272 PPM when done
 *   -w file   write the SID's sound as a 48 kHz mono WAV
 *   -t        print the time the emulation took against real time (stderr)
 *   -F        fast boot: skip the KERNAL's RAM test
 *   -R        print how many frames it took to get to READY. (stderr)
 *   -v        print the CPU's registers and the I/O chips' state (stderr)
 *   -M a-b    print memory from a to b (hex, as the CPU sees it) when done
 *
 * Exit status 3: a ROM is missing.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "c64.h"
#include "c64_cart.h"
#include "c64_d64.h"
#include "c64_drive.h"
#include "c64_sid.h"

static uint8_t ram[0x10000], color[0x400], basic[0x2000], kernal[0x2000], charrom[0x1000];
static uint8_t frame[C64_SCREEN_W * C64_SCREEN_H];
static c64 m;

/// Drive 8: a .d64 image in memory, or a folder of .prg files.
static uint8_t *disk;
static size_t disk_size;
static const char *disk_dir;
static uint8_t load_buf[0x10002];

/// A file name as a 16-character C64 name: upper case, ".prg" dropped.
static bool prg_name(const char *file, uint8_t name[16]) {
    size_t n = strlen(file);
    if (n < 5 || strcasecmp(file + n - 4, ".prg") || n - 4 > 16)
        return false;
    memset(name, 0xa0, 16);
    for (size_t i = 0; i < n - 4; ++i)
        name[i] = (uint8_t)toupper((unsigned char)file[i]);
    return true;
}

static int folder_load(const uint8_t *typed, int len, uint8_t *buf) {
    DIR *d = opendir(disk_dir);
    if (!d)
        return C64_LOAD_NO_DEVICE;
    bool dir = len == 1 && typed[0] == '$';
    c64_listing l;
    static const uint8_t title[16] = "PC FOLDER\xa0\xa0\xa0\xa0\xa0\xa0\xa0", id[5] = "00 2A";
    if (dir)
        c64_listing_start(&l, buf, title, id);
    struct dirent *e;
    int n = C64_LOAD_NOT_FOUND;
    uint8_t name[16];
    while ((e = readdir(d))) {
        if (!prg_name(e->d_name, name))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", disk_dir, e->d_name);
        struct stat st;
        if (stat(path, &st) || st.st_size < 2 || st.st_size > 0x10002)
            continue;
        if (dir) {
            c64_listing_file(&l, (unsigned)(st.st_size + 253) / 254, name, "PRG");
        } else if (c64_name_match(typed, len, name)) {
            FILE *f = fopen(path, "rb");
            n = f ? (int)fread(buf, 1, (size_t)st.st_size, f) : C64_LOAD_NOT_FOUND;
            if (f)
                fclose(f);
            break;
        }
    }
    closedir(d);
    return dir ? c64_listing_end(&l, 664) : n;
}

static int load(void *user, unsigned device, const uint8_t *name, int len, uint8_t *buf) {
    if (device != 8 || (!disk && !disk_dir))
        return C64_LOAD_NO_DEVICE;
    if (disk_dir)
        return folder_load(name, len, buf);
    if (len == 1 && name[0] == '$')
        return c64_d64_directory(disk, disk_size, buf);
    int n = c64_d64_load(disk, disk_size, name, len, buf);
    return n < 0 ? C64_LOAD_NOT_FOUND : n;
}

/// The SID's sound, made as the board does it: a sample at a time, each
/// register write applied at its cycle.
static c64_sid sound;
static FILE *wav;
static uint64_t wav_samples;

static void sound_to(uint64_t cycle) {
    uint64_t want = cycle * 48000 / C64_CPU_HZ;
    while (wav_samples < want) {
        int16_t v = c64_sid_sample(&sound);
        fputc(v & 0xff, wav);
        fputc((v >> 8) & 0xff, wav);
        ++wav_samples;
    }
}

static void sid_write(void *user, uint8_t reg, uint8_t value, uint64_t cycle) {
    sound_to(cycle);
    c64_sid_write(&sound, reg, value);
}

static void wav_header(uint32_t samples) {
    uint32_t bytes = samples * 2;
    uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\xbb\0\0\0\x77\x01\0\x02\0\x10\0data____";
    uint32_t riff = bytes + 36;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
}

/// A C64 name as a file name: ".PRG" added, and the characters a PC folder
/// can't hold as "_".
static void file_name(const uint8_t *name, int len, char *out, size_t n) {
    if (len >= 1 && name[0] == '@')
        ++name, --len;
    if (len >= 2 && name[1] == ':')
        name += 2, len -= 2;
    size_t o = 0;
    for (int i = 0; i < len && i < 16 && o + 5 < n; ++i) {
        uint8_t c = name[i];
        out[o++] = (c < 0x20 || c >= 0x7f || strchr("/\\:*?\"<>|", c)) ? '_' : (char)c;
    }
    snprintf(out + o, n - o, ".PRG");
}

static bool disk_changed;

static int save(void *user, unsigned device, const uint8_t *name, int len, const uint8_t *data, int n) {
    if (device != 8 || (!disk && !disk_dir))
        return C64_LOAD_NO_DEVICE;
    if (disk) {
        if (c64_d64_save(disk, disk_size, name, len, data, n) < 0)
            return C64_SAVE_FAILED;
        disk_changed = true;
        return 0;
    }
    char file[64], path[1100];
    file_name(name, len, file, sizeof file);
    snprintf(path, sizeof path, "%s/%s", disk_dir, file);
    FILE *f = fopen(path, "wb");
    if (!f)
        return C64_SAVE_FAILED;
    bool ok = fwrite(data, 1, (size_t)n, f) == (size_t)n;
    return fclose(f) == 0 && ok ? 0 : C64_SAVE_FAILED;
}

static void usage(void) {
    fprintf(stderr, "usage: c64 ROMDIR [-k keys] [-K keyfile] [-d frames] [-f frames] "
                    "[-L file.prg] [-8 disk.d64|folder] [-p out.ppm] [-t]\n");
    exit(2);
}

static bool name_has(const char *name, const char *a, const char *b) {
    char low[256];
    size_t i;
    for (i = 0; name[i] && i < sizeof low - 1; ++i)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    return strstr(low, a) || strstr(low, b);
}

/// Finds a ROM of exactly `size` bytes whose name has a or b in it.
static bool load_rom(const char *dir, const char *a, const char *b, uint8_t *buf, size_t size) {
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (!name_has(e->d_name, a, b))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        if (fseek(f, 0, SEEK_END) == 0 && (size_t)ftell(f) == size) {
            rewind(f);
            found = fread(buf, 1, size, f) == size;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: can't read\n", path);
        exit(1);
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/// C escapes in -k text.
static char *unescape(const char *s) {
    char *out = malloc(strlen(s) + 1), *o = out;
    for (; *s; ++s) {
        if (*s != '\\' || !s[1]) {
            *o++ = *s;
            continue;
        }
        switch (*++s) {
        case 'r': case 'n': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'w': *o++ = 0x01; break;  // a second's wait
        case 'x': {
            unsigned v = 0;
            for (int i = 0; i < 2 && isxdigit((unsigned char)s[1]); ++i, ++s)
                v = v * 16 + (unsigned)(isdigit((unsigned char)s[1]) ? s[1] - '0' : tolower(s[1]) - 'a' + 10);
            *o++ = (char)v;
            break;
        }
        default: *o++ = *s; break;
        }
    }
    *o = 0;
    return out;
}

static void run_frames(int n) {
    for (int i = 0; i < n; ++i)
        c64_run_frame(&m);
}

/// Holds the key down for 3 frames and lets it go for 3: the KERNAL scans
/// the keyboard 60 times a second.
static void type(const char *keys) {
    for (; *keys; ++keys) {
        unsigned k;
        bool shift;
        if (*keys == 0x01) {
            run_frames(50);
            continue;
        }
        if (!c64_ascii_key(*keys, &k, &shift)) {
            fprintf(stderr, "no key for '%c' (0x%02x)\n", *keys, (unsigned char)*keys);
            continue;
        }
        if (shift)
            c64_key(&m, C64K_LSHIFT, true);
        c64_key(&m, k, true);
        run_frames(3);
        c64_key(&m, k, false);
        c64_key(&m, C64K_LSHIFT, false);
        run_frames(3);
    }
}

static void load_prg(const char *path) {
    size_t n;
    uint8_t *p = (uint8_t *)read_file(path, &n);
    if (n < 2) {
        fprintf(stderr, "%s: too short\n", path);
        exit(1);
    }
    uint16_t at = p[0] | p[1] << 8;
    size_t len = n - 2;
    if (at + len > 0x10000)
        len = 0x10000 - at;
    memcpy(ram + at, p + 2, len);
    if (at == 0x0801) {
        uint16_t end = (uint16_t)(at + len);
        for (int z = 0x2d; z <= 0x31; z += 2) {  // VARTAB, ARYTAB, STREND
            ram[z] = end & 0xff;
            ram[z + 1] = end >> 8;
        }
    }
    free(p);
}

static void print_screen(void) {
    uint16_t base = m.vic.bank | (uint16_t)(m.vic.reg[0x18] & 0xf0) << 6;
    for (int row = 0; row < 25; ++row) {
        char line[41];
        for (int col = 0; col < 40; ++col)
            line[col] = c64_screen_ascii(ram[(uint16_t)(base + row * 40 + col)]);
        int n = 40;
        while (n > 0 && line[n - 1] == ' ')
            --n;
        line[n] = 0;
        puts(line);
    }
}

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", C64_SCREEN_W, C64_SCREEN_H);
    for (int i = 0; i < C64_SCREEN_W * C64_SCREEN_H; ++i) {
        uint32_t c = c64_palette[frame[i] & 15];
        fputc((int)(c >> 16), f);
        fputc((int)(c >> 8) & 0xff, f);
        fputc((int)c & 0xff, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == '-')
        usage();
    const char *dir = argv[1], *ppm = NULL, *prg = NULL, *switch_to = NULL;
    char *keys = NULL;
    int delay = 150, after = 50;
    bool timing = false, verbose = false, write_back = false, true_drive = false, fast_boot = false,
         ready_frames = false;
    const char *disk_path = NULL;
    static c64_cart cart;
    bool have_cart = false;
    unsigned dump_from = 1, dump_to = 0;
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "-k") && more) {
            keys = unescape(argv[++i]);
        } else if (!strcmp(a, "-K") && more) {
            size_t n;
            char *raw = read_file(argv[++i], &n);
            keys = unescape(raw);  // \w and the rest work in files too
            free(raw);
        } else if (!strcmp(a, "-d") && more) {
            delay = atoi(argv[++i]);
        } else if (!strcmp(a, "-f") && more) {
            after = atoi(argv[++i]);
        } else if (!strcmp(a, "-L") && more) {
            prg = argv[++i];
        } else if (!strcmp(a, "-c") && more) {
            size_t n;
            uint8_t *data = (uint8_t *)read_file(argv[++i], &n);
            int type;
            int r = c64_cart_parse(&cart, data, n, &type);
            if (r) {
                fprintf(stderr, "%s: %s\n", argv[i],
                        r == C64_CART_NOT_CRT ? "not a .crt cartridge" : "a cartridge type this doesn't do");
                return 1;
            }
            have_cart = true;
        } else if (!strcmp(a, "-T")) {
            true_drive = true;
        } else if (!strcmp(a, "-W")) {
            write_back = true;
        } else if (!strcmp(a, "-8") && more) {
            const char *path = disk_path = argv[++i];
            struct stat st;
            if (!stat(path, &st) && S_ISDIR(st.st_mode)) {
                disk_dir = path;
            } else {
                disk = (uint8_t *)read_file(path, &disk_size);
                if (!c64_d64_size_ok(disk_size)) {
                    fprintf(stderr, "%s: not a .d64 image\n", path);
                    return 1;
                }
            }
        } else if (!strcmp(a, "-S") && more) {
            switch_to = argv[++i];
        } else if (!strcmp(a, "-p") && more) {
            ppm = argv[++i];
        } else if (!strcmp(a, "-w") && more) {
            if (!(wav = fopen(argv[++i], "wb"))) {
                perror(argv[i]);
                return 1;
            }
            wav_header(0);
        } else if (!strcmp(a, "-F")) {
            fast_boot = true;
        } else if (!strcmp(a, "-R")) {
            ready_frames = true;
        } else if (!strcmp(a, "-t")) {
            timing = true;
        } else if (!strcmp(a, "-v")) {
            verbose = true;
        } else if (!strcmp(a, "-M") && more) {
            if (sscanf(argv[++i], "%x-%x", &dump_from, &dump_to) != 2)
                usage();
        } else {
            usage();
        }
    }

    bool ok = true;
    if (!load_rom(dir, "kern", "901227", kernal, sizeof kernal)) {
        fprintf(stderr, "%s: no KERNAL ROM (8 KB, \"kernal\", \"kernel\" or 901227 in its name)\n", dir);
        ok = false;
    }
    if (!load_rom(dir, "basic", "901226", basic, sizeof basic)) {
        fprintf(stderr, "%s: no BASIC ROM (8 KB, \"basic\" or 901226 in its name)\n", dir);
        ok = false;
    }
    if (!load_rom(dir, "char", "901225", charrom, sizeof charrom)) {
        fprintf(stderr, "%s: no character ROM (4 KB, \"char\" or 901225 in its name)\n", dir);
        ok = false;
    }
    if (!ok)
        return 3;

    // RAM powers up in stripes of $00 and $FF, as on most C64s.
    for (int i = 0; i < 0x10000; ++i)
        ram[i] = (i & 0x40) ? 0xff : 0x00;
    static c64_drive drive;
    static uint8_t drive_ram[0x800], drive_rom[0x4000], drive_gcr[C64_DRIVE_TRACKS * C64_GCR_TRACK_MAX];
    if (true_drive) {
        if (!load_rom(dir, "1541", "dos", drive_rom, 0x4000) &&
            !(load_rom(dir, "325302", "c000", drive_rom, 0x2000) &&
              load_rom(dir, "901229", "e000", drive_rom + 0x2000, 0x2000))) {
            fprintf(stderr, "%s: no 1541 ROM (16 KB, or 325302-01 and 901229-0x)\n", dir);
            return 3;
        }
        if (!disk) {
            fprintf(stderr, "-T: give the drive a .d64 with -8\n");
            return 1;
        }
    }
    m.ram = ram, m.color = color, m.basic = basic, m.kernal = kernal, m.charrom = charrom;
    m.vic.frame = frame;
    m.load = load;
    m.save = save;
    m.load_buf = load_buf;
    c64_sid_init(&sound, C64_CPU_HZ, 48000);
    if (wav)
        m.sid_write = sid_write;
    c64_init(&m);
    if (true_drive) {
        drive.ram = drive_ram;
        drive.rom = drive_rom;
        drive.gcr = drive_gcr;
        c64_drive_init(&drive, &m.cia2);
        c64_drive_insert(&drive, disk, disk_size, false);
        c64_attach_drive(&m, &drive);
    }
    if (have_cart) {
        c64_cart_attach(&m, &cart);
        c64_power_on(&m);
    }

    m.fast_boot = fast_boot;
    clock_t t0 = clock();
    if (ready_frames) {
        int f = 0;
        while (!c64_ready(&m) && f < 1000) {
            c64_run_frame(&m);
            ++f;
        }
        fprintf(stderr, "READY. after %d frames (%.2f s)\n", f, f * (double)C64_FRAME_CYCLES / C64_CPU_HZ);
    }
    run_frames(delay);
    if (prg)
        load_prg(prg);
    if (keys)
        type(keys);
    if (switch_to) {
        c64_power_on(&m);
        int f = 0;
        while (!c64_ready(&m) && f++ < 500)
            c64_run_frame(&m);
        if (!c64_ready(&m))
            fprintf(stderr, "-S: BASIC didn't come up after switching on\n");
        int n = load(NULL, 8, (const uint8_t *)switch_to, (int)strlen(switch_to), load_buf);
        if (n >= 3)
            c64_start_prg(&m, load_buf, n);
        else
            fprintf(stderr, "-S: no %s on drive 8\n", switch_to);
    }
    run_frames(after);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (wav) {
        sound_to(m.cycles);
        wav_header((uint32_t)wav_samples);
        fclose(wav);
    }

    if (true_drive && c64_drive_flush(&drive))
        disk_changed = true;
    if (disk && disk_changed && write_back) {
        FILE *f = fopen(disk_path, "wb");
        if (!f || fwrite(disk, 1, disk_size, f) != disk_size || fclose(f)) {
            perror(disk_path);
            return 1;
        }
    }
    print_screen();
    if (ppm)
        write_ppm(ppm);
    if (verbose) {
        c64_cpu *c = &m.cpu;
        fprintf(stderr, "pc %04X a %02X x %02X y %02X s %02X p %02X%s; port %02X/%02X\n", c->pc, c->a, c->x,
                c->y, c->s, c->p, c->jammed ? " JAMMED" : "", m.port_dir, m.port_data);
        for (int i = 0; i < 2; ++i) {
            c64_cia *x = i ? &m.cia2 : &m.cia1;
            fprintf(stderr, "cia%d pra %02X prb %02X ddr %02X/%02X ta %04X/%04X tb %04X/%04X cr %02X/%02X icr %02X/%02X\n",
                    i + 1, x->pra, x->prb, x->ddra, x->ddrb, x->ta, x->ta_latch, x->tb, x->tb_latch, x->cra,
                    x->crb, x->icr_mask, x->icr_data);
        }
        fprintf(stderr, "vic d011 %02X d016 %02X d018 %02X d019 %02X d01a %02X raster cmp %02X bank %04X\n",
                m.vic.reg[0x11], m.vic.reg[0x16], m.vic.reg[0x18], m.vic.irq_flags, m.vic.reg[0x1a],
                m.vic.reg[0x12], m.vic.bank);
        fprintf(stderr, "sid");
        for (int i = 0; i < 0x19; ++i)
            fprintf(stderr, " %02X", m.sid[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "code from pc-16:");
        for (int i = -16; i < 16; ++i)
            fprintf(stderr, " %02X", c64_peek(&m, (uint16_t)(c->pc + i)));
        fprintf(stderr, "\n");
    }
    for (unsigned a = dump_from; a <= dump_to && a < 0x10000; a += 16) {
        fprintf(stderr, "%04X:", a);
        for (unsigned i = 0; i < 16 && a + i <= dump_to; ++i)
            fprintf(stderr, " %02X", c64_peek(&m, (uint16_t)(a + i)));
        fprintf(stderr, "\n");
    }
    if (timing) {
        double real = (double)m.cycles / C64_CPU_HZ;
        fprintf(stderr, "%.2f s of C64 time in %.3f s (%.0fx real time)%s\n", real, secs, real / secs,
                m.cpu.jammed ? "; the CPU jammed" : "");
    }
    return 0;
}
