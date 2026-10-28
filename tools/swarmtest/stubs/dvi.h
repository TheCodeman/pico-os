/// @file dvi.h
/// @brief PC stand-in for the screen (swarm tests): the framebuffer, the doubling flags, and a frame count that moves each time it is read.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define DVI_WIDTH 640   ///< the screen's width
#define DVI_HEIGHT 480  ///< its height
#define RGB332(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))  ///< a colour
extern uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
extern volatile bool dvi_line_double, dvi_pixel_double;  ///< doubling
extern volatile uint16_t dvi_double_top, dvi_double_rows;  ///< the rows shown
uint32_t pc_frames(void);       ///< the frame count, one more each read
#define dvi_frames pc_frames()  ///< so waiting for it to change never waits
