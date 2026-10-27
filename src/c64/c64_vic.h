/**
 * @file c64_vic.h
 * @brief The PAL VIC-II (6569), drawn a line at a time (in pieces where registers change part way along it).
 *
 * the text and bitmap modes (standard, multicolour, extended background
 * colour), smooth scrolling, the 24/25-row and 38/40-column borders, bad
 * lines (which stop the CPU for 40 cycles), the raster interrupt, and
 * sprites with expansion, multicolour, priority and both kinds of collision.
 * A register written during a line changes the pixels from about where the
 * beam is (to 8 pixels), so raster bars and splits show as they should; the
 * fetches happen at one moment.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define C64_LINES        312     ///< PAL
/// CPU cycles per raster line (PAL).
#define C64_LINE_CYCLES  63
#define C64_FRAME_CYCLES (C64_LINES * C64_LINE_CYCLES)  ///< 19656
/// The PAL C64's CPU clock, in Hz.
#define C64_CPU_HZ       985248

/// The picture drawn: 320x200 inside a 32-pixel border left and right and
/// 35/37 lines top and bottom. Pixels are colour numbers 0-15.
#define C64_SCREEN_W     384
/// Lines of the picture drawn (the border included).
#define C64_SCREEN_H     272
#define C64_FIRST_LINE   16      ///< the raster line drawn as row 0
#define C64_FIRST_X      (24 - 32)  ///< the sprite X coordinate of column 0

/// The VIC-II: registers, raster position, sprites and the line being drawn.
typedef struct c64_vic {
    uint8_t reg[0x40];  ///< the registers
    uint16_t raster;      ///< the line being drawn
    uint8_t irq_flags;    ///< $D019 bits 0-3
    bool irq;             ///< the IRQ output

    /// Memory as the VIC sees it: a 16 KB bank of RAM (bank is 0, $4000,
    /// $8000 or $C000, set by CIA 2), with the character ROM at $1000-$1FFF
    /// in banks 0 and 2, and the 1 KB colour RAM (low nybbles).
    ///@{
    const uint8_t *ram, *charrom, *color;
    ///@}
    uint16_t bank;  ///< the 16 KB bank the VIC-II sees

    /// The picture, C64_SCREEN_W x C64_SCREEN_H colour numbers (NULL: none).
    uint8_t *frame;
    /// Each row of frame that was drawn with different pixels from before
    /// sets its bit here (the caller clears them).
    uint32_t dirty[(C64_SCREEN_H + 31) / 32];
    /// Or (as well) each row as it is drawn, C64_SCREEN_W colour numbers.
    void (*line_out)(void *ctx, int row, const uint8_t *pix);
    void *out_ctx;  ///< passed to the line callback
    /// The rows drawn: [draw_from, draw_to) of the C64_SCREEN_H (all of them
    /// unless set; reset keeps them). Sprite collisions are only seen on these.
    ///@{
    uint16_t draw_from, draw_to;
    ///@}
    bool no_draw;  ///< the lines aren't drawn this frame (the rest goes on)

    /// The line being drawn, in pieces: before each write to a register
    /// (at write_x, which the caller sets from the cycle) the pixels up to
    /// there are drawn with the registers as they were.
    uint8_t line[C64_SCREEN_W];
    int16_t done_x;   ///< drawn up to here
    int16_t write_x;  ///< where the next write lands
    bool line_on;     ///< this line is drawn
    bool bad;         ///< and is a bad line

    /// Video logic.
    ///@{
    uint16_t vcbase, vc;
    ///@}
    uint8_t rc;  ///< the row counter (RC)
    bool display;      ///< display state (else idle)
    bool den_frame;    ///< DEN was set on line $30: bad lines happen this frame
    bool vborder;      ///< the vertical border flip-flop
    /// the row of characters fetched on the last bad line
    ///@{
    uint8_t matrix[40], colors[40];
    ///@}

    /// Sprites.
    uint8_t sp_mcbase[8];
    /// each sprite's DMA on, and its Y expansion flip-flop
    ///@{
    bool sp_active[8], sp_yexp_ff[8];
    ///@}
    /// $D01E, $D01F
    ///@{
    uint8_t ss_coll, sb_coll;
    ///@}
} c64_vic;

/// Resets the VIC-II.
/// @param v the VIC-II
void c64_vic_reset(c64_vic *v);
/// Reads VIC-II register `reg` (0-63).
/// @param v the VIC-II
/// @param reg the register
/// @return its value
uint8_t c64_vic_read(c64_vic *v, uint8_t reg);
/// Writes VIC-II register `reg` (0-63).
/// @param v the VIC-II
/// @param reg the register
/// @param value its new value
void c64_vic_write(c64_vic *v, uint8_t reg, uint8_t value);
/// The start of a line: the raster interrupt.
/// @param v the VIC-II
void c64_vic_line_start(c64_vic *v);
/// About cycle 14 of the line: the bad line's fetches; returns how many
/// cycles the CPU loses to the VIC's fetches.
/// @param v the VIC-II
/// @return the cycles the CPU loses
int c64_vic_line_fetch(c64_vic *v);
/// The end of the line's cycles: the rest of the line drawn.
/// @param v the VIC-II
void c64_vic_line_finish(c64_vic *v);
/// The pixel (0 to C64_SCREEN_W) a register write lands on, from the cycle of
/// the line it is made in (0: where the line's raster interrupt comes).
/// @param cycle the cycle of the line
/// @return the pixel the write lands on
int c64_vic_cycle_x(int cycle);
/// The end of a line: on to the next. True at the end of the frame.
/// @param v the VIC-II
/// @return true at the end of the frame
bool c64_vic_line_end(c64_vic *v);

/// The colours (Pepto's PAL palette), 0xRRGGBB.
extern const uint32_t c64_palette[16];
