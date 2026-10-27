/// @file psram_heap.h
/// @brief PC stand-in for the PSRAM heap (si5351 GUI tests).
#pragma once
#include <stdlib.h>
static inline void *psram_malloc(size_t n) { return malloc(n); }  ///< malloc
static inline void psram_free(void *p) { free(p); }               ///< free
