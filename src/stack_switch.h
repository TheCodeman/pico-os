/**
 * @file stack_switch.h
 * @brief Running code on a stack of its own (core 0's normal stack is only 4 KB).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>

/// Calls fn(arg) on the `size`-byte stack at `stack`, with the hardware stack
/// limit (MSPLIM) set to its bottom so an overflow faults instead of
/// corrupting memory. Both are restored afterwards.
/// @param fn the function
/// @param arg its argument
/// @param stack the stack's lowest address
/// @param size its size in bytes
void call_on_stack(void (*fn)(void *), void *arg, void *stack, size_t size);

/// Bottom of the stack the caller is running on, for overflow checks.
/// @return the stack's lowest address
char *stack_bottom(void);
