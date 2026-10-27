/**
 * @file serial_console.h
 * @brief The debug UART (UART0, GP0 TX / GP1 RX) as a stdio driver with interrupt-driven buffers, so a fast paste isn't lost while the system is busy (the UART itself only holds 32 received characters).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include "pico/stdio/driver.h"

/// The stdio driver for the debug UART (GP0/GP1).
extern stdio_driver_t serial_stdio_driver;

/// Takes over UART0 from the SDK's stdio_uart driver. Call after stdio_init_all().
void serial_console_init(void);

/// Characters dropped because the receive buffer was full.
/// @return the characters dropped
unsigned serial_console_overruns(void);

/// Detail for the debug UART only (not the screen): start-up details, USB
/// and WiFi chatter.
/// @param format printf's format, and its arguments
void debug_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
/// Writes text to the debug UART only.
/// @param text the text
/// @param len its length
void debug_write(const char *text, int len);

/// Queues all of `buf` for the debug UART if there's room for all of it, else
/// none of it (false); never waits.
/// @param buf the bytes
/// @param len how many
/// @return false if there wasn't room for all of them
bool serial_console_write_some(const char *buf, int len);
