/**
 * @file basic_port_pico.c
 * @brief Part of Pico 2 mini OS.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "basic_port.h"

#include <malloc.h>
#include <unistd.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "audio.h"
#include "console.h"
#include "basic.h"
#include "dvi.h"
#include "shell.h"
#include <string.h>
#include "editor.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "mod.h"
#include "xm.h"
#include "mouse.h"
#include "usb_keyboard.h"
#include "hardware/xip_cache.h"
#include "stack_switch.h"

int port_key(uint32_t timeout_us) {
    // A running program asks every statement: look at the input drivers at
    // most every 200 us (a key waits that long at worst), so core 0 isn't
    // forever in stdio's locks while core 1 mixes music.
    static uint32_t last_poll;
    uint32_t now = time_us_32();
    if (!timeout_us && now - last_poll < 200)
        return -1;
    last_poll = now;
    int c = getchar_timeout_us(timeout_us);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

void port_put_glyph(int c) {
    console_put_glyph(c);
}

bool port_play(unsigned freq_hz, unsigned ms) {
    return audio_play(freq_hz, ms);
}

int port_play_pending(void) {
    return audio_play_pending();
}

void port_play_stop(void) {
    audio_play_stop();
}

void port_get_cell(int row, int col, uint8_t *ch, uint8_t *attr) {
    console_get_cell(row, col, ch, attr);
}

void port_set_cell(int row, int col, uint8_t ch, uint8_t attr) {
    console_set_cell(row, col, ch, attr);
}

bool port_shell(const char *command) {
    return shell_run_command(command);
}

void port_beep(unsigned freq_hz, unsigned ms) {
    audio_beep(freq_hz, ms);
}

void port_sleep_ms(unsigned ms) {
    sleep_ms(ms);
}

double port_seconds(void) {
    return to_us_since_boot(get_absolute_time()) / 1e6;
}

// ---- Memory: big blocks in PSRAM, small ones in SRAM ---------------------------
// The PSRAM allocator walks its block list on every call, so the many small,
// short-lived blocks (strings, temporaries, program lines) stay in SRAM, which
// is faster too; arrays and other big blocks go to PSRAM. When SRAM runs low,
// small blocks go to PSRAM as well.

/// Allocations from this size up go to PSRAM.
#define BIG_BLOCK  512
#define SRAM_KEEP  (24 * 1024)  ///< left for the system, the editor and network

/// The SRAM heap's free bytes (above it and freed inside).
/// @return them
static size_t sram_free(void) {
    extern char __StackLimit;  // top of the heap, from the SDK's linker script
    struct mallinfo mi = mallinfo();
    return (size_t)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks;
}

/// Whether memory is in PSRAM.
/// @param p the memory
/// @return it is
static bool is_psram(const void *p) {
    return (uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x12000000u;
}

/// PSRAM's free bytes.
/// @return them
static size_t psram_free_bytes(void) {
    return psram_free_total();
}

/// True if SRAM has room for n more bytes. Cheap in the usual case: the
/// untouched space above the heap (mallinfo walks the whole heap, so it's only
/// asked when that space is short).
/// @param n the bytes
/// @return there is room
static bool sram_room(size_t n) {
    extern char __StackLimit;
    if ((size_t)(&__StackLimit - (char *)sbrk(0)) >= n + SRAM_KEEP)
        return true;
    return sram_free() >= n + SRAM_KEEP;
}

void *port_alloc(size_t n) {
    if (!n)
        n = 1;
    if (n < BIG_BLOCK && sram_room(n))  // the usual case
        return malloc(n);
    void *p = psram_malloc(n);
    if (p || n >= BIG_BLOCK)
        return p;
    // PSRAM full and SRAM short: malloc panics when it runs out, so only if
    // there's still a safe margin.
    return sram_free() >= n + SRAM_KEEP / 2 ? malloc(n) : NULL;
}

void *port_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size)
        return NULL;
    void *p = port_alloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void port_release(void *p) {
    if (is_psram(p))
        psram_free(p);
    else
        free(p);
}

void *port_realloc(void *p, size_t n) {
    if (!p)
        return port_alloc(n);
    if (!n) {
        port_release(p);
        return NULL;
    }
    size_t old = is_psram(p) ? psram_block_size(p) : malloc_usable_size(p);
    if (is_psram(p)) {
        void *q = psram_realloc(p, n);
        if (q)
            return q;
    } else if (n < BIG_BLOCK && sram_room(n)) {
        return realloc(p, n);
    }
    // Move it (SRAM to PSRAM as it grows, or PSRAM full: try SRAM).
    void *q = port_alloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, old < n ? old : n);
    port_release(p);
    return q;
}

size_t port_free_mem(void) {
    size_t s = sram_free();
    return (s > SRAM_KEEP ? s - SRAM_KEEP : 0) + psram_free_bytes();
}

/// BASIC's expression evaluator recurses; it runs on the shell's 16 KB stack
/// (see main.c) and checks it has this much left, for interrupts (the WiFi
/// stack's runs on whatever stack is in use) and the deepest leaf calls.
#define STACK_MARGIN 2048

bool port_stack_low(void) {
    char here;
    return &here < stack_bottom() + STACK_MARGIN;
}

// ---- Screen pages -------------------------------------------------------------
// The page on show lives in the framebuffer; the others are kept in PSRAM and
// swapped in by copying when SCREEN changes the visual page.

/// Graphics pages.
#define MAX_PAGES  8
/// Bytes in a page.
#define PAGE_BYTES (DVI_WIDTH * DVI_HEIGHT)

static uint8_t *pages[MAX_PAGES];  ///< NULL until used; stale while on show
/// The page on show.
static int visual_page;
/// Where drawing goes: the screen, or a page in PSRAM.
static uint8_t *draw_fb = dvi_framebuf;

/// Makes page n if it isn't yet.
/// @param n the page
/// @return it is there
static bool need_page(int n) {
    if (!pages[n]) {
        pages[n] = psram_malloc(PAGE_BYTES);
        if (!pages[n])
            return false;
        memset(pages[n], 0, PAGE_BYTES);
    }
    return true;
}

const char *port_set_pages(int active, int visual) {
    if (active < 0 || active >= MAX_PAGES || visual < 0 || visual >= MAX_PAGES)
        return "no such page";
    if ((active != visual && !need_page(active)) ||
        (visual != visual_page && (!need_page(visual) || !need_page(visual_page))))
        return "not enough memory for the screen pages";
    if (visual != visual_page) {
        memcpy(pages[visual_page], dvi_framebuf, PAGE_BYTES);
        memcpy(dvi_framebuf, pages[visual], PAGE_BYTES);
        visual_page = visual;
    }
    draw_fb = active == visual ? dvi_framebuf : pages[active];
    return NULL;
}

const char *port_copy_page(int from, int to) {
    if (from < 0 || from >= MAX_PAGES || to < 0 || to >= MAX_PAGES)
        return "no such page";
    if (from == to)
        return NULL;
    uint8_t *src = from == visual_page ? dvi_framebuf : (need_page(from) ? pages[from] : NULL);
    uint8_t *dst = to == visual_page ? dvi_framebuf : (need_page(to) ? pages[to] : NULL);
    if (!src || !dst)
        return "not enough memory for the screen pages";
    memcpy(dst, src, PAGE_BYTES);
    return NULL;
}

void port_reset_pages(void) {
    port_set_pages(0, 0);
    for (int i = 0; i < MAX_PAGES; ++i) {
        psram_free(pages[i]);
        pages[i] = NULL;
    }
}

void port_fill_rect(int x, int y, int w, int h, uint8_t rgb332) {
    uint8_t *row = &draw_fb[y * DVI_WIDTH + x];
    for (int i = 0; i < h; ++i, row += DVI_WIDTH)
        memset(row, rgb332, w);
}

uint8_t port_get_pixel(int x, int y) {
    return draw_fb[y * DVI_WIDTH + x];
}

void port_edit(const char *path) {
    editor_run(path);
}

// ---- GPIO -------------------------------------------------------------------
// The RP2350B on the Pico Plus 2 has GPIO 0-47; ADC inputs are GP40-47.

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

/// GPIO pins.
#define PIN_COUNT 48
/// The LED's pin.
#define LED_PIN   25
/// The first ADC pin.
#define ADC_FIRST 40

/// What a pin is being used for.
enum { UNUSED, INPUT, OUTPUT, PULLUP, PULLDOWN, PWM, ADC };
static uint8_t pin_use[PIN_COUNT];      ///< what BASIC has set each pin to
/// Each PWM slice's frequency.
static float slice_freq[NUM_PWM_SLICES];

/// "1000" or "8.5": the SDK's printf has no %g.
/// @param f the frequency
/// @param buf the text
/// @param size room in buf
/// @return buf
static const char *freq_text(double f, char *buf, size_t size) {
    long whole = (long)(f + 0.05);
    int tenth = (int)((f + 0.05 - whole) * 10);
    if (tenth)
        snprintf(buf, size, "%ld.%d", whole, tenth);
    else
        snprintf(buf, size, "%ld", whole);
    return buf;
}

int port_pin_count(void) {
    return PIN_COUNT;
}

int port_led_pin(void) {
    return LED_PIN;
}

/// COM1's TX and RX pins while it is open (-1: closed).
///@{
static int serial_tx = -1, serial_rx = -1;
///@}
static int i2c_pins[2] = {-1, -1};          ///< SDA, SCL while I2C is open
static int spi_pins[3] = {-1, -1, -1};      ///< SCK, MOSI, MISO while SPI is open
/// Its flow control pins (CTS, RTS) while that is on.
///@{
static int serial_cts = -1, serial_rts = -1;
///@}

const char *port_pin_reserved(int pin) {
    if (pin == serial_tx || pin == serial_rx || pin == serial_cts || pin == serial_rts)
        return "the COM1 serial port (CLOSE it first)";
    if (pin >= 0 && (pin == i2c_pins[0] || pin == i2c_pins[1]))
        return "I2C (I2C CLOSE first)";
    if (pin >= 0 && (pin == spi_pins[0] || pin == spi_pins[1] || pin == spi_pins[2]))
        return "SPI (SPI CLOSE first)";
    if (pin == 0 || pin == 1)
        return "the debug serial port";
    if (pin >= 12 && pin <= 19)
        return "HDMI";
    if (pin >= 32 && pin <= 36)
        return "the WiFi breakout";
    if (pin == 47)
        return "PSRAM";
    return NULL;
}

void port_pin_mode(int pin, int mode) {
    if (pin_use[pin] == PWM)
        port_pwm(pin, 0, 0);
    gpio_init(pin);
    gpio_set_dir(pin, mode == PORT_PIN_OUTPUT);
    gpio_set_pulls(pin, mode == PORT_PIN_PULLUP, mode == PORT_PIN_PULLDOWN);
    pin_use[pin] = mode == PORT_PIN_OUTPUT   ? OUTPUT
                   : mode == PORT_PIN_PULLUP ? PULLUP
                   : mode == PORT_PIN_PULLDOWN ? PULLDOWN
                                               : INPUT;
}

void port_pin_write(int pin, int level) {
    if (pin_use[pin] != OUTPUT)
        port_pin_mode(pin, PORT_PIN_OUTPUT);
    gpio_put(pin, level != 0);
}

int port_pin_read(int pin) {
    if (pin_use[pin] == UNUSED || pin_use[pin] == PWM || pin_use[pin] == ADC)
        port_pin_mode(pin, PORT_PIN_INPUT);
    return gpio_get(pin);
}

void port_pwm(int pin, double freq, double duty) {
    uint slice = pwm_gpio_to_slice_num(pin);
    uint partner = pin ^ 1;
    if (freq <= 0) {
        if (pin_use[pin] == PWM) {
            pwm_set_gpio_level(pin, 0);
            if (partner >= PIN_COUNT || pin_use[partner] != PWM)
                pwm_set_enabled(slice, false);
        }
        gpio_init(pin);  // back to a plain input
        pin_use[pin] = INPUT;
        return;
    }
    // Pick the smallest divider that lets the counter's top fit 16 bits,
    // for the finest duty steps.
    float clk = (float)clock_get_hz(clk_sys);
    float div = clk / ((float)freq * 65536.0f);
    if (div < 1.0f)
        div = 1.0f;
    if (div > 255.9f)
        div = 255.9f;
    uint32_t top = (uint32_t)(clk / (div * (float)freq)) - 1;
    if (top > 65535)
        top = 65535;
    if (partner < PIN_COUNT && pin_use[partner] == PWM && slice_freq[slice] != (float)freq) {
        char text[16];
        printf("Warning: GP%u shares a PWM slice with GP%d; both now run at %s Hz\n", partner,
               pin, freq_text(freq, text, sizeof text));
    }
    pwm_set_clkdiv(slice, div);
    pwm_set_wrap(slice, top);
    if (duty < 0)
        duty = 0;
    if (duty > 100)
        duty = 100;
    pwm_set_gpio_level(pin, (uint32_t)((top + 1) * duty / 100.0 + 0.5));
    gpio_set_function(pin, GPIO_FUNC_PWM);
    pwm_set_enabled(slice, true);
    slice_freq[slice] = (float)freq;
    pin_use[pin] = PWM;
}

/// Starts the ADC, once.
static void adc_start(void) {
    static bool started;
    if (!started) {
        adc_init();
        started = true;
    }
}

int port_adc(int pin) {
    if (pin < ADC_FIRST || pin >= PIN_COUNT)
        return -1;
    adc_start();
    if (pin_use[pin] != ADC) {
        if (pin_use[pin] == PWM)
            port_pwm(pin, 0, 0);
        adc_gpio_init(pin);
        pin_use[pin] = ADC;
    }
    adc_select_input(pin - ADC_FIRST);
    return adc_read();
}

double port_temperature(void) {
    adc_start();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; ++i)  // the sensor is noisy
        sum += adc_read();
    double volts = sum / 16.0 * 3.3 / 4096;
    return 27 - (volts - 0.706) / 0.001721;  // RP2350 datasheet formula
}

void port_pins_list(void) {
    static const char *const names[] = {"", "in", "out", "in up", "in down", "PWM", "ADC"};
    for (int row = 0; row < PIN_COUNT / 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            int pin = col * (PIN_COUNT / 4) + row;
            const char *why = port_pin_reserved(pin);
            char what[24];
            if (pin == i2c_pins[0] || pin == i2c_pins[1])
                snprintf(what, sizeof what, "I2C%d %s", (pin / 2) & 1, pin == i2c_pins[0] ? "SDA" : "SCL");
            else if (pin == spi_pins[0] || pin == spi_pins[1] || pin == spi_pins[2])
                snprintf(what, sizeof what, "SPI%d %s", (pin / 8) & 1,
                         pin == spi_pins[0] ? "SCK" : pin == spi_pins[1] ? "MOSI" : "MISO");
            else if (pin == serial_tx || pin == serial_rx || pin == serial_cts || pin == serial_rts)
                snprintf(what, sizeof what, "COM1 %s", pin == serial_tx ? "TX" : pin == serial_rx ? "RX"
                                                       : pin == serial_cts ? "CTS" : "RTS");
            else if (why)
                snprintf(what, sizeof what, "%s", pin <= 1 ? "UART" : pin <= 19 ? "HDMI"
                                                  : pin <= 36 ? "WiFi" : "PSRAM");
            else if (pin_use[pin] == UNUSED)
                snprintf(what, sizeof what, "free%s", pin == LED_PIN ? " (LED)" : "");
            else if (pin_use[pin] == PWM) {
                char text[16];
                snprintf(what, sizeof what, "PWM %sHz",
                         freq_text(slice_freq[pwm_gpio_to_slice_num(pin)], text, sizeof text));
            }
            else if (pin_use[pin] == ADC)
                snprintf(what, sizeof what, "ADC");
            else
                snprintf(what, sizeof what, "%s %d", names[pin_use[pin]], gpio_get(pin));
            printf("GP%-2d %-14s%s", pin, what, col < 3 ? " " : "\n");
        }
    }
}

void port_i2c_close(void);
void port_spi_close(void);

void port_pins_reset(void) {
    port_i2c_close();
    port_spi_close();
    for (int pin = 0; pin < PIN_COUNT; ++pin) {
        if (pin_use[pin] == UNUSED)
            continue;
        if (pin_use[pin] == PWM)
            port_pwm(pin, 0, 0);
        gpio_init(pin);
        gpio_set_pulls(pin, false, false);
        pin_use[pin] = UNUSED;
    }
}

// ---- Serial port: COM1 is UART1 -----------------------------------------------
// Received bytes go into a ring buffer from the UART's interrupt, so none are
// lost while BASIC is busy.

#include "hardware/irq.h"
#include "hardware/uart.h"

/// The receive ring's size.
#define RX_SIZE 4096
static uint8_t *rx_buf;  ///< RX_SIZE, while COM1 is open (SRAM is short)
/// The ring's ends: the interrupt writes head, BASIC reads tail.
///@{
static volatile uint16_t rx_head, rx_tail;
///@}

/// UART1's interrupt: received bytes into the ring.
static void serial_irq(void) {
    while (uart_is_readable(uart1)) {
        uint16_t next = (rx_head + 1) % RX_SIZE;
        if (next == rx_tail) {
            // Full. With flow control, leave the rest in the UART so its
            // FIFO fills and RTS tells the sender to wait; reading resumes
            // when there's room. Without, the byte is lost either way.
            if (serial_rts >= 0) {
                uart_set_irq_enables(uart1, false, false);
                return;
            }
            uart_getc(uart1);
            continue;
        }
        rx_buf[rx_head] = uart_getc(uart1);
        rx_head = next;
    }
}

/// Whether a pin can be UART1's TX (or RX).
/// @param pin the pin
/// @param tx TX (else RX)
/// @return it can
static bool uart1_pin(int pin, bool tx) {
    // UART1's TX/RX pins below GP30, as on every RP2 chip.
    static const int8_t txs[] = {4, 8, 20, 24}, rxs[] = {5, 9, 21, 25};
    for (int i = 0; i < 4; ++i)
        if (pin == (tx ? txs[i] : rxs[i]))
            return true;
    return false;
}

const char *port_serial_open(unsigned baud, int data_bits, char parity, int stop_bits, int tx,
                             int rx) {
    if (!uart1_pin(tx, true) || !uart1_pin(rx, false))
        return "COM1 can use TX on GP4, 8, 20 or 24 and RX on GP5, 9, 21 or 25";
    const char *why = port_pin_reserved(tx);
    if (!why)
        why = port_pin_reserved(rx);
    if (why)
        return why;
    if (pin_use[tx] != UNUSED || pin_use[rx] != UNUSED)
        return "a pin is in use by PINMODE/PIN/PWM";
    if (!rx_buf && !(rx_buf = malloc(RX_SIZE)))
        return "not enough memory for COM1's buffer";
    uart_init(uart1, baud);
    uart_set_format(uart1, data_bits, stop_bits,
                    parity == 'E' ? UART_PARITY_EVEN : parity == 'O' ? UART_PARITY_ODD
                                                     : UART_PARITY_NONE);
    uart_set_hw_flow(uart1, false, false);
    uart_set_fifo_enabled(uart1, true);
    gpio_set_function(tx, GPIO_FUNC_UART);
    gpio_set_function(rx, GPIO_FUNC_UART);
    gpio_pull_up(rx);  // an unconnected input reads as idle, not as noise
    rx_head = rx_tail = 0;
    irq_set_exclusive_handler(UART1_IRQ, serial_irq);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(uart1, true, false);
    serial_tx = tx;
    serial_rx = rx;
    return NULL;
}

/// RTS/CTS flow control on the two pins after TX (TX+2 CTS in, TX+3 RTS out:
/// GP6/7 for GP4/5, and so on). NULL if done, else why not.
const char *port_serial_flow(bool on) {
    if (serial_tx < 0)
        return "COM1 isn't open";
    if (on && serial_cts < 0) {
        int cts = serial_tx + 2, rts = serial_tx + 3;
        const char *why = port_pin_reserved(cts);
        if (!why)
            why = port_pin_reserved(rts);
        if (why)
            return why;
        if (pin_use[cts] != UNUSED || pin_use[rts] != UNUSED)
            return "a flow control pin is in use by PINMODE/PIN/PWM";
        gpio_set_function(cts, GPIO_FUNC_UART);
        gpio_set_function(rts, GPIO_FUNC_UART);
        gpio_pull_down(cts);  // nothing connected: never clear to send
        serial_cts = cts;
        serial_rts = rts;
        uart_set_hw_flow(uart1, true, true);
    } else if (!on && serial_cts >= 0) {
        uart_set_hw_flow(uart1, false, false);
        gpio_init(serial_cts);
        gpio_init(serial_rts);
        gpio_set_pulls(serial_cts, false, false);
        serial_cts = serial_rts = -1;
        uart_set_irq_enables(uart1, true, false);
    }
    return NULL;
}

/// Holds TX low (a BREAK) for `ms` milliseconds.
void port_serial_break(unsigned ms) {
    if (serial_tx < 0)
        return;
    uart_tx_wait_blocking(uart1);
    uart_set_break(uart1, true);
    sleep_ms(ms);
    uart_set_break(uart1, false);
}

void port_serial_close(void) {
    if (serial_tx < 0)
        return;
    port_serial_flow(false);
    uart_set_irq_enables(uart1, false, false);
    irq_set_enabled(UART1_IRQ, false);
    irq_remove_handler(UART1_IRQ, serial_irq);
    uart_tx_wait_blocking(uart1);  // let the last bytes go out
    uart_deinit(uart1);
    gpio_init(serial_tx);
    gpio_init(serial_rx);
    gpio_set_pulls(serial_rx, false, false);
    serial_tx = serial_rx = -1;
    free(rx_buf);
    rx_buf = NULL;
    rx_head = rx_tail = 0;
}

void port_serial_write(const char *data, int n) {
    uart_write_blocking(uart1, (const uint8_t *)data, n);
}

int port_serial_getc(void) {
    if (rx_tail == rx_head)
        return -1;
    int c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_SIZE;
    if (serial_rts >= 0)
        uart_set_irq_enables(uart1, true, false);  // in case it stopped when full
    return c;
}

int port_serial_waiting(void) {
    return (rx_head - rx_tail + RX_SIZE) % RX_SIZE;
}

int port_serial_room(void) {
    return RX_SIZE - 1 - port_serial_waiting();
}

// ---- Pictures -----------------------------------------------------------------

#include "image.h"

const char *port_image(const char *path, int x, int y, int w, int h, int sx, int sy, bool fit,
                       int cx0, int cy0, int cx1, int cy1) {
    image_t img;
    const char *err;
    if (!image_load(path, w ? w : 640, h ? h : 480, &img, &err))
        return err;
    if (!w) {
        w = img.w * sx;
        h = img.h * sy;
    } else if (fit) {
        image_fit(&img, &x, &y, &w, &h);
    }
    image_clip_t clip = {cx0, cy0, cx1, cy1};
    image_draw(&img, draw_fb, x, y, w, h, &clip);
    image_free(&img);
    return NULL;
}

const char *port_save_image(const char *path) {
    const char *err;
    return image_save_bmp(path, dvi_framebuf, &err) ? NULL : err;
}

bool port_mouse(int *x, int *y, int *buttons) {
    mouse_get(x, y, buttons);
    return mouse_count() > 0;
}

int port_mouse_wheel(void) {
    return mouse_wheel();
}

/// PC (set 1) scan codes to USB HID usages, for KEYDOWN. Shift, Ctrl and Alt
/// (42/54, 29, 56) are modifier bits in the report instead.
static const uint8_t scan_to_hid[84] = {
    0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,     // 0-15
    20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 0, 4, 22,      // 16-31
    7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 0, 49, 29, 27, 6, 25,       // 32-47
    5, 17, 16, 54, 55, 56, 0, 85, 0, 44, 57, 58, 59, 60, 61, 62,      // 48-63
    63, 64, 65, 66, 67, 83, 71, 74, 82, 75, 86, 80, 93, 79, 87, 77,   // 64-79
    81, 78, 73, 76,                                                   // 80-83
};

bool port_key_down(int scan) {
    uint8_t keys[6], mods;
    bool present = usb_keyboard_held_keys(keys, &mods);
    if (scan == 0)
        return present;
    if (scan == 29)
        return mods & 0x11;  // either Ctrl
    if (scan == 42 || scan == 54)
        return mods & (scan == 42 ? 0x02 : 0x20);
    if (scan == 56)
        return mods & 0x44;  // either Alt
    if (scan < 0 || scan >= (int)sizeof scan_to_hid || !scan_to_hid[scan])
        return false;
    for (int i = 0; i < 6; ++i)
        if (keys[i] == scan_to_hid[scan])
            return true;
    return false;
}

void port_mouse_pointer(bool on) {
    mouse_pointer(on ? MOUSE_POINTER_TEXT : MOUSE_POINTER_OFF);
}

// ---- MUSIC: a MOD or XM song in the background ------------------------------

/// The song (PSRAM) plays on core 1 through the MOD or XM player (src/mod).
/// Its patterns and samples sit untouched in PSRAM for minutes, and this
/// board's PSRAM has had cells that forget after a minute or more: a timer
/// rewrites all of it a little at a time, over and over. (A flipped sample
/// bit would only click, so no checksums.)
static mod_song_t *music;   ///< a MOD, or
static xm_song_t *xmusic;   ///< an XM
/// Keeps the song's PSRAM fresh while it plays.
static repeating_timer_t music_timer;
/// It is running.
static bool music_timer_on;
/// The region being refreshed, and the volume (0-100).
///@{
static int music_part, music_volume = 100;
///@}
/// Where in the region.
static uint32_t music_off;
/// MUSIC is paused.
static bool music_paused;

#define MUSIC_REFRESH_BYTES 512  ///< each 5 ms: ~100 KB a second, a short stop each time

/// The song's k-th region of memory; false past the last.
/// @param k the region
/// @param base set to its start
/// @param len set to its length
/// @return there is one
static bool music_region(int k, uint8_t **base, uint32_t *len) {
    *base = NULL;
    *len = 0;
    if (music) {
        if (k == 0) {
            *base = music->patterns;
            *len = (uint32_t)music->npatterns * MOD_ROWS * music->channels * 4;
            return true;
        }
        if (k > MOD_MAX_SAMPLES)
            return false;
        *base = (uint8_t *)music->samples[k - 1].data;
        *len = *base ? music->samples[k - 1].length : 0;
        return true;
    }
    if (xmusic) {
        if (k < XM_MAX_PATTERNS) {
            *base = (uint8_t *)xmusic->patterns[k].cells;
            *len = *base ? xmusic->patterns[k].rows * xmusic->channels * sizeof(xm_cell_t) : 0;
            return true;
        }
        k -= XM_MAX_PATTERNS;
        for (int i = 0; i < xmusic->ninstruments; ++i) {
            xm_instrument_t *in = &xmusic->instruments[i];
            if (k < in->nsamples) {
                *base = (uint8_t *)in->samples[k].data;
                *len = *base ? in->samples[k].length * 2 : 0;
                return true;
            }
            k -= in->nsamples;
        }
    }
    return false;
}

/// The refresh timer: rewrites a piece of the song's memory.
/// @param t the timer
/// @return true (keep going)
static bool music_refresh(repeating_timer_t *t) {
    (void)t;
    uint8_t *base;
    uint32_t len;
    if (!music_region(music_part, &base, &len)) {
        music_part = 0;
        music_off = 0;
        return true;
    }
    uint32_t n = len > music_off ? len - music_off : 0;
    n = n < MUSIC_REFRESH_BYTES ? n : MUSIC_REFRESH_BYTES;
    if (base && n >= 4) {
        uintptr_t a = ((uintptr_t)base + music_off + 3) & ~(uintptr_t)3;
        volatile uint32_t *w = (volatile uint32_t *)(a - 0x11000000u + 0x15000000u);
        for (uint32_t i = 0; i + 1 < n / 4; ++i)
            w[i] = w[i];
    }
    music_off += n;
    if (music_off >= len) {
        music_off = 0;
        ++music_part;
    }
    return true;
}

void port_music_stop(void) {
    if (!music && !xmusic)
        return;
    if (music)
        mod_stop();
    if (xmusic)
        xm_stop();
    audio_set_source(NULL);
    mod_set_loop(true);
    mod_pause(false);
    xm_set_loop(true);
    xm_pause(false);
    sleep_ms(2);  // core 1 may be in the middle of a frame
    if (xmusic)
        xm_release();
    mod_song_t *s = music;
    xm_song_t *x = xmusic;
    music = NULL;
    xmusic = NULL;
    if (s) {
        mod_clear(s);
        psram_free(s);
    }
    if (x) {
        xm_clear(x);
        psram_free(x);
    }
}

const char *port_music_play(const char *path, bool loop) {
    port_music_stop();
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return "file not found";
    uint32_t size = f_size(&f);
    uint8_t *data = size && size <= (8u << 20) ? psram_malloc(size) : NULL;
    UINT got = 0;
    bool ok = data && f_read(&f, data, size, &got) == FR_OK && got == size;
    f_close(&f);
    if (!ok) {
        psram_free(data);
        return data ? "can't read the file" : "not enough memory";
    }
    const char *err;
    if (xm_is_xm(data, size)) {
        xm_song_t *x = psram_malloc(sizeof *x);
        err = x ? xm_load(x, data, size) : "not enough memory";
        psram_free(data);
        if (err) {
            if (x) {
                xm_clear(x);
                psram_free(x);
            }
            return err;
        }
        xip_cache_clean_all();  // all of it in the chip: the refresh reads it there
        xmusic = x;
    } else {
        mod_song_t *s = psram_malloc(sizeof *s);
        err = s ? mod_load(s, data, size) : "not enough memory";
        psram_free(data);
        if (err) {
            if (s) {
                mod_clear(s);
                psram_free(s);
            }
            return err;
        }
        xip_cache_clean_all();
        music = s;
    }
    music_part = 0;
    music_off = 0;
    if (!music_timer_on)
        music_timer_on = add_repeating_timer_ms(-5, music_refresh, NULL, &music_timer);
    music_paused = false;
    if (xmusic) {
        xm_set_loop(loop);
        xm_set_volume(music_volume);
        xm_pause(false);
        audio_set_source(xm_render_frame);
        if (!xm_play(xmusic, 0, 0)) {
            port_music_stop();
            return "not enough memory for the player";
        }
    } else {
        mod_set_loop(loop);
        mod_set_volume(music_volume);
        mod_pause(false);
        mod_set_tempo(6, 125);
        audio_set_source(mod_render);
        mod_play(music, 0, 0, false);
    }
    return NULL;
}

void port_music_pause(bool paused) {
    music_paused = paused;
    if (music)
        mod_pause(paused);
    if (xmusic)
        xm_pause(paused);
}

void port_music_volume(int percent) {
    music_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    mod_set_volume(music_volume);
    xm_set_volume(music_volume);
}

void port_music_pos(int order, int row) {
    if (music && order >= 0 && order < music->length)
        mod_play(music, order, row, false);
    if (xmusic && order >= 0 && order < xmusic->length)
        xm_play(xmusic, order, row);
}

int port_music_state(int what) {
    if (xmusic) {
        xm_status_t st;
        xm_get_status(&st);
        switch (what) {
        case 0: return st.playing && !music_paused ? -1 : 0;
        case 1: return st.order;
        case 2: return st.row;
        default: return st.pattern;
        }
    }
    if (!music)
        return 0;
    mod_status_t st;
    mod_get_status(&st);
    switch (what) {
    case 0: return st.state != MOD_STOPPED && !music_paused ? -1 : 0;
    case 1: return st.order;
    case 2: return st.row;
    default: return st.pattern;
    }
}

// ---- I2C and SPI ------------------------------------------------------------------
// The RP2350's own controllers. Which one a pin belongs to follows from its
// number: I2C SDA on even pins and SCL on odd ones, I2C0 or I2C1 by (pin / 2)
// mod 2; SPI in groups of four (MISO, CS, SCK, MOSI), SPI0 or SPI1 by
// (pin / 8) mod 2.

#include "hardware/i2c.h"
#include "hardware/spi.h"

/// The I2C controller open, or NULL.
static i2c_inst_t *i2c_bus;
/// The SPI controller open, or NULL.
static spi_inst_t *spi_bus;

/// Why a pin can't be used, or NULL.
/// @param pin the pin
/// @return the reason, or NULL
static const char *pin_free(int pin) {
    if (pin < 0 || pin >= PIN_COUNT)
        return "no such pin";
    const char *why = port_pin_reserved(pin);
    if (why)
        return why;
    if (pin_use[pin] != UNUSED)
        return "a pin is in use by PINMODE/PIN/PWM";
    return NULL;
}

const char *port_i2c_open(int sda, int scl, int khz) {
    port_i2c_close();
    if (sda < 0 || scl < 0 || sda >= PIN_COUNT || scl >= PIN_COUNT || (sda & 1) || scl != sda + 1)
        return "I2C needs SDA on an even pin and SCL on the next one (e.g. GP2, GP3)";
    const char *why = pin_free(sda);
    if (!why)
        why = pin_free(scl);
    if (why)
        return why;
    if (khz < 10 || khz > 1000)
        return "the speed must be 10-1000 kHz";
    i2c_bus = (sda / 2) & 1 ? i2c1 : i2c0;
    i2c_init(i2c_bus, khz * 1000);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);  // weak: devices should bring their own pull-ups
    gpio_pull_up(scl);
    i2c_pins[0] = sda;
    i2c_pins[1] = scl;
    return NULL;
}

void port_i2c_close(void) {
    if (!i2c_bus)
        return;
    i2c_deinit(i2c_bus);
    for (int i = 0; i < 2; ++i) {
        gpio_init(i2c_pins[i]);
        gpio_set_pulls(i2c_pins[i], false, false);
        i2c_pins[i] = -1;
    }
    i2c_bus = NULL;
}

/// Bytes written, or -1 if no device answered (or it stopped answering), -2
/// if I2C isn't open.
int port_i2c_write(int addr, const uint8_t *data, int n, bool nostop) {
    if (!i2c_bus)
        return -2;
    int r = i2c_write_timeout_us(i2c_bus, (uint8_t)addr, data, n, nostop, 2000 + n * 200);
    return r < 0 ? -1 : r;
}

int port_i2c_read(int addr, uint8_t *data, int n) {
    if (!i2c_bus)
        return -2;
    int r = i2c_read_timeout_us(i2c_bus, (uint8_t)addr, data, n, false, 2000 + n * 200);
    return r < 0 ? -1 : r;
}

const char *port_spi_open(int sck, int mosi, int miso, int khz, int mode) {
    port_spi_close();
    if (sck < 0 || mosi < 0 || miso < 0 || sck >= PIN_COUNT || mosi >= PIN_COUNT || miso >= PIN_COUNT ||
        sck % 4 != 2 || mosi % 4 != 3 || miso % 4 != 0 || ((sck / 8) & 1) != ((mosi / 8) & 1) ||
        ((sck / 8) & 1) != ((miso / 8) & 1))
        return "SPI needs SCK, MOSI and MISO of one SPI (e.g. GP10, GP11, GP8)";
    const char *why = pin_free(sck);
    if (!why)
        why = pin_free(mosi);
    if (!why)
        why = pin_free(miso);
    if (why)
        return why;
    if (khz < 1 || khz > 50000 || mode < 0 || mode > 3)
        return "the speed must be 1-50000 kHz and the mode 0-3";
    spi_bus = (sck / 8) & 1 ? spi1 : spi0;
    spi_init(spi_bus, khz * 1000);
    spi_set_format(spi_bus, 8, (mode & 2) ? SPI_CPOL_1 : SPI_CPOL_0, (mode & 1) ? SPI_CPHA_1 : SPI_CPHA_0,
                   SPI_MSB_FIRST);
    gpio_set_function(sck, GPIO_FUNC_SPI);
    gpio_set_function(mosi, GPIO_FUNC_SPI);
    gpio_set_function(miso, GPIO_FUNC_SPI);
    spi_pins[0] = sck;
    spi_pins[1] = mosi;
    spi_pins[2] = miso;
    return NULL;
}

void port_spi_close(void) {
    if (!spi_bus)
        return;
    spi_deinit(spi_bus);
    for (int i = 0; i < 3; ++i) {
        gpio_init(spi_pins[i]);
        gpio_set_pulls(spi_pins[i], false, false);
        spi_pins[i] = -1;
    }
    spi_bus = NULL;
}

/// Sends n bytes and keeps the n that came back (in may be NULL). False if
/// SPI isn't open.
bool port_spi_transfer(const uint8_t *out, uint8_t *in, int n) {
    if (!spi_bus)
        return false;
    if (in)
        spi_write_read_blocking(spi_bus, out, in, n);
    else
        spi_write_blocking(spi_bus, out, n);
    return true;
}
