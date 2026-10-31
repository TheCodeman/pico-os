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
/// The UART then carries debug_printf's messages only, until
/// serial_console_enable(true) or a key typed there.
void serial_console_init(void);

/// The shell's output on the debug UART too (the shell's sercon command):
/// off at start-up, as every character sent there costs time on the screen
/// too. Keys typed there always reach the shell, and until this is called the
/// first one turns it on.
/// @param on on or off
void serial_console_enable(bool on);
/// Whether the serial console is on.
/// @return it is
bool serial_console_enabled(void);

/// Waits until everything queued for the debug UART has been sent (stdio's
/// flush doesn't wait): before a restart, so the last messages get out.
void serial_console_drain(void);

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
/// none of it (false); never waits. With the serial console off, nothing is
/// sent (true).
/// @param buf the bytes
/// @param len how many
/// @return false if there wasn't room for all of them
bool serial_console_write_some(const char *buf, int len);
