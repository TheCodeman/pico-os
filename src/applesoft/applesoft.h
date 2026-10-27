/**
 * @file applesoft.h
 * @brief An Applesoft BASIC interpreter, after the enhanced Apple IIe's.
 *
 * the same tokenised program format, 40-bit floating point, memory map, text
 * and graphics screens, with DOS 3.3 style file commands on the USB drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/// Runs the ] prompt until BYE. If `file` is given it is loaded and run first.
void applesoft_main(const char *file);

/// If set, called with the emulated 64 KB just before applesoft_main returns
/// (the PC test harness dumps memory with it).
extern void (*applesoft_on_exit)(const uint8_t *ram);
