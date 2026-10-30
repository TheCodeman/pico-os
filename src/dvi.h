/**
 * @file dvi.h
 * @brief 640x480 @ 60 Hz HDMI output (video plus audio) through the RP2350 HSTX peripheral.
 *
 * Wired for the Pico DVI Sock (GPIO 12-19). Pixels are RGB332, one byte
 * each. Audio is fed through hdmi_audio_push().
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The HDMI picture's width, in pixels.
#define DVI_WIDTH  640
/// The HDMI picture's height, in pixels.
#define DVI_HEIGHT 480

/// RGB332 helper: r and g are 0..7, b is 0..3.
#define RGB332(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

/// The framebuffer: 640x480, one RGB332 byte per pixel.
extern uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];

/// While set, each framebuffer row is shown on two lines: the picture is the
/// first 640x240 of dvi_framebuf, and the second half is free for other use
/// (the Apple IIe keeps its memory there). Clear the whole framebuffer after
/// clearing the flag.
extern volatile bool dvi_line_double;
/// And with it: only framebuffer rows 0 to dvi_double_rows - 1 are shown,
/// starting dvi_double_top rows down the 240; the lines above and below are
/// black and the rest of the framebuffer is free too. The defaults (0 and
/// 240) show the whole first half; set them back after changing them.
/// The lines drawn twice (from the top, how many): a 320x240 picture shown at 640x480.
///@{
extern volatile uint16_t dvi_double_top, dvi_double_rows;
///@}
/// And with it: each pixel is shown twice across too. Framebuffer rows are
/// then 320 bytes (row r at dvi_framebuf + r * 320), so the picture takes a
/// quarter of the framebuffer and the rest is free. Clear it after setting
/// it back.
extern volatile bool dvi_pixel_double;

/// The mouse arrow, laid over the picture as each line is sent: it never
/// touches dvi_framebuf, so drawing under it can't leave a ghost of it and
/// it doesn't flicker. Its tip is at framebuffer pixel (x, y).
extern volatile bool dvi_arrow_on;
/// Where the video output draws the mouse arrow (its tip, in pixels).
///@{
extern volatile int16_t dvi_arrow_x, dvi_arrow_y;
///@}

/// Counts pictures: goes up by one as the vertical sync starts, while the
/// lines being sent are blank. The first framebuffer row is read about 1 ms
/// later and each row after it 32 us (64 us when lines are doubled) after
/// the one before, so a program drawing in a buffer of its own can wait for
/// the count to change and copy its picture in without tearing.
extern volatile uint32_t dvi_frames;

/// Starts scanning out dvi_framebuf. Expects clk_sys (and so clk_hstx) at
/// 126 MHz, which gives the exact 25.2 MHz pixel clock for 640x480.
void dvi_init(void);

/// Turns the HDMI signal off for two seconds and starts it again, so the
/// display locks onto it afresh: now and then one locks badly after a reset
/// and shows a red screen full of lines. Ctrl-Alt-R on the USB keyboard.
/// Safe from either core and from interrupts; the work is done later.
void dvi_relock(void);

/// How often the scanout has had to be restarted (and how many of those were
/// because an interrupt came too late), and the longest gap seen between the
/// scanout's interrupts in microseconds (normally 190, one per six lines).
/// Also the longest the interrupt took to build its six lines.
/// Counters for testing (read over SWD): restarts, late lines, the worst gap and line build time.
///@{
extern volatile uint32_t dvi_restarts, dvi_late, dvi_worst_gap_us, dvi_worst_build_us;
///@}
