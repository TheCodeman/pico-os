/**
 * @file as_cpu.h
 * @brief A 65C02 as fitted to the enhanced Apple IIe (no Rockwell bit instructions; their opcodes are NOPs), with cycle counts.
 *
 * Applesoft uses it for CALL, USR and & into machine code, and the host test
 * harness to run the real ROM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The 65C02 that runs machine code (CALL, USR, &) inside Applesoft.
typedef struct as_cpu {
    uint16_t pc;  ///< program counter
    uint8_t a, x, y, s, p;  ///< accumulator, index registers, stack pointer and status
    uint32_t cycles;  ///< running total, wraps
    bool irq;         ///< the IRQ line, held by whatever wants service
    /// Memory. ctx is passed back unchanged.
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t value);  ///< writes a byte of memory
    void *ctx;  ///< passed to read and write
} as_cpu;

/// Status register bits.
enum {
    AS_C = 0x01, AS_Z = 0x02, AS_I = 0x04, AS_D = 0x08,
    AS_B = 0x10, AS_U = 0x20, AS_V = 0x40, AS_N = 0x80,
};

/// Loads the reset vector, sets S and P as a reset does.
void as_cpu_reset(as_cpu *c);

/// Runs one instruction, or takes the interrupt if the IRQ line is held and
/// interrupts are enabled; returns the cycles it took.
int as_cpu_step(as_cpu *c);

/// Pushes a byte (for setting up a call's return address).
void as_cpu_push(as_cpu *c, uint8_t v);
