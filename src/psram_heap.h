/**
 * @file psram_heap.h
 * @brief The Pico Plus 2's 8 MB PSRAM (APS6404L on QMI chip select 1, GPIO 47).
 *
 * The SDK's hardware_psram detects and maps it at boot (0x11000000 cached,
 * 0x15000000 uncached); this module retunes it for our 126 MHz clock, tests
 * it, and hands it out with psram_malloc().
 *
 * Keep the framebuffer, scanout buffers and anything an IRQ touches in SRAM:
 * PSRAM goes through the 16 KB XIP cache shared with flash, so a miss can
 * take a microsecond or more.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Core 0, right after the system clock is set and before core 1 starts.
/// Returns the PSRAM size in bytes, or 0 if there is none.
size_t psram_setup(void);

/// Writes a pattern over [start, start + len) through the cached window and
/// reads it back through the uncached one. Returns the first bad address, or
/// NULL if it all matched. The memory's contents are lost.
void *psram_test(void *start, size_t len);

/// Any core. 8-byte aligned blocks; NULL if PSRAM is missing or full.
void *psram_malloc(size_t size);
/// Frees a block from psram_malloc.
void psram_free(void *p);
/// A big buffer that needn't be fast: from PSRAM (NULL if it's full). With
/// no PSRAM, from the heap, but only up to 64 KB, as malloc panics when SRAM
/// runs out. Free with psram_buffer_free.
void *psram_buffer_alloc(size_t size);
/// Frees a block from psram_buffer_alloc.
void psram_buffer_free(void *p);
void *psram_realloc(void *p, size_t size);  ///< as realloc; NULL (p kept) if no room
size_t psram_block_size(const void *p);     ///< usable bytes of a psram_malloc block

/// The PSRAM heap's figures (psram_get_stats).
typedef struct {
    size_t total;    ///< bytes managed by psram_malloc
    size_t used;     ///< bytes in allocated blocks, including headers
    size_t largest;  ///< biggest block psram_malloc can return right now
    int blocks;      ///< allocated blocks
} psram_stats_t;
void psram_get_stats(psram_stats_t *stats);  ///< walks the whole heap: not for hot paths
/// True if PSRAM was found at start-up.
bool psram_present(void);
size_t psram_free_total(void);  ///< bytes not in allocated blocks (quick)

/// Keeps [start, start + len) fresh by rewriting every word of it every
/// period_s seconds in the background (see psram_heap.c). len 0 stops it.
void psram_scrub(void *start, size_t len, unsigned period_s);
uint32_t psram_scrub_passes(void);  ///< full passes done so far
