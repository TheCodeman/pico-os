/**
 * @file stack_switch.c
 * @brief Implementation of stack_switch.h: running code on a stack of its own (core 0's normal stack is only 4 KB).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "stack_switch.h"

#include <stdint.h>

extern char __StackBottom;  ///< core 0's normal stack, from the linker script
/// volatile: the compiler otherwise drops the store before the switch.
static char *volatile bottom = &__StackBottom;

char *stack_bottom(void) {
    return bottom;
}

/// Calls fn(arg) on another stack, from low to top, setting MSPLIM to match.
/// @param fn the function
/// @param arg its argument
/// @param low the stack's bottom
/// @param top its top
static void __attribute__((noinline)) switch_and_call(void (*fn)(void *), void *arg, char *low,
                                                      char *top) {
    __asm volatile(
        "mov r4, sp\n"
        "mrs r5, msplim\n"
        "msr msplim, %[low]\n"
        "mov sp, %[top]\n"
        "mov r0, %[arg]\n"
        "blx %[fn]\n"
        "mov sp, r4\n"
        "msr msplim, r5\n"
        :
        : [low] "r"(low), [top] "r"(top), [arg] "r"(arg), [fn] "r"(fn)
        : "r0", "r1", "r2", "r3", "r4", "r5", "r12", "lr", "memory", "cc");
}

void call_on_stack(void (*fn)(void *), void *arg, void *stack, size_t size) {
    char *low = stack;
    char *top = (char *)(((uintptr_t)low + size) & ~(uintptr_t)7);
    char *saved = bottom;
    bottom = low;
    switch_and_call(fn, arg, low, top);
    bottom = saved;
}
