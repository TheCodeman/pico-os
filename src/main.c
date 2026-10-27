/**
 * @file main.c
 * @brief Pico 2 mini OS: a command shell on an HDMI text console with sound, typed on a USB keyboard (or the Debug Probe's serial port), working on files on a USB flash drive.
 *
 * Core 0 runs the console, the HDMI scanout interrupt and (in a low-priority
 * interrupt) the WiFi network stack. Core 1 runs the USB host stack and keeps
 * the HDMI audio queue full.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "console.h"
#include "core1_job.h"
#include "dvi.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "net.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "serial_console.h"
#include "shell.h"
#include "stack_switch.h"
#include "tusb.h"
#include "usb_hub_kick.h"
#include "usb_keyboard.h"
#include "mouse.h"
#include "usb_log.h"
#include "usb_storage.h"

/// The shell (and BASIC and the editor under it) runs on this, not on core
/// 0's 4 KB boot stack; interrupt handlers, the WiFi stack's included, run on
/// it too.
#define SHELL_STACK_SIZE (16 * 1024)

volatile uint32_t usb_restarts;  ///< read over SWD when looking into USB trouble

/// A program's work for core 1 (core1_job.h), and how many times core 1 has
/// been round its loop, so core1_job_stop can tell it has left the job.
static void (*volatile core1_job)(void);
static volatile uint32_t core1_loops;

void core1_job_start(void (*fn)(void)) {
    core1_job = fn;
}

void (*core1_job_now(void))(void) {
    return core1_job;
}

void core1_job_stop(void) {
    core1_job = NULL;
    uint32_t seen = core1_loops;
    while (core1_loops - seen < 2)  // a whole time round without it
        tight_loop_contents();
}

static void core1_main(void) {
    usb_keyboard_start();
    bool refilled = audio_start_refill();
    for (;;) {
        usb_keyboard_task();  // runs tuh_task() for every USB device
        usb_hub_kick_task();
        if (usb_restart_wanted()) {
            // Something got stuck: start the host stack again. Unmounting
            // everything fails any drive request that was waiting.
            usb_restarts = usb_restarts + 1;
            tuh_deinit(0);
            mouse_reset();  // their slots, as TinyUSB may not say they went
            sleep_ms(100);
            tuh_init(0);
            usb_hub_kick_reset();
        }
        usb_storage_task();
        if (!refilled)
            audio_task();
        void (*job)(void) = core1_job;
        if (job)
            job();
        core1_loops = core1_loops + 1;
    }
}

#define DRIVE_WAIT_MS 10000  ///< start without a drive after this long
#define CHIME_AFTER_MS 2000  ///< after the video starts
volatile uint32_t psram_test_ms;  ///< how long the boot check took (read over SWD)

static volatile bool chimed;

static int64_t start_chime(alarm_id_t id, void *unused) {
    audio_chime();
    chimed = true;
    return 0;  // once
}

/// Start-up screen colours (ANSI, understood by the console and terminals).
#define TITLE  "\x1b[97;44m"  ///< bright white on blue
#define DIM    "\x1b[90m"
#define GOOD   "\x1b[92m"
#define WARN   "\x1b[93m"
#define PLAIN  "\x1b[0m"

/// "  Keyboard  " in grey: the start of a status line.
static void label(const char *name) {
    printf("  " DIM "%-10s" PLAIN, name);
}

static int keyboards, mice;
static bool drive;
static bool starting = true;  ///< start_up lists the drive itself

/// USB and video events, reported on the screen in one short line each (the
/// details go to the debug UART).
static void poll_events(void) {
    int n = usb_keyboard_count();
    if (n != keyboards) {
        shell_async_begin();
        printf(n > keyboards ? "[keyboard connected]\n" : "[keyboard disconnected]\n");
        shell_async_end();
        keyboards = n;
    }
    n = mouse_count();
    if (n != mice) {
        shell_async_begin();
        printf(n > mice ? "[mouse connected]\n" : "[mouse disconnected]\n");
        shell_async_end();
        mice = n;
    }
    char log[64];
    int len = 0, ch;
    while ((ch = usb_log_getc()) >= 0) {
        log[len++] = ch;
        if (len == sizeof log) {
            debug_write(log, len);
            len = 0;
        }
    }
    debug_write(log, len);
    if (!starting && usb_storage_ready() != drive) {
        drive = !drive;
        shell_drive_changed(drive);
        if (drive)
            mouse_load_config();
    }
    static uint32_t restarts;
    if (dvi_restarts != restarts) {
        restarts = dvi_restarts;
        debug_printf("video: restarted the HDMI output (%lu so far, %lu after a late"
                     " interrupt; longest gap %lu us)\n",
                     (unsigned long)restarts, (unsigned long)dvi_late,
                     (unsigned long)dvi_worst_gap_us);
    }
}

static bool spinner_shown;

/// Clears the "Waiting..." line so a status line can take its place.
static void unspin(void) {
    if (spinner_shown)
        printf("\r\x1b[K");
    spinner_shown = false;
}

/// Start-up: lists the keyboard, drive and WiFi as they appear, with a
/// spinner, and gives the prompt once there's a keyboard and a drive. A key
/// on the serial console also starts the shell (it's then the keyboard), and
/// after DRIVE_WAIT_MS it starts without a drive. Returns a key typed
/// meanwhile, or -1.
static int start_up(void) {
    static const char spin[] = "|/-\\";
    uint64_t start = time_us_64();
    bool kb_done = false, drive_done = false;
    int key = -1, frame = -1;
    for (;;) {
        int k = getchar_timeout_us(10000);
        if (k >= 0)
            key = k;
        int n = usb_keyboard_count();
        if (n && !keyboards) {
            keyboards = n;
            unspin();
            label("Keyboard");
            printf(GOOD "ready" PLAIN "\n");
            kb_done = true;
        }
        if (usb_storage_ready() && !drive) {
            drive = true;
            unspin();
            label("Drive");
            shell_drive_changed(true);  // finishes the Drive line
            mouse_load_config();
            drive_done = true;
            label("WiFi");
            net_print_status();
            printf("\n");
        }
        mice = mouse_count();  // listed with the rest at the end, not as an event
        if (kb_done && keyboards)
            keyboards = usb_keyboard_count();  // a keyboard's second part: no line (it
                                               // would push the WiFi line, which is redrawn)
        poll_events();
        uint64_t ms = (time_us_64() - start) / 1000;
        if (k >= 0 && !kb_done) {
            unspin();
            label("Keyboard");
            printf(WARN "none yet" DIM " (using the serial console)" PLAIN "\n");
            kb_done = true;
        }
        if (!drive_done && ms > DRIVE_WAIT_MS) {
            unspin();
            label("Drive");
            printf(WARN "none" DIM " (plug one in any time)" PLAIN "\n");
            drive_done = true;
        }
        if (kb_done && drive_done) {
            unspin();
            label("Mouse");
            printf(mice ? GOOD "ready" PLAIN "\n" : DIM "none (plug one in any time)" PLAIN "\n");
            starting = false;
            return key;
        }
        int f = (int)(ms / 150) % 4;
        if (f != frame || !spinner_shown) {
            frame = f;
            printf("\r  " DIM "Waiting for the %s %c" PLAIN,
                   !kb_done && !drive_done ? "keyboard and drive" : !kb_done ? "keyboard" : "drive",
                   spin[f]);
            fflush(stdout);
            spinner_shown = true;
        }
    }
}

static void main_loop(void *unused) {
    net_init();
    int key = start_up();
    if (shell_autorun(key) && (key == 0x1b || key == 3))
        key = -1;  // it was to skip /AUTORUN
    shell_start();
    if (key >= 0)
        shell_key(key);
    for (;;) {
        poll_events();
        net_task();
        shell_task();
        int c = getchar_timeout_us(10000);
        if (c != PICO_ERROR_TIMEOUT)
            shell_key(c);
    }
}


int main(void) {
    // 126 MHz (or twice that, divided by 2 for HSTX) gives the exact 25.2 MHz
    // 640x480 pixel clock.
    // The boot ROM runs the flash at clk_sys / 3: 42 MHz, or 84 MHz at 252
    // (well within the chip's 133 MHz; reading the whole image back at 84
    // MHz while everything ran matched).
#if OS_CLOCK_MHZ > 150
    vreg_set_voltage(VREG_VOLTAGE_1_20);  // overclocked: more core voltage first
    busy_wait_us(10000);
#endif
    set_sys_clock_khz(OS_CLOCK_MHZ * 1000, true);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, OS_CLOCK_MHZ * MHZ,
                    126 * MHZ);
    size_t psram_size = psram_setup();

    stdio_init_all();  // UART0 on GP0 (TX) / GP1 (RX), wired to the Debug Probe
    serial_console_init();
    dvi_init();
    console_init();
    audio_init();
    usb_keyboard_init();
    usb_log_init();
    // Reset core 1 first: after a debugger reset (e.g. OpenOCD "program ... reset")
    // it can be left asleep in the boot ROM where a plain launch doesn't wake it,
    // and USB and audio would then stay dead until a power cycle.
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    // The chime, from a timer, so it plays while the memory is checked:
    // displays take a second or two to lock on and unmute after the video
    // starts, so not before then.
    add_alarm_in_ms(CHIME_AFTER_MS, start_chime, NULL, true);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    stdio_set_driver_enabled(&usb_keyboard_stdio_driver, true);

    // A title bar across the screen. Colours are ANSI escapes, so the serial
    // console shows the same start-up screen as the HDMI one.
    char chip[48];
    unsigned mhz = (clock_get_hz(clk_sys) + MHZ / 2) / MHZ;  // what it really runs at
    if (psram_size)
        snprintf(chip, sizeof chip, "RP2350 @ %u MHz, %u MB PSRAM", mhz, (unsigned)(psram_size >> 20));
    else
        snprintf(chip, sizeof chip, "RP2350 @ %u MHz", mhz);
    printf(TITLE "  Pico 2 mini OS%*s  " PLAIN "\n\n", CONSOLE_COLS - 18, chip);
    debug_printf("640x480 HDMI text console, %dx%d characters, 48 kHz stereo audio\n",
                 CONSOLE_COLS, CONSOLE_ROWS);
    if (psram_size) {
        // Check every byte once at boot, before anything is allocated.
        psram_stats_t st;
        psram_get_stats(&st);
        void *start = psram_malloc(st.largest);
        printf("  " DIM "Checking memory..." PLAIN);
        fflush(stdout);
        // After the chime: the check keeps the memory bus so busy that core 1,
        // running from flash on the same bus, can't keep the sound going.
        while (!chimed || audio_chime_playing())
            sleep_ms(5);
        uint64_t t = time_us_64();
        void *bad = psram_test(start, st.largest);
        t = time_us_64() - t;
        psram_test_ms = (uint32_t)(t / 1000);
        printf("\r\x1b[K");
        psram_free(start);
        if (bad)
            printf("PSRAM: %u MB, FAILED at %p\n", (unsigned)(psram_size >> 20), bad);
        else
            debug_printf("PSRAM: %u MB OK (tested in %u ms)\n", (unsigned)(psram_size >> 20),
                         (unsigned)(t / 1000));
    } else {
        printf("PSRAM: not found\n");
    }
    usb_log_use_psram();
    void *stack = malloc(SHELL_STACK_SIZE);
    if (!stack)
        panic("no memory for the shell's stack");
    call_on_stack(main_loop, NULL, stack, SHELL_STACK_SIZE);
}
