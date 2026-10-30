/// @file psram_heap.h
/// @brief PC stand-in for the PSRAM heap (IRC client tests).
#pragma once
#include <stdbool.h>
#include <stdlib.h>
extern bool pc_psram_full;  ///< uitest.c: psram_malloc and psram_realloc fail (for running out)
static inline void *psram_malloc(size_t n) { return pc_psram_full ? NULL : malloc(n); }  ///< malloc
static inline void psram_free(void *p) { free(p); }  ///< free
static inline void *psram_realloc(void *p, size_t n) { return pc_psram_full ? NULL : realloc(p, n); }  ///< realloc
static inline bool psram_present(void) { return true; }  ///< there is some
static inline void *psram_buffer_alloc(size_t n) { return psram_malloc(n); }  ///< a passing buffer
static inline void psram_buffer_free(void *p) { free(p); }  ///< frees one
static inline size_t psram_free_total(void) { return 8u << 20; }  ///< bytes free
