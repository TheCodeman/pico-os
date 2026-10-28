/// @file psram_heap.h
/// @brief PC stand-in for the PSRAM heap (WAV editor tests).
#pragma once
#include <stdlib.h>
extern size_t pc_psram_limit;  ///< uitest.c: bytes "free" (for running out)
void *psram_malloc(size_t n);                ///< malloc, within the limit
void psram_free(void *p);                    ///< free
void *psram_realloc(void *p, size_t n);      ///< realloc, within the limit
size_t psram_free_total(void);               ///< what's left
