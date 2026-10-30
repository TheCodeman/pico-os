/// @file dvi.h
/// @brief PC stand-in for the screen (web browser tests).
#pragma once
#include <stdint.h>
#define DVI_WIDTH 640   ///< the screen's width
#define DVI_HEIGHT 480  ///< its height
extern uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
