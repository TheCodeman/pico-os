/**
 * @file a2e_host.c
 * @brief The Apple IIe machine on a PC, for testing: boots the ROM, types keys at it, and prints the text screen at the end (and optionally saves the display as a PPM picture).
 *
 *   apple2e ROM [-k keys] [-K keyfile] [-d delay] [-c cycles] [-s] [-p out.ppm]
 *               [-P p5.rom [-1 disk] [-2 disk] [-W]]
 *
 * ROM is the enhanced IIe's $C000-$FFFF image: 16 KB, or a 32 KB file whose
 * second half is that image (as APPLE2E.ROM is).
 *   -k keys   typed once the machine has run `delay` cycles; C escapes work
 *             (\r or \n is Return, \e Esc, \xNN any code)
 *   -K file   the same, from a file (a new line is Return)
 *   -d n      cycles before the first key (default 2000000)
 *   -c n      cycles to run after the last key (default 3000000)
 *   -C        Caps Lock down: typed letters reach the Apple as capitals, as
 *             on the board
 *   -g n      cycles to wait between keys, once the last has been read
 *             (default 20000)
 *   -m n      stop after this many cycles in all, typed or not (default 1000000000)
 *   -s        hold Open Apple and Solid Apple through power-on: the self test
 *   -p file   write the display as a 640x480 PPM picture when done
 *   -v        print the machine's switches to stderr when done
 *   -V        check the board's changed-lines renderer against the full one
 *             (640x480 and line-doubled 640x240) and print the result
 *   -P file   a Disk II card in slot 6 with this boot ROM (P5, 341-0027)
 *   -1/-2 f   140 KB disk images for drives 1 and 2: .po is ProDOS order,
 *             anything else DOS 3.3 order
 *   -W        save what was written to the disks back to their files
 *   -R        write-protect the disks
 *   -S n f    after n cycles, swap the disk in drive 1 for image f
 *   -B n f    the same, then a cold start (the board's F9 then B)
 *   -M        a Mockingboard in slot 4; at the end, print its sound chips'
 *             registers and the pitch of each one's channel A, measured from
 *             the samples the AY-3-8910 core makes
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2e.h"
#include "a2e_disk2.h"
#include "a2e_mockingboard.h"
#include "ay38910.h"

static uint64_t key_gap = 20000;  ///< cycles between typed keys, once the last is read

static void usage(void) {
    fprintf(stderr, "usage: apple2e ROM [-k keys] [-K keyfile] [-d delay] [-c cycles] [-s] "
                    "[-p out.ppm] [-v] [-P p5.rom [-1 disk] [-2 disk] [-W] [-R]]\n");
    exit(2);
}

static void *must_calloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static bool load_rom(const char *path, uint8_t rom[0x4000]) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    static uint8_t buf[0x8000];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0x4000)
        memcpy(rom, buf, 0x4000);
    else if (n == 0x8000)
        memcpy(rom, buf + 0x4000, 0x4000);
    else
        return false;
    return true;
}

/// Unescapes -k text in place; returns its length.
static size_t unescape(char *s) {
    char *o = s;
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            *o++ = '\r';
            continue;
        }
        if (*p != '\\' || !p[1]) {
            *o++ = *p;
            continue;
        }
        switch (*++p) {
        case 'r': case 'n': *o++ = '\r'; break;
        case 'e': *o++ = 0x1b; break;
        case 't': *o++ = '\t'; break;
        case 'x': {
            unsigned v = 0;
            int k = 0;
            while (k < 2 && p[1] && strchr("0123456789abcdefABCDEF", p[1])) {
                char h = *++p;
                v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                ++k;
            }
            *o++ = (char)v;
            break;
        }
        default: *o++ = *p; break;
        }
    }
    *o = 0;
    return (size_t)(o - s);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = must_calloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(s);
        return NULL;
    }
    fclose(f);
    // New lines are Return; drop CRs of CR LF files.
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (*p == '\r')
            continue;
        *o++ = *p == '\n' ? '\r' : *p;
    }
    *o = 0;
    return s;
}

static bool ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    if (n < e)
        return false;
    for (size_t i = 0; i < e; ++i)
        if ((s[n - e + i] | 0x20) != ext[i])
            return false;
    return true;
}

static uint8_t *load_disk(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *img = must_calloc(A2E_DISK_SIZE);
    size_t n = fread(img, 1, A2E_DISK_SIZE, f);
    fclose(f);
    if (n != A2E_DISK_SIZE) {
        free(img);
        return NULL;
    }
    return img;
}

static void save_disk(const char *path, const uint8_t *img) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img, 1, A2E_DISK_SIZE, f) != A2E_DISK_SIZE)
        perror(path);
    if (f)
        fclose(f);
}

static void write_ppm(const char *path, const uint8_t *fb) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", A2E_FB_WIDTH, A2E_FB_HEIGHT);
    for (int i = 0; i < A2E_FB_WIDTH * A2E_FB_HEIGHT; ++i) {
        uint8_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 5) * 255 / 7), (uint8_t)(((c >> 2) & 7) * 255 / 7),
                          (uint8_t)((c & 3) * 255 / 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2)
        usage();
    const char *rom_path = argv[1];
    char *keys = NULL;
    const char *ppm = NULL, *p5_path = NULL, *disk_path[2] = {NULL, NULL};
    bool write_back = false, read_only = false;
    uint64_t swap_at = 0;
    bool mockingboard = false;
    const char *swap_path = NULL;
    bool swap_boot = false;
    uint64_t delay = 2000000, after = 3000000, max_cycles = 1000000000;
    bool self_test = false, verbose = false, video_check = false, caps = false;
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-k") && i + 1 < argc) {
            keys = strdup(argv[++i]);
            unescape(keys);
        } else if (!strcmp(a, "-K") && i + 1 < argc) {
            if (!(keys = read_file(argv[++i]))) {
                perror(argv[i]);
                return 1;
            }
        } else if (!strcmp(a, "-d") && i + 1 < argc) {
            delay = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "-c") && i + 1 < argc) {
            after = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "-C")) {
            caps = true;
        } else if (!strcmp(a, "-g") && i + 1 < argc) {
            key_gap = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "-m") && i + 1 < argc) {
            max_cycles = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "-p") && i + 1 < argc) {
            ppm = argv[++i];
        } else if (!strcmp(a, "-s")) {
            self_test = true;
        } else if (!strcmp(a, "-P") && i + 1 < argc) {
            p5_path = argv[++i];
        } else if ((!strcmp(a, "-1") || !strcmp(a, "-2")) && i + 1 < argc) {
            disk_path[a[1] - '1'] = argv[++i];
        } else if (!strcmp(a, "-W")) {
            write_back = true;
        } else if ((!strcmp(a, "-S") || !strcmp(a, "-B")) && i + 2 < argc) {
            swap_boot = a[1] == 'B';
            swap_at = strtoull(argv[++i], NULL, 0);
            swap_path = argv[++i];
        } else if (!strcmp(a, "-M")) {
            mockingboard = true;
        } else if (!strcmp(a, "-R")) {
            read_only = true;
        } else if (!strcmp(a, "-V")) {
            video_check = true;
        } else if (!strcmp(a, "-v")) {
            verbose = true;
        } else {
            usage();
        }
    }

    uint8_t rom[0x4000];
    if (!load_rom(rom_path, rom)) {
        fprintf(stderr, "%s: can't read a 16 KB or 32 KB ROM image\n", rom_path);
        return 1;
    }

    a2e *m = must_calloc(sizeof *m);
    m->main = must_calloc(0x10000);
    m->aux = must_calloc(0x10000);
    m->lc1_main = must_calloc(0x1000);
    m->lc1_aux = must_calloc(0x1000);
    for (int i = 0; i < 4; ++i)
        m->paddle[i] = 127;
    m->button[0] = m->button[1] = self_test;
    a2e_init(m, rom);

    static a2e_disk2 disk;
    uint8_t *image[2] = {NULL, NULL};
    if (p5_path) {
        uint8_t p5[256];
        FILE *f = fopen(p5_path, "rb");
        if (!f || fread(p5, 1, 256, f) != 256 || !a2e_disk2_check_rom(p5)) {
            fprintf(stderr, "%s: not a Disk II boot ROM (P5, 341-0027)\n", p5_path);
            return 1;
        }
        fclose(f);
        a2e_disk2_install(m, &disk, 6, p5);
        for (int d = 0; d < 2; ++d) {
            if (!disk_path[d])
                continue;
            if (!(image[d] = load_disk(disk_path[d]))) {
                fprintf(stderr, "%s: can't read a 140 KB disk image\n", disk_path[d]);
                return 1;
            }
            a2e_disk2_insert(&disk, d, image[d], ends_with(disk_path[d], ".po"), read_only);
        }
    } else if (disk_path[0] || disk_path[1]) {
        fprintf(stderr, "disks need a Disk II card: give its ROM with -P\n");
        return 2;
    }

    static a2e_mockingboard mb;
    if (mockingboard)
        a2e_mockingboard_install(m, &mb, 4);

    // Power on, then type the keys one at a time: each waits until the one
    // before has been read (the strobe cleared) and a little longer.
    size_t nkeys = keys ? strlen(keys) : 0, next = 0;
    uint64_t last_key = 0, end = 0;
    bool done_typing = nkeys == 0;
    if (done_typing)
        end = delay + after;
    for (;;) {
        a2e_run(m, 1000);
        if (swap_path && m->cycles >= swap_at) {
            // As the board's F9 does: out with the old (saved if asked), in
            // with the new.
            a2e_disk2_flush(&disk, 0);
            if (write_back && image[0] && disk.drive[0].dirty)
                save_disk(disk_path[0], image[0]);
            a2e_disk2_insert(&disk, 0, NULL, false, false);
            free(image[0]);
            if (!(image[0] = load_disk(swap_path))) {
                fprintf(stderr, "%s: can't read a 140 KB disk image\n", swap_path);
                return 1;
            }
            disk_path[0] = swap_path;
            a2e_disk2_insert(&disk, 0, image[0], ends_with(swap_path, ".po"), read_only);
            swap_path = NULL;
            if (swap_boot)
                a2e_cold_start(m);
        }
        if (self_test && m->cycles > 200000)
            m->button[0] = m->button[1] = false;
        if (!done_typing && m->cycles >= delay && !m->strobe && m->cycles - last_key >= key_gap) {
            a2e_key(m, a2e_caps((uint8_t)keys[next++], caps));
            last_key = m->cycles;
            if (next == nkeys) {
                done_typing = true;
                end = m->cycles + after;
            }
        }
        if ((done_typing && m->cycles >= end) || m->cycles >= max_cycles) {
            if (!done_typing)
                fprintf(stderr, "stopped after %llu cycles with %zu keys not typed\n",
                        (unsigned long long)m->cycles, nkeys - next);
            break;
        }
    }

    for (int d = 0; d < 2; ++d) {
        if (!image[d])
            continue;
        a2e_disk2_flush(&disk, d);
        if (write_back && disk.drive[d].dirty)
            save_disk(disk_path[d], image[d]);
    }

    char text[24 * 81 + 1];
    a2e_text_dump(m, text);
    fputs(text, stdout);

    if (mockingboard) {
        for (int chip = 0; chip < 2; ++chip) {
            const uint8_t *r = mb.via[chip].ay_regs;
            printf("AY%d:", chip + 1);
            for (int i = 0; i < 16; ++i)
                printf(" %02X", r[i]);
            // A second of its sound: count the rising edges.
            static ay38910 ay;
            ay_init(&ay, 1023000, 48000);
            for (uint8_t i = 0; i < 16; ++i)
                ay_write(&ay, i, r[i]);
            ay_write(&ay, 7, (uint8_t)(r[7] | 0x3e));  // channel A's tone alone
            int edges = 0, prev = 0;
            for (int n = 0; n < 48000; ++n) {
                int v = ay_sample(&ay);
                edges += v > 0 && prev <= 0;
                prev = v;
            }
            printf("  A %d Hz\n", edges);
        }
        printf("IRQ %s\n", m->cpu.irq ? "held" : "free");
    }

    if (video_check) {
        // The full drawing, then the changed-lines one twice over (from
        // scratch, then with nothing changed) at both scales.
        static uint8_t full[A2E_FB_WIDTH * A2E_FB_HEIGHT], upd[A2E_FB_WIDTH * A2E_FB_HEIGHT];
        a2e_video_draw(m, full);
        static a2e_screen s2, s1;
        s2 = (a2e_screen){upd, (A2E_FB_HEIGHT - 384) / 2, false, {0}};
        uint32_t first = a2e_video_update(m, &s2), again = a2e_video_update(m, &s2);
        bool same2 = !memcmp(full, upd, sizeof full);
        memset(upd, 0, sizeof upd);
        s1 = (a2e_screen){upd, 24, true, {0}};
        a2e_video_update(m, &s1);
        bool same1 = true;
        for (int y = 0; y < 384 && same1; ++y)
            same1 = !memcmp(full + (48 + y) * A2E_FB_WIDTH, upd + (24 + y / 2) * A2E_FB_WIDTH,
                            A2E_FB_WIDTH);
        printf("video: %s 640x480, %s 640x240, %u then %u lines drawn\n",
               same2 ? "same" : "DIFFERENT", same1 ? "same" : "DIFFERENT", first, again);
    }

    if (ppm) {
        uint8_t *fb = must_calloc(A2E_FB_WIDTH * A2E_FB_HEIGHT);
        a2e_video_draw(m, fb);
        write_ppm(ppm, fb);
        free(fb);
    }
    if (verbose)
        fprintf(stderr,
                "cycles %llu  PC %04X A %02X X %02X Y %02X S %02X P %02X  clicks %u\n"
                "TEXT %d MIXED %d PAGE2 %d HIRES %d 80COL %d ALTCHAR %d DHIRES %d 80STORE %d\n"
                "RAMRD %d RAMWRT %d ALTZP %d INTCXROM %d SLOTC3ROM %d LC bank%d read %s write %d\n",
                (unsigned long long)m->cycles, m->cpu.pc, m->cpu.a, m->cpu.x, m->cpu.y, m->cpu.s,
                m->cpu.p, m->speaker_clicks, m->text, m->mixed, m->page2, m->hires, m->col80,
                m->altchar, m->dhires, m->store80, m->ramrd, m->ramwrt, m->altzp, m->intcxrom,
                m->slotc3rom, m->lc_bank2 ? 2 : 1, m->lc_read_ram ? "RAM" : "ROM",
                m->lc_write_ram);
    if (verbose && p5_path)
        fprintf(stderr, "disk: motor %s, drive %d, track %d\n", disk.motor ? "on" : "off",
                disk.sel + 1, disk.drive[disk.sel].halftrack / 2);
    return 0;
}
