/**
 * @file fnt_stb.h
 * @brief How stb_truetype is built here: its working buffers from fnt.c's
 *        arena, single-precision maths (the RP2350's FPU has no doubles),
 *        no asserts, and small chunks for the active edges.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <math.h>
#include <stddef.h>

/// stb_truetype's malloc: from the open font's SRAM arena, else PSRAM (fnt.c).
/// @param n bytes wanted
/// @param user the font (fnt_t)
/// @return the block, or NULL
void *fnt_stb_alloc(size_t n, void *user);
/// stb_truetype's free.
/// @param p the block (NULL is fine)
/// @param user the font (fnt_t)
void fnt_stb_free(void *p, void *user);

/// @cond STB_CONFIG
#define STBTT_malloc(x, u) fnt_stb_alloc(x, u)
#define STBTT_free(x, u)   fnt_stb_free(x, u)
#define STBTT_ifloor(x)    ((int)floorf(x))
#define STBTT_iceil(x)     ((int)ceilf(x))
#define STBTT_sqrt(x)      sqrtf(x)
#define STBTT_pow(x, y)    powf(x, y)
#define STBTT_fmod(x, y)   fmodf(x, y)
#define STBTT_cos(x)       cosf(x)
#define STBTT_acos(x)      acosf(x)
#define STBTT_fabs(x)      fabsf(x)
#define STBTT_assert(x)    ((void)0)
#define STBTT_HHEAP_MID    64  // 64 active edges a chunk (1.8 KB), not 800
/// @endcond

#include "stb_truetype.h"
