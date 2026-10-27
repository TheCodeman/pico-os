/**
 * @file usb_log.h
 * @brief USB diagnostics.
 *
 * Core 1 (TinyUSB and our USB callbacks) writes messages into a queue; core
 * 0 prints them, so both cores never draw on the console at once.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// Core 0, before core 1 starts.
void usb_log_init(void);
/// Moves the log's buffer to PSRAM (once the boot check of it is done).
void usb_log_use_psram(void);

/// Any core. Also used as TinyUSB's debug printf (CFG_TUSB_DEBUG_PRINTF).
/// @param format printf's format, and its arguments
/// @return the characters written
int usb_log_printf(const char *format, ...);

/// Core 0: next queued character, or -1 if none.
/// @return the character, or -1
int usb_log_getc(void);
