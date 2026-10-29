/**
 * @file genesis_pico.c
 * @brief The genesis command on the Pico 2 mini OS.
 *
 * While it runs the scanout shows each framebuffer pixel as two by two
 * (dvi_line_double and dvi_pixel_double), so the picture takes 320x240 bytes
 * and the rest of the framebuffer (225 KB of SRAM) is free: the machine's
 * VRAM and its 68000 and Z80 RAM live there, with the YM2612's main tables
 * and the sound ring, and the core's hottest code runs from the end of it
 * (genesis_fast.h). The cartridge and the YM2612's LFO table are in PSRAM.
 *
 * A frame runs at a time on core 0, paced to the Genesis's 60 Hz (50 for
 * PAL). Core 1 draws its lines (genesis_render_pending, between its USB
 * work) into the framebuffer, 224 (or 240), while core 0 runs the 68000
 * and Z80 on. When a frame runs late and the drawing was what held core 0
 * up, the next ones aren't drawn (up to MAX_SKIP), so the game keeps its
 * speed.
 *
 * Sound: core 0 makes the YM2612's and SN76489's samples (about 53 kHz,
 * mono) as the CPUs write to them; after each frame they go into a ring in
 * SRAM, and core 1 takes them out at 48 kHz (genesis_sound, an audio
 * source), stretching or squeezing (up to about 2%) to keep the ring half full.
 * The YM2612 works out every other sample (genesis_ym_half_rate) unless
 * -f: at its full rate it takes core 0 to the edge of a frame, and a busy
 * game then falls behind and the sound has gaps.
 *
 * Pad 1 is the USB keyboard (arrows; Z, X, C are A, B, C; Return Start) and
 * the first USB gamepad; a second gamepad is pad 2. F11 resets, F10 goes back
 * to the shell; both work from the serial console too.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "genesis_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_overlay.h"
#include "audio.h"
#include "console.h"
#include "core1_job.h"
#include "dvi.h"
#include "ff.h"
#include "genesis.h"
#include "genesis_launcher.h"
#include "hdmi_audio.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "serial_console.h"
#include "shell.h"
#include "tusb.h"
#include "usb_keyboard.h"

/// Where the games are.
#define DEFAULT_DIR "/GENESIS"
/// The longest path.
#define PATH_LEN    320
#define MAX_SKIP    4  ///< frames in a row not drawn when behind

/// A frame in master clocks over the master clock: NTSC 896040 / 53693175
/// (16.688 ms), PAL 1067040 / 53203424 (20.056 ms); in ns.
#define FRAME_NS_NTSC 16688154u
#define FRAME_NS_PAL  20055853u  ///< PAL

/// The framebuffer: the picture (rows of 320), the machine's memory, the
/// YM2612's main tables, the sound ring, then the core's hottest code
/// (genesis_fast.h), linked to run there (src/ld/section_extra_post_data.incl)
/// and copied in from flash before the machine starts.
#define PIC_W      320
#define PIC_ROWS   240  ///< rows
#define MEM_AT     (PIC_W * PIC_ROWS)  ///< the machine's memory
#define YM_AT      ((MEM_AT + GENESIS_MEM_SIZE + 3) & ~3)  ///< the YM2612's tables
#define SOUND_AT   (YM_AT + GENESIS_YM_FAST_SIZE)  ///< the sound ring
#define SOUND_RING 2048  ///< samples (int16), about 38 ms
/// The sound ring.
#define sound_ring ((int16_t *)(dvi_framebuf + SOUND_AT))
/// The core's fast code: where it runs, its end, and where it is in flash.
///@{
extern char __genesis_fast_start[], __genesis_fast_end[], __genesis_fast_load[];
///@}

/// Whether the fast code fits after the sound ring.
/// @return it does
static bool fast_code_fits(void) {
    return (uint8_t *)__genesis_fast_start >= dvi_framebuf + SOUND_AT + SOUND_RING * 2;
}

// ---- Sound ----

/// Samples put in and taken out.
///@{
static volatile uint32_t sound_head, sound_tail;
///@}
static uint32_t sound_pos;   ///< core 1: the next sample's position, 16.16 past sound_tail
static uint32_t sound_step0;  ///< core 1: Genesis samples a 48 kHz sample, 16.16
static bool sound_playing;    ///< core 1: once the ring has filled to half
static int32_t sound_fill;    ///< core 1: how full the ring is, averaged (x 256)
/// Samples lost to a full ring, and 48 kHz samples with none ready.
///@{
static volatile uint32_t sound_dropped, sound_short;
///@}

/// Core 0, after each frame: the frame's samples into the ring.
static void sound_frame(void) {
    const int16_t *ym, *sn;
    int n = genesis_audio(&ym, &sn);
    uint32_t head = sound_head, room = SOUND_RING - (head - __atomic_load_n(&sound_tail, __ATOMIC_ACQUIRE));
    if ((uint32_t)n > room) {
        sound_dropped += n - room;
        n = (int)room;
    }
    for (int i = 0; i < n; i++) {
        int32_t s = ym[i] + sn[i];
        sound_ring[(head + i) % SOUND_RING] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    __atomic_store_n(&sound_head, head + n, __ATOMIC_RELEASE);
}

/// Core 1, at 48 kHz: the next sample, between the two Genesis samples it
/// falls between. The step is nudged by how full the ring is (half full
/// is right), so it follows the emulation's real speed.
/// @param left the left sample
/// @param right the right
/// @return true
static bool __attribute__((section("genesis_fast"))) genesis_sound(int32_t *left, int32_t *right) {
    static int32_t last;
    uint32_t tail = sound_tail, have = __atomic_load_n(&sound_head, __ATOMIC_ACQUIRE) - tail;
    if (!sound_playing) {  // silent until the ring is half full
        sound_playing = have >= SOUND_RING / 2;
        sound_fill = (int32_t)have * 256;
        *left = *right = 0;
        return true;
    }
    if (have < 2) {  // run dry: hold the last sample
        sound_short = sound_short + 1;
        *left = *right = last * 2;
        return true;
    }
    int32_t s0 = sound_ring[tail % SOUND_RING], s1 = sound_ring[(tail + 1) % SOUND_RING];
    uint32_t frac = sound_pos & 0xffff;
    last = s0 + (int32_t)(((s1 - s0) * (int32_t)frac) >> 16);
    *left = *right = last * 2;  // the mixer halves sources
    // The step follows how full the ring is, up to about 2% either way: a
    // game that falls behind a little slows the sound with it (a touch lower
    // and slower) instead of running dry; one that falls further behind gets
    // short gaps rather than slowing the music right down. The fill is
    // averaged over about 43 ms: it rises by a whole frame's samples at once
    // and falls in between, and followed as it is that made the pitch wobble
    // by almost 3% sixty times a second (now about 0.3%).
    sound_fill += ((int32_t)have * 256 - sound_fill) >> 11;
    int32_t off = sound_fill / 256 - SOUND_RING / 2;
    sound_pos += sound_step0 + (int32_t)((int64_t)sound_step0 * off / (SOUND_RING * 24));
    uint32_t whole = sound_pos >> 16;
    if (whole) {
        sound_pos &= 0xffff;
        __atomic_store_n(&sound_tail, tail + (whole < have ? whole : have - 1), __ATOMIC_RELEASE);
    }
    return true;
}

/// Starts the sound for NTSC or PAL.
/// @param pal PAL
static void sound_start(bool pal) {
    sound_head = sound_tail = 0;
    sound_pos = 0;
    sound_dropped = sound_short = 0;
    sound_playing = false;
    // Genesis samples a second: a frame's master clocks over 1009, at the frame rate.
    double rate = pal ? 1067040.0 / 1009 * 1e9 / FRAME_NS_PAL : 896040.0 / 1009 * 1e9 / FRAME_NS_NTSC;
    sound_step0 = (uint32_t)(rate / 48000 * 65536);
    audio_set_source(genesis_sound);
}


// ---- The ROM ----

/// A folder and a name joined.
/// @param out the path
/// @param n room in out
/// @param dir the folder
/// @param name the name
static void path_join(char *out, size_t n, const char *dir, const char *name) {
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name);
}

/// Why a game didn't start, for the console or the launcher's bottom line.
static char failed[160] APP_OV_DATA(genesis);  ///< (the apps' SRAM area, while it runs)

/// Reads the cartridge into PSRAM, telling progress how far it has got (0 to
/// 100). NULL (and why in failed) if it can't. The buffer is a little bigger
/// than the file and zeroed after it, so a 32-bit read at the end stays inside.
/// @param path the ROM
/// @param size set to its size
/// @param progress told how far it is, or NULL
/// @return the ROM, or NULL
static uint8_t *load_rom(const char *path, size_t *size, void (*progress)(const char *path, int percent)) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        snprintf(failed, sizeof failed, "%s: can't open it", path);
        return NULL;
    }
    FSIZE_t n = f_size(&f);
    if (n < 0x200 || n > GENESIS_ROM_MAX) {
        snprintf(failed, sizeof failed, "%s: not a Genesis ROM (%lu bytes)", path, (unsigned long)n);
        f_close(&f);
        return NULL;
    }
    size_t alloc = ((size_t)n + 1) & ~(size_t)1;
    uint8_t *rom = psram_buffer_alloc(alloc + 16);
    if (!rom) {
        snprintf(failed, sizeof failed, "Not enough memory for a %lu KB ROM.", (unsigned long)(n / 1024));
        f_close(&f);
        return NULL;
    }
    memset(rom + n, 0, alloc + 16 - n);
    FSIZE_t done = 0;
    bool ok = true;
    progress(path, 0);
    while (ok && done < n) {
        UINT chunk = n - done > 0x10000 ? 0x10000 : (UINT)(n - done), got;
        ok = f_read(&f, rom + done, chunk, &got) == FR_OK && got == chunk;
        done += chunk;
        progress(path, (int)(done * 100 / n));
    }
    f_close(&f);
    if (!ok) {
        snprintf(failed, sizeof failed, "%s: read error", path);
        psram_buffer_free(rom);
        return NULL;
    }
    *size = alloc;
    return rom;
}

/// Loading progress on the console: the percentage, rewritten in place.
/// @param path the ROM
/// @param percent how far
static void console_progress(const char *path, int percent) {
    if (!percent)
        printf("Loading %s   ", path);
    printf("\b\b\b\b%3d%%", percent);
    if (percent == 100)
        printf("\n");
}

/// And on the launcher's bottom line, with the file's name.
/// @param path the ROM
/// @param percent how far
static void launcher_progress(const char *path, int percent) {
    const char *slash = strrchr(path, '/');
    genesis_launcher_progress(slash ? slash + 1 : path, percent);
}

// ---- The picture ----

/// Each colour number (all 256: the core's four copies of the 64 CRAM
/// colours) as an RGB332 pixel, made again when CRAM changes.
static uint8_t lut[256] APP_OV_DATA(genesis);
static uint32_t lut_cram[32] APP_OV_DATA(genesis);  ///< the colours lut was made from (64 RGB565, in pairs)
/// The colour table is good for the palette.
static bool lut_valid;
static volatile int pic_top;  ///< framebuffer rows above the picture

/// Called as each line is drawn (on core 1): 320 colour numbers to RGB332.
/// The palette is checked each line, as games change it between lines.
static void __attribute__((section("genesis_fast")))
/// @param ctx unused
/// @param line the line
/// @param pix the colour numbers
/// @param pal the palette
put_line(void *ctx, int line, const uint8_t *pix, const uint16_t *pal) {
    const uint32_t *pal32 = (const uint32_t *)pal;
    bool same = lut_valid;
    for (int i = 0; same && i < 32; i++)
        same = pal32[i] == lut_cram[i];
    if (!same) {
        for (int i = 0; i < 32; i++)
            lut_cram[i] = pal32[i];
        for (int i = 0; i < 64; i++) {
            uint16_t c = pal[i];  // RGB565
            uint8_t p = (uint8_t)((c >> 13) << 5 | ((c >> 8) & 7) << 2 | ((c >> 3) & 3));
            lut[i] = lut[i + 64] = lut[i + 128] = lut[i + 192] = p;
        }
        lut_valid = true;
    }
    const uint32_t *in = (const uint32_t *)pix;
    uint32_t *out = (uint32_t *)(dvi_framebuf + line * PIC_W);
    for (int i = 0; i < PIC_W / 4; i += 2) {
        uint32_t a = in[i], b = in[i + 1];
        out[i] = lut[a & 0xff] | lut[(a >> 8) & 0xff] << 8 | lut[(a >> 16) & 0xff] << 16 |
                 (uint32_t)lut[a >> 24] << 24;
        out[i + 1] = lut[b & 0xff] | lut[(b >> 8) & 0xff] << 8 | lut[(b >> 16) & 0xff] << 16 |
                     (uint32_t)lut[b >> 24] << 24;
    }
}

/// The rows shown: 224 lines in the middle of the 240, or all 240 (PAL's V30).
/// @param height 224 or 240
static void set_view(int height) {
    if (dvi_double_rows == height)
        return;
    dvi_double_top = (PIC_ROWS - height) / 2;
    dvi_double_rows = height;
}

// ---- Keys and pads ----

/// What the keys ask for.
enum { ACT_NONE, ACT_RESET, ACT_EXIT, ACT_MENU, ACT_START };

/// F9, F10 and F11 from the console (USB keyboard or serial): ESC [ 20 ~,
/// ESC [ 21 ~ and ESC [ 23 ~. Everything else typed is dropped.
/// @return ACT_NONE and the like
static int console_keys(void) {
    static char seq[8];
    static int len;
    int c, act = ACT_NONE;
    while ((c = getchar_timeout_us(0)) >= 0) {
        if (c == 0x1b) {
            len = 1;
            seq[0] = 0x1b;
            continue;
        }
        if (!len) {
            // Return on the serial console taps Start (the USB keyboard's
            // Return is Start already, as a held key).
            if (c == '\r')
                act = ACT_START;
            continue;
        }
        if (len < (int)sizeof seq)
            seq[len++] = (char)c;
        if (c == '~') {
            if (len == 5 && seq[1] == '[' && seq[2] == '2' && seq[3] == '0')
                act = ACT_MENU;
            else if (len == 5 && seq[1] == '[' && seq[2] == '2' && seq[3] == '1')
                act = ACT_EXIT;
            else if (len == 5 && seq[1] == '[' && seq[2] == '2' && seq[3] == '3')
                act = ACT_RESET;
            len = 0;
        } else if (len == (int)sizeof seq) {
            len = 0;
        }
    }
    return act;
}

/// The keyboard as pad 1: arrows, Z X C, Return.
/// @return the buttons
static unsigned keyboard_pad(void) {
    uint8_t keys[6], mods;
    unsigned pad = 0;
    if (!usb_keyboard_held_keys(keys, &mods))
        return 0;
    for (int i = 0; i < 6; i++)
        switch (keys[i]) {
        case HID_KEY_ARROW_UP: pad |= GENESIS_UP; break;
        case HID_KEY_ARROW_DOWN: pad |= GENESIS_DOWN; break;
        case HID_KEY_ARROW_LEFT: pad |= GENESIS_LEFT; break;
        case HID_KEY_ARROW_RIGHT: pad |= GENESIS_RIGHT; break;
        case HID_KEY_Z: pad |= GENESIS_A; break;
        case HID_KEY_X: pad |= GENESIS_B; break;
        case HID_KEY_C: pad |= GENESIS_C; break;
        case HID_KEY_ENTER: pad |= GENESIS_START; break;
        }
    return pad;
}

/// USB gamepad n: the stick or D-pad, buttons 1-3 A, B, C, and any of
/// buttons 8-12 (where pads put Start and Select) Start.
/// @param n the pad
/// @return the buttons
static unsigned gamepad(unsigned n) {
    uint8_t x, y;
    uint32_t b;
    unsigned pad = 0;
    if (!usb_gamepad_read_n(n, &x, &y, &b))
        return 0;
    if (y < 64) pad |= GENESIS_UP;
    if (y > 191) pad |= GENESIS_DOWN;
    if (x < 64) pad |= GENESIS_LEFT;
    if (x > 191) pad |= GENESIS_RIGHT;
    if (b & 1) pad |= GENESIS_A;
    if (b & 2) pad |= GENESIS_B;
    if (b & 4) pad |= GENESIS_C;
    if (b & 0xf80) pad |= GENESIS_START;
    return pad;
}

// ---- The command ----

/// The core's clock.
/// @return microseconds
static uint32_t clock_us(void) {
    return time_us_32();
}

/// How a game runs.
typedef struct {
    long bench;     ///< frames to run as fast as it can, or 0
    bool one_core;  ///< draw on core 0 too
    bool half;      ///< the YM2612 at half rate
} options;

/// What happened in the last game, for the console (printed when genesis
/// gives the screen back) and the serial port.
static char summary[1024];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/// Adds to the summary.
/// @param fmt printf's format
static void say(const char *fmt, ...) {
    size_t len = strlen(summary);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(summary + len, sizeof summary - len, fmt, ap);
    va_end(ap);
}

/// Runs the game at path until F10 (ACT_EXIT) or F9 (ACT_MENU), with the
/// console's drawing already off; gives the screen back as 640x480, clear.
/// ACT_NONE (and why in failed) if it couldn't start.
/// @param path the ROM
/// @param o how
/// @param progress told how far loading is, or NULL
/// @return ACT_EXIT or ACT_MENU
static int run_game(const char *path, const options *o, void (*progress)(const char *path, int percent)) {
    failed[0] = 0;
    void *tables = psram_malloc(GENESIS_YM_SLOW_SIZE);
    size_t size;
    uint8_t *rom = tables ? load_rom(path, &size, progress) : NULL;
    if (!rom) {
        if (!tables)
            snprintf(failed, sizeof failed, "Not enough memory.");
        psram_free(tables);
        return ACT_NONE;
    }

    // The scanout doubles pixels both ways, which frees most of the
    // framebuffer for the machine.
    memset(dvi_framebuf, 0, PIC_W * PIC_ROWS);
    dvi_pixel_double = true;
    dvi_line_double = true;

    memcpy(__genesis_fast_start, __genesis_fast_load, __genesis_fast_end - __genesis_fast_start);
    __dsb();
    __isb();
    genesis_init(dvi_framebuf + MEM_AT, dvi_framebuf + YM_AT, tables);
    lut_valid = false;
    genesis_enable(true, true, true);
    genesis_ym_half_rate(o->half);
    genesis_set_clock(clock_us);
    memset(&genesis_time, 0, sizeof genesis_time);
    if (!o->one_core) {
        genesis_set_worker(true);
        core1_job_start(genesis_render_pending);
    }
    char name[49];
    snprintf(name, sizeof name, "%s", genesis_load(rom, size));
    set_view(genesis_height());
    sound_start(genesis_pal());
    audio_source_load();  // from now
    uint32_t underruns0 = hdmi_audio_underruns;

    // One frame at a time, paced to real time.
    uint64_t frames = 0, drawn = 0, busy_us = 0, worst_us = 0, late = 0;
    uint64_t sched_us = time_us_64(), sched_frames = 0;
    // NTSC frames start as the scanout's vertical sync does (see below).
    uint32_t frame_vsync = dvi_frames;
    while (dvi_frames == frame_vsync)
        __wfi();
    frame_vsync = dvi_frames;
    // The XIP cache's (flash and PSRAM) accesses and hits, counted by the
    // hardware (32-bit saturating counters, so they're taken each frame).
    uint64_t xip_acc = 0, xip_hit = 0;
    xip_ctrl_hw->ctr_acc = 0;
    xip_ctrl_hw->ctr_hit = 0;
    int skip = 0, act, start_tap = 0;
    for (;;) {
        act = console_keys();
        if (act == ACT_EXIT || act == ACT_MENU)
            break;
        if (act == ACT_RESET)
            genesis_reset();
        if (act == ACT_START)
            start_tap = 10;  // frames
        genesis_set_pad(0, keyboard_pad() | gamepad(0) | (start_tap ? GENESIS_START : 0));
        if (start_tap)
            --start_tap;
        genesis_set_pad(1, gamepad(1));

        uint64_t start = time_us_64(), waited = genesis_time.wait_us;
        bool draw = skip == 0 || o->bench;
        genesis_run_frame(draw ? put_line : NULL, NULL);
        sound_frame();
        if (draw) {
            ++drawn;
            set_view(genesis_height());
        }
        uint64_t now = time_us_64(), used = now - start;
        xip_acc += xip_ctrl_hw->ctr_acc;
        xip_hit += xip_ctrl_hw->ctr_hit;
        xip_ctrl_hw->ctr_acc = 0;
        xip_ctrl_hw->ctr_hit = 0;
        busy_us += used;
        if (used > worst_us)
            worst_us = used;
        ++frames;

        if (o->bench) {
            if ((long)frames >= o->bench) {
                act = ACT_EXIT;
                break;
            }
            continue;
        }
        // Behind: the next frame isn't drawn, but only if drawing held
        // core 0 up (more than a millisecond waiting for core 1). Otherwise
        // the CPUs are the slow part, and drawing on core 1 costs them little.
        bool held_up = o->one_core || genesis_time.wait_us - waited > 1000;
        if (!genesis_pal()) {
            // NTSC: in step with the HDMI picture (59.94 Hz, 0.03% faster than
            // the Genesis's 59.92, which the sound takes up): each frame starts
            // as the scanout's vertical sync does, about 1.5 ms before it reads
            // the picture's first line and 64 us a line after that, so the
            // lines are drawn ahead of it and the picture doesn't tear. A frame
            // that ran past the next sync starts at once (and may tear) and
            // the one after it at the sync after that.
            uint32_t next = frame_vsync + 1;
            if ((int32_t)(dvi_frames - next) < 0) {
                skip = 0;
                while ((int32_t)(dvi_frames - next) < 0)
                    __wfi();
                frame_vsync = next;
            } else {
                if ((int32_t)(dvi_frames - next) >= 6)  // 100 ms behind
                    ++late;
                frame_vsync = dvi_frames;
                skip = held_up && skip < MAX_SKIP ? skip + 1 : 0;
            }
            continue;
        }
        uint32_t frame_ns = FRAME_NS_PAL;
        uint64_t due = sched_us + (frames - sched_frames) * frame_ns / 1000;
        if (now > due + 100000) {
            // Far behind: start the schedule again.
            ++late;
            sched_us = now;
            sched_frames = frames;
            due = now;
        }
        if (due > now) {
            skip = 0;
            sleep_until(from_us_since_boot(due));
        } else {
            skip = held_up && skip < MAX_SKIP ? skip + 1 : 0;
        }
    }

    unsigned sound_load = audio_source_load();
    audio_set_source(NULL);

    // Give the screen back.
    genesis_render_wait();
    if (!o->one_core) {
        core1_job_stop();
        genesis_set_worker(false);
    }
    psram_buffer_free(rom);
    psram_free(tables);
    dvi_double_top = 0;
    dvi_double_rows = PIC_ROWS;
    dvi_line_double = false;
    dvi_pixel_double = false;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);

    summary[0] = 0;
    say("Genesis: \"%s\"\n", name);
    uint64_t frame_us = (genesis_pal() ? FRAME_NS_PAL : FRAME_NS_NTSC) / 1000;
    say("%llu frames, %llu drawn; emulation took %llu%% of real time (%llu us a frame, worst %llu, of %llu)%s.\n",
        (unsigned long long)frames, (unsigned long long)drawn,
        (unsigned long long)(busy_us * 100 / (frames * frame_us)), (unsigned long long)(busy_us / frames),
        (unsigned long long)worst_us, (unsigned long long)frame_us, late ? "; fell behind" : "");
    uint64_t core0_draw = o->one_core ? genesis_time.draw_us : genesis_time.wait_us;
    say("Sound chips (catching up, besides at each write): %llu us a frame.\n",
        (unsigned long long)(genesis_time.sound_us / frames));
    say("Per frame: 68000 %llu us, Z80 %llu us, drawing %llu us a drawn frame on core %d (%llu of it"
        " colours to RGB332), waiting for it %llu us, the rest %llu us.\n",
        (unsigned long long)(genesis_time.m68k_us / frames), (unsigned long long)(genesis_time.z80_us / frames),
        (unsigned long long)(drawn ? genesis_time.draw_us / drawn : 0), o->one_core ? 0 : 1,
        (unsigned long long)(drawn ? genesis_time.put_us / drawn : 0),
        (unsigned long long)(genesis_time.wait_us / frames),
        (unsigned long long)((busy_us - genesis_time.m68k_us - genesis_time.z80_us - core0_draw -
                              genesis_time.sound_us) / frames));
    say("Sound: core 1 took %u.%u%% for it; %lu samples dropped (ring full), %lu short (empty);"
        " HDMI audio ran dry %lu times.\n",
        sound_load / 10, sound_load % 10, (unsigned long)sound_dropped, (unsigned long)sound_short,
        (unsigned long)(hdmi_audio_underruns - underruns0));
    say("XIP cache: %llu accesses a frame, %llu.%llu%% hits.\n", (unsigned long long)(xip_acc / frames),
        (unsigned long long)(xip_acc ? xip_hit * 100 / xip_acc : 0),
        (unsigned long long)(xip_acc ? xip_hit * 1000 / xip_acc % 10 : 0));
    return act;
}

/// The launcher's folder and the last game run from it, kept while the
/// board is on.
/// Where the launcher was, kept between runs: in PSRAM, made the first time
/// (SRAM is for what's running).
static struct {
    char dir[PATH_LEN];  ///< the folder
    char last[256];  ///< the last game played
} *launch;
/// The launcher's folder.
#define launch_dir  (launch->dir)
/// The last game.
#define launch_last (launch->last)

void genesis_main(int argc, char **argv) {
    const char *dir = DEFAULT_DIR, *file = NULL;
    options o = {.half = true};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            o.bench = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s"))
            o.one_core = true;
        else if (!strcmp(argv[i], "-f"))
            o.half = false;
        else if (argv[i][0] != '-' && !file)
            file = argv[i];
        else {
            printf("usage: genesis [-d folder] [-b frames] [-s] [-f] [rom.bin]\n"
                   "  without a ROM: the games in the folder (default %s), to pick one\n"
                   "  -f full-quality FM sound (the YM2612 at its own rate; heavy games may stutter)\n"
                   "  -b runs that many frames as fast as it can, drawing them all, and stops\n"
                   "  -s draws on core 0 too (normally core 1 draws)\n",
                   DEFAULT_DIR);
            return;
        }
    }
    if (!shell_drive_mounted()) {
        printf("No USB drive. Games go in %s on it.\n", dir);
        return;
    }
    if (!fast_code_fits()) {
        printf("No room for the Genesis: GENESIS_FAST_AT is too low.\n");
        return;
    }
    if (!launch && (launch = psram_malloc(sizeof *launch)) != NULL)
        memset(launch, 0, sizeof *launch);
    if (!launch) {
        printf("Not enough memory.\n");
        return;
    }
    char path[PATH_LEN];
    if (file) {
        FILINFO fi;
        snprintf(path, sizeof path, "%s", file);
        if (f_stat(path, &fi) != FR_OK) {
            path_join(path, sizeof path, dir, file);
            if (f_stat(path, &fi) != FR_OK) {
                printf("%s: not found, looked here and in %s\n", file, dir);
                return;
            }
        }
    }
    // The launcher starts in -d's folder when given, else where it was.
    bool dir_given = false;
    for (int i = 1; i < argc; ++i)
        dir_given |= !strcmp(argv[i], "-d");
    if (dir_given || !launch_dir[0]) {
        snprintf(launch_dir, sizeof launch_dir, "%s", dir);
        launch_last[0] = 0;
    }

    // Take the screen from the console.
    console_write("\x1b[?25l", 6);
    stdio_set_driver_enabled(&console_stdio_driver, false);
    summary[0] = 0;
    int act = ACT_NONE;
    if (file) {
        act = run_game(path, &o, console_progress);
        if (act == ACT_NONE) {  // the console still shows the shell: say why there
            stdio_set_driver_enabled(&console_stdio_driver, true);
            console_write("\x1b[?25h", 6);
            printf("%s\n", failed);
            return;
        }
        // F9 from a game given on the command line opens the launcher in its folder.
        if (act == ACT_MENU) {
            const char *slash = strrchr(path, '/');
            if (slash) {
                snprintf(launch_dir, sizeof launch_dir, "%.*s", (int)(slash - path), path);
                if (!launch_dir[0])
                    snprintf(launch_dir, sizeof launch_dir, "/");
                snprintf(launch_last, sizeof launch_last, "%.255s", slash + 1);
            }
        }
    }
    // The launcher, and the games picked from it, until Esc or F10 there.
    if (!file || act == ACT_MENU) {
        const char *message = NULL;
        for (;;) {
            if (summary[0])
                debug_write(summary, strlen(summary));
            if (!genesis_launcher(launch_dir, sizeof launch_dir, launch_last, message, path, sizeof path))
                break;
            const char *slash = strrchr(path, '/');
            snprintf(launch_last, sizeof launch_last, "%.255s", slash ? slash + 1 : path);
            message = NULL;
            if (run_game(path, &o, launcher_progress) == ACT_NONE)
                message = failed;
        }
    }

    // Give the screen back to the console, and say how the last game went.
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    console_write("\x1b" "c", 2);
    while (getchar_timeout_us(0) >= 0) {
    }
    printf("%s", summary);
}
