/// @file psram_heap.h
/// @brief PC stand-in for the PSRAM heap (web browser tests).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
extern bool pc_psram_full;  ///< the test's switch: psram_malloc and psram_realloc fail
static inline void *psram_malloc(size_t n) { return pc_psram_full ? NULL : malloc(n); }  ///< malloc
static inline void psram_free(void *p) { free(p); }  ///< free
static inline void *psram_realloc(void *p, size_t n) { return pc_psram_full ? NULL : realloc(p, n); }  ///< realloc
static inline size_t psram_free_total(void) { return 8u << 20; }  ///< 8 MB free
