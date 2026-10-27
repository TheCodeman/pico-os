/**
 * @file c64_cpu.h
 * @brief The C64's 6510.
 *
 * an NMOS 6502 with every undocumented opcode (the stable ones as the chip
 * does them, the unstable ones as most emulators agree), NMOS decimal mode
 * flags, and cycle counts per instruction (with page crossings and taken
 * branches). The $00/$01 I/O port lives in the machine (c64.c), which sees
 * every access through read/write.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The C64's 6510 CPU: registers and cycle count.
typedef struct c64_cpu {
    uint16_t pc;  ///< program counter
    /// accumulator, index registers, stack pointer and status
    ///@{
    uint8_t a, x, y, s, p;
    ///@}
    bool irq;         ///< the IRQ line (level), held by whatever wants service
    bool nmi;         ///< an NMI edge waiting to be taken
    bool jammed;      ///< a JAM opcode stopped the CPU; only reset restarts it
    /// Memory. ctx is passed back unchanged.
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t value);  ///< writes a byte of memory
    void *ctx;  ///< passed to read and write
    /// Optional: where each 256-byte page reads from and writes to directly;
    /// a NULL page (or NULL tables) goes through read/write. Writes to $0000
    /// and $0001 always go through write.
    const uint8_t *const *rpage;
    uint8_t *const *wpage;  ///< each 256-byte page's memory for writing (NULL: through write)
} c64_cpu;

/// Status register bits.
enum {
    C64_C = 0x01, C64_Z = 0x02, C64_I = 0x04, C64_D = 0x08,
    C64_B = 0x10, C64_U = 0x20, C64_V = 0x40, C64_N = 0x80,
};

/// Loads the reset vector, sets S and P as a reset does.
/// @param c the CPU
void c64_cpu_reset(c64_cpu *c);

/// Runs one instruction, or takes an NMI, or the IRQ if the line is held and
/// interrupts are enabled; returns the cycles it took (2 while jammed, so
/// time still passes).
/// @param c the CPU
/// @return the cycles it took
int c64_cpu_step(c64_cpu *c);
