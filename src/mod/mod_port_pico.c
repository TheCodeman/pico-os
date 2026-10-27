/**
 * @file mod_port_pico.c
 * @brief The MOD player's memory on the board: PSRAM.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "mod.h"
#include "xm.h"
#include "psram_heap.h"

void *mod_alloc(size_t size) {
    return psram_malloc(size);
}

void mod_free(void *p) {
    psram_free(p);
}

void *xm_alloc(size_t size) {
    return psram_malloc(size);
}

void xm_free(void *p) {
    psram_free(p);
}
