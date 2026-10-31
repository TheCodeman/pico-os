/**
 * @file serial_console.c
 * @brief Implementation of serial_console.h: the debug UART (UART0, GP0 TX / GP1 RX) as a stdio driver with interrupt-driven buffers, so a fast paste isn't lost while the system is busy (the UART itself only holds 32 received characters).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "serial_console.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"

/// The debug UART (GP0/GP1).
#define UART     uart0
#define RX_SIZE  4096  ///< powers of two
/// Bytes waiting to be sent (room for a whole 80 x 30 screen drawn at once).
#define TX_SIZE  4096

/// The receive and send rings.
///@{
static uint8_t rx_buf[RX_SIZE], tx_buf[TX_SIZE];
///@}
/// The rings' write and read counts.
///@{
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;
///@}
/// Characters dropped because the receive ring was full.
static volatile unsigned rx_dropped;
/// Guards the send ring between cores.
static spin_lock_t *lock;
/// stdio's callback for characters arriving.
static void (*chars_available)(void *);
/// Passed to it.
static void *chars_available_param;

/// Moves queued output into the UART's FIFO. Called with `lock` held.
static void tx_fill(void) {
    while (tx_tail != tx_head && uart_is_writable(UART))
        uart_get_hw(UART)->dr = tx_buf[tx_tail++ % TX_SIZE];
    if (tx_tail != tx_head)
        hw_set_bits(&uart_get_hw(UART)->imsc, UART_UARTIMSC_TXIM_BITS);
    else
        hw_clear_bits(&uart_get_hw(UART)->imsc, UART_UARTIMSC_TXIM_BITS);
}

/// The UART's interrupt: received characters into the ring, and more sent.
static void uart_irq(void) {
    bool got = false;
    while (uart_is_readable(UART)) {
        uint8_t c = uart_get_hw(UART)->dr;
        if (rx_head - rx_tail < RX_SIZE)
            rx_buf[rx_head++ % RX_SIZE] = c;
        else
            ++rx_dropped;
        got = true;
    }
    uint32_t save = spin_lock_blocking(lock);
    tx_fill();
    spin_unlock(lock, save);
    if (got && chars_available)
        chars_available(chars_available_param);
}

/// stdio driver: queues characters to send, waiting for room when the ring is full.
/// @param buf the characters
/// @param len how many
static void serial_out_chars(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        uint32_t save = spin_lock_blocking(lock);
        // Full: wait for room by feeding the UART directly, which also works
        // with interrupts off (e.g. a panic message).
        while (tx_head - tx_tail >= TX_SIZE) {
            tx_fill();
            if (tx_head - tx_tail >= TX_SIZE) {
                spin_unlock(lock, save);
                tight_loop_contents();
                save = spin_lock_blocking(lock);
            }
        }
        tx_buf[tx_head++ % TX_SIZE] = buf[i];
        tx_fill();
        spin_unlock(lock, save);
    }
}

bool serial_console_write_some(const char *buf, int len) {
    uint32_t save = spin_lock_blocking(lock);
    bool room = TX_SIZE - (tx_head - tx_tail) >= (uint32_t)len;
    if (room) {
        for (int i = 0; i < len; i++)
            tx_buf[tx_head++ % TX_SIZE] = buf[i];
        tx_fill();
    }
    spin_unlock(lock, save);
    return room;
}

/// stdio driver's flush, which the SDK calls after every printf: starts
/// sending what is queued but doesn't wait for it. Waiting there held every
/// program's output to the UART's speed (a full screen of text took 0.2 s
/// at 115200 baud, even on HDMI); the queue sends it on by interrupt.
static void serial_out_flush(void) {
    uint32_t save = spin_lock_blocking(lock);
    tx_fill();
    spin_unlock(lock, save);
}

void serial_console_drain(void) {
    while (tx_head != tx_tail) {
        uint32_t save = spin_lock_blocking(lock);
        tx_fill();
        spin_unlock(lock, save);
    }
    uart_tx_wait_blocking(UART);
}

/// stdio driver: characters received.
/// @param buf where they go
/// @param len room in buf
/// @return how many, or PICO_ERROR_NO_DATA
static int serial_in_chars(char *buf, int len) {
    int n = 0;
    while (n < len && rx_tail != rx_head)
        buf[n++] = rx_buf[rx_tail++ % RX_SIZE];
    return n ? n : PICO_ERROR_NO_DATA;
}

/// stdio driver: sets the function called when characters arrive.
/// @param fn the function
/// @param param passed to it
static void serial_set_callback(void (*fn)(void *), void *param) {
    chars_available = fn;
    chars_available_param = param;
}

stdio_driver_t serial_stdio_driver = {
    .out_chars = serial_out_chars,
    .out_flush = serial_out_flush,
    .in_chars = serial_in_chars,
    .set_chars_available_callback = serial_set_callback,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

void serial_console_init(void) {
    stdio_set_driver_enabled(&stdio_uart, false);
    lock = spin_lock_instance(spin_lock_claim_unused(true));
    uart_get_hw(UART)->rsr = 0;  // clears old overrun flags
    // Interrupt once 4 characters are in, or one has waited a few character times.
    uart_set_fifo_enabled(UART, true);
    irq_set_exclusive_handler(UART0_IRQ, uart_irq);
    irq_set_priority(UART0_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UART, true, false);
    stdio_set_driver_enabled(&serial_stdio_driver, true);
}

unsigned serial_console_overruns(void) {
    return rx_dropped;
}

void debug_write(const char *text, int len) {
    for (int i = 0; i < len; ++i) {
        if (text[i] == '\n')
            serial_out_chars("\r", 1);
        serial_out_chars(&text[i], 1);
    }
}

void debug_printf(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    debug_write(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}
