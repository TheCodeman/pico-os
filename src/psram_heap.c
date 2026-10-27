/**
 * @file psram_heap.c
 * @brief Implementation of psram_heap.h: the Pico Plus 2's 8 MB PSRAM (APS6404L on QMI chip select 1, GPIO 47).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "psram_heap.h"

#include <stdlib.h>
#include <string.h>

#include "hardware/psram.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/sync.h"

/// PSRAM's address through the XIP cache.
#define PSRAM_CACHED   0x11000000u
/// PSRAM's address without the cache.
#define PSRAM_UNCACHED 0x15000000u

/// 126 MHz / 3. At 63 MHz (divider 2, the SDK's choice for this clock) reads
/// fail tens of thousands of times per 8 MB pass once the HDMI scanout DMA and
/// its interrupt are running, whatever the RX delay or bus priority; with
/// nothing else running they pass. Divider 3 passed every full-load test with
/// RX delay 2 to 4, and the SDK picks 3, the middle of that range.
#define PSRAM_MAX_FREQ (42 * MHZ)

/// First-fit allocator. Each block starts with a header; blocks tile the
/// region with no gaps, so the next block is always at hdr + size.
typedef struct {
    uint32_t size;  ///< whole block, header included, multiple of 8
    uint32_t used;  ///< the block is allocated
} block_t;

/// A block header's size.
#define HDR   sizeof(block_t)
/// Blocks' alignment.
#define ALIGN 8u

/// The heap's start and end in PSRAM.
///@{
static uint8_t *heap_start, *heap_end;
///@}
/// A mutex, not a critical section: walking the block list can take a
/// while, and interrupts (the HDMI scanout's above all) must go on meanwhile.
/// So none of this may be called from an interrupt.
static mutex_t lock;
static size_t used_bytes;  ///< in allocated blocks, headers included

size_t psram_setup(void) {
    if (!psram_is_available())
        return 0;

    // The SDK set the PSRAM timing up for the 150 MHz boot clock. At 126 MHz
    // the same max-select count would hold chip select past the chip's 8 us
    // refresh limit, so recalculate for the clock we run at now, and slow the
    // PSRAM clock down (see PSRAM_MAX_FREQ).
    uint32_t irq = save_and_disable_interrupts();
    psram_configure_params(PSRAM_MAX_FREQ, PICO_DEFAULT_PSRAM_MAX_SELECT,
                           PICO_DEFAULT_PSRAM_MIN_DESELECT);
    psram_reinitialize();
    restore_interrupts(irq);

    // Anything the linker placed in PSRAM (__in_psram) comes first.
    extern uint8_t __psram_end__;
    uintptr_t start = ((uintptr_t)&__psram_end__ + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    if (start < PSRAM_CACHED)
        start = PSRAM_CACHED;
    heap_start = (uint8_t *)start;
    heap_end = (uint8_t *)(PSRAM_CACHED + psram_get_size());

    mutex_init(&lock);
    block_t *first = (block_t *)heap_start;
    first->size = heap_end - heap_start;
    first->used = 0;
    return psram_get_size();
}

void *psram_test(void *start, size_t len) {
    uint32_t *p = start;
    size_t words = len / 4;
    // Through the uncached window both ways: going through the XIP cache
    // would push core 1's code (USB, the sound) out of it, and the sound
    // would stall while flash refilled it.
    volatile uint32_t *q = (volatile uint32_t *)((uintptr_t)start - PSRAM_CACHED + PSRAM_UNCACHED);
    // Address-dependent pattern so a stuck or aliased address line shows up.
    for (size_t i = 0; i < words; ++i)
        q[i] = (uint32_t)(uintptr_t)&p[i] * 2654435761u ^ 0x5a5aa5a5u;
    for (size_t i = 0; i < words; ++i)
        if (q[i] != ((uint32_t)(uintptr_t)&p[i] * 2654435761u ^ 0x5a5aa5a5u))
            return &p[i];
    return NULL;
}

void *psram_malloc(size_t size) {
    if (!heap_start || size == 0 || size > (size_t)(heap_end - heap_start))
        return NULL;
    uint32_t need = (size + HDR + ALIGN - 1) & ~(ALIGN - 1);
    void *result = NULL;
    mutex_enter_blocking(&lock);
    for (uint8_t *b = heap_start; b < heap_end; b += ((block_t *)b)->size) {
        block_t *blk = (block_t *)b;
        if (blk->used || blk->size < need)
            continue;
        if (blk->size - need >= HDR + ALIGN) {  // split off the rest
            block_t *rest = (block_t *)(b + need);
            rest->size = blk->size - need;
            rest->used = 0;
            blk->size = need;
        }
        blk->used = 1;
        used_bytes += blk->size;
        result = b + HDR;
        break;
    }
    mutex_exit(&lock);
    return result;
}

void psram_free(void *p) {
    if (!p)
        return;
    mutex_enter_blocking(&lock);
    block_t *freed = (block_t *)((uint8_t *)p - HDR);
    freed->used = 0;
    used_bytes -= freed->size;
    // Merge every run of free blocks.
    for (uint8_t *b = heap_start; b < heap_end;) {
        block_t *blk = (block_t *)b;
        uint8_t *next = b + blk->size;
        if (!blk->used && next < heap_end && !((block_t *)next)->used)
            blk->size += ((block_t *)next)->size;
        else
            b = next;
    }
    mutex_exit(&lock);
}

size_t psram_block_size(const void *p) {
    return ((const block_t *)((const uint8_t *)p - HDR))->size - HDR;
}

void *psram_realloc(void *p, size_t size) {
    if (!p)
        return psram_malloc(size);
    if (!size) {
        psram_free(p);
        return NULL;
    }
    size_t old = ((block_t *)((uint8_t *)p - HDR))->size - HDR;
    if (old >= size)
        return p;
    void *n = psram_malloc(size);
    if (!n)
        return NULL;
    memcpy(n, p, old);
    psram_free(p);
    return n;
}

bool psram_present(void) {
    return heap_end > heap_start;
}

size_t psram_free_total(void) {
    return heap_start ? (size_t)(heap_end - heap_start) - used_bytes : 0;
}

void psram_get_stats(psram_stats_t *s) {
    *s = (psram_stats_t){0};
    if (!heap_start)
        return;
    s->total = heap_end - heap_start;
    mutex_enter_blocking(&lock);
    for (uint8_t *b = heap_start; b < heap_end; b += ((block_t *)b)->size) {
        block_t *blk = (block_t *)b;
        if (blk->used) {
            s->used += blk->size;
            ++s->blocks;
        } else if (blk->size - HDR > s->largest) {
            s->largest = blk->size - HDR;
        }
    }
    mutex_exit(&lock);
}

// ---- Scrubbing ---------------------------------------------------------------
// This board's PSRAM has a few weak cells that lose their data after a minute
// or so. Scrubbing reads every word of a region and writes it straight back,
// through the uncached window so it reaches the chip, often enough that a weak
// cell is rewritten before it fades. It runs from a timer interrupt on core 0;
// a word is read and written inside the interrupt, so code on core 0 can't
// change it in between, and nothing else writes PSRAM.

#include "pico/time.h"

/// How often the scrubber runs a chunk.
#define SCRUB_TICK_MS 10

static volatile uint32_t *scrub_base;  ///< uncached alias
/// The scrubber's region (words), chunk per tick and place.
///@{
static volatile size_t scrub_words, scrub_chunk, scrub_pos;
///@}
/// Full passes the scrubber has made.
static volatile uint32_t scrub_passes;
/// The scrubber's timer.
static repeating_timer_t scrub_timer;
/// The scrubber's timer is running.
static bool scrub_timer_on;

/// Rewrites the next chunk of the scrubbed region.
/// @param t the timer
/// @return true: keep going
static bool scrub_tick(repeating_timer_t *t) {
    size_t words = scrub_words;
    if (!words)
        return true;
    volatile uint32_t *base = scrub_base;
    size_t pos = scrub_pos;
    for (size_t n = scrub_chunk; n; --n) {
        base[pos] = base[pos];
        if (++pos == words) {
            pos = 0;
            ++scrub_passes;
        }
    }
    scrub_pos = pos;
    return true;
}

void psram_scrub(void *start, size_t len, unsigned period_s) {
    scrub_words = 0;  // stop while changing
    if (!start || !len || !period_s)
        return;
    scrub_base = (volatile uint32_t *)((uintptr_t)start - PSRAM_CACHED + PSRAM_UNCACHED);
    scrub_pos = 0;
    size_t words = len / 4;
    size_t ticks = period_s * 1000 / SCRUB_TICK_MS;
    scrub_chunk = words / ticks + 1;
    scrub_words = words;
    if (!scrub_timer_on)
        scrub_timer_on = add_repeating_timer_ms(-SCRUB_TICK_MS, scrub_tick, NULL, &scrub_timer);
}

uint32_t psram_scrub_passes(void) {
    return scrub_passes;
}

void *psram_buffer_alloc(size_t size) {
    if (heap_end > heap_start)
        return psram_malloc(size);
    return size <= 64 * 1024 ? malloc(size) : NULL;
}

void psram_buffer_free(void *p) {
    if ((uintptr_t)p >= 0x11000000u && (uintptr_t)p < 0x16000000u)
        psram_free(p);
    else
        free(p);
}
