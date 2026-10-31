/**
 * @file sync.h
 * @brief PC stand-in for the SDK's hardware/sync.h: __dmb does nothing.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
/// A memory barrier (one core on a PC: nothing).
static inline void __dmb(void) {}
