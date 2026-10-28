/// @file dvi.h
/// @brief PC stand-in for the screen (SDR tests).
#pragma once
#include <stdint.h>
#define DVI_WIDTH 640   ///< the screen's width
#define DVI_HEIGHT 480  ///< its height
#define RGB332(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))  ///< a colour
extern uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
