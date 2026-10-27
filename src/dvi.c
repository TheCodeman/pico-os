/**
 * @file dvi.c
 * @brief Based on the dvi_out_hstx_encoder example in raspberrypi/pico-examples (BSD-3-Clause), extended from DVI to HDMI so it can carry audio.
 *
 * Two DMA channels ping-pong command lists and scanlines into the HSTX FIFO;
 * the HSTX TMDS encoder does the 8b/10b work for pixels, and data islands
 * are sent as pre-encoded RAW words.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "dvi.h"

#include <string.h>

#include "hdmi_audio.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/timer.h"
#include "hardware/resets.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/platform.h"
#include "pico/time.h"

#define TMDS_CTRL_00 0x354u  ///< TMDS control symbol for C1 C0 = 00
#define TMDS_CTRL_01 0x0abu  ///< TMDS control symbol for C1 C0 = 01
#define TMDS_CTRL_10 0x154u  ///< TMDS control symbol for C1 C0 = 10
#define TMDS_CTRL_11 0x2abu  ///< TMDS control symbol for C1 C0 = 11

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))  ///< sync: vsync 0, hsync 0
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))  ///< sync: vsync 0, hsync 1
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))  ///< sync: vsync 1, hsync 0
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))  ///< sync: vsync 1, hsync 1

#define MODE_H_FRONT_PORCH   16  ///< 640x480 at 60 Hz: the horizontal front porch, in pixels
#define MODE_H_SYNC_WIDTH    96  ///< the horizontal sync, in pixels
#define MODE_H_BACK_PORCH    48  ///< the horizontal back porch, in pixels
#define MODE_H_ACTIVE_PIXELS DVI_WIDTH  ///< pixels shown in a line

#define MODE_V_FRONT_PORCH   10  ///< the vertical front porch, in lines
#define MODE_V_SYNC_WIDTH    2  ///< the vertical sync, in lines
#define MODE_V_BACK_PORCH    33  ///< the vertical back porch, in lines
#define MODE_V_ACTIVE_LINES  DVI_HEIGHT  ///< lines shown

/// Lines in a frame.
#define MODE_V_TOTAL_LINES \
    (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

#define HSTX_CMD_RAW         (0x0u << 12)  ///< HSTX command: raw words
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)  ///< HSTX command: a raw word repeated
#define HSTX_CMD_TMDS        (0x2u << 12)  ///< HSTX command: pixels through the TMDS encoders
#define HSTX_CMD_NOP         (0xfu << 12)  ///< HSTX command: nothing

/// HDMI preambles: CTL0..CTL3 ride on channels 1 and 2 (HDMI 1.4 table 5-2).
#define LANE12(c1, c2)       (((c1) << 10) | ((c2) << 20))
#define PREAMBLE_VIDEO       LANE12(TMDS_CTRL_01, TMDS_CTRL_00)  ///< the preamble before video
#define PREAMBLE_DATA_ISLAND LANE12(TMDS_CTRL_01, TMDS_CTRL_01)  ///< the preamble before a data island
#define VIDEO_GUARD_BAND     (0x2ccu | (0x133u << 10) | (0x2ccu << 20))  ///< the guard band before video
#define VIDEO_PREAMBLE_PIXELS 8  ///< the video preamble's length in pixels
#define VIDEO_GUARD_PIXELS    2  ///< the video guard band's length in pixels

#define DMACH_PING 0  ///< DMA channel for the first line buffer
#define DMACH_PONG 1  ///< DMA channel for the second line buffer

/// The first line shown.
#define FIRST_ACTIVE_LINE (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)

/// dvi_framebuf is placed by the linker script, just before the heap
/// (src/ld/section_extra_post_data.incl): the genesis command runs code from
/// its far end while it needs only a quarter of it.
volatile bool dvi_line_double;
volatile bool dvi_pixel_double;
/// The line-doubled band: its first row and height.
///@{
volatile uint16_t dvi_double_top, dvi_double_rows = DVI_HEIGHT / 2;
///@}
static uint32_t black_row[MODE_H_ACTIVE_PIXELS / 4];  ///< in SRAM, as the IRQ needs

volatile bool dvi_arrow_on;
/// Where the arrow is.
///@{
volatile int16_t dvi_arrow_x, dvi_arrow_y;
///@}
/// The arrow, 11 x 17: bit c of a row is column c (outline black, inside white).
#define ARROW_W 11
#define ARROW_H 17  ///< its height
/// Its outline (black).
static uint16_t arrow_outline[ARROW_H] = {0x001, 0x003, 0x005, 0x009, 0x011, 0x021, 0x041, 0x081, 0x101,
                                          0x201, 0x7c1, 0x049, 0x095, 0x093, 0x121, 0x120, 0x0c0};
/// Its inside (white).
static uint16_t arrow_fill[ARROW_H] = {0x000, 0x000, 0x002, 0x006, 0x00e, 0x01e, 0x03e, 0x07e, 0x0fe,
                                       0x1fe, 0x03e, 0x036, 0x062, 0x060, 0x0c0, 0x0c0, 0x000};

/// The arrow over framebuffer row fbrow, now in the line's pixels at px.
/// @param px the line's pixels
/// @param fbrow the framebuffer row
static void __not_in_flash_func(arrow_over)(uint8_t *px, int fbrow) {
    int ay = fbrow - dvi_arrow_y, x0 = dvi_arrow_x;
    if (ay < 0 || ay >= ARROW_H)
        return;
    uint32_t o = arrow_outline[ay], f = arrow_fill[ay];
    for (int c = 0; c < ARROW_W && x0 + c < DVI_WIDTH; ++c) {
        if (o >> c & 1)
            px[x0 + c] = 0x00;
        else if (f >> c & 1)
            px[x0 + c] = 0xff;
    }
}

/// Each scanline is built just before it is needed, because the data island
/// changes from line to line: the blanking commands, then for active lines a
/// copy of the pixels. Each DMA channel has its own buffer, rebuilt only once
/// that channel has finished sending it.
///
/// Each DMA transfer carries LINES_PER_BUF whole lines, back to back. When one
/// channel finishes, the IRQ must refill its buffer before the other channel
/// has sent its lines: with one line per transfer that left about 30 us, and
/// anything on core 0 that kept interrupts off longer (up to 85 us was seen)
/// broke the picture. Six lines give about 190 us, less the 60 us or so it
/// takes to build them.
#define LINE_CMD_WORDS   96
/// Pixel words in a line.
#define LINE_PIXEL_WORDS (MODE_H_ACTIVE_PIXELS / sizeof(uint32_t))
/// Lines a buffer holds.
#define LINES_PER_BUF    6
/// The two line buffers, sent by DMA in turn.
static uint32_t line_buf[2][LINES_PER_BUF * (LINE_CMD_WORDS + LINE_PIXEL_WORDS)];

/// The IRQ ran too late: the scanout must restart.
static volatile bool scanout_late;
static volatile uint32_t lines_built;  ///< check_scanout() watches this move
/// The buffer being sent is the second.
static bool dma_pong = false;
static uint v_scanline;  ///< the next line to build

static uint build_line(uint32_t *buf, uint line);

/// Builds the next LINES_PER_BUF lines into buf; returns the word count.
/// @param buf the buffer
/// @return its words
static uint __not_in_flash_func(build_lines)(uint32_t *buf) {
    uint32_t *p = buf;
    for (uint i = 0; i < LINES_PER_BUF; ++i) {
        p += build_line(p, v_scanline);
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        ++lines_built;
    }
    return p - buf;
}

/// One whole line, with the data island inside the hsync pulse:
///   front porch 16 | sync: preamble 8, island 68, 20 | back porch 48 | active
/// Active lines end the back porch with the video preamble and guard band,
/// followed by a TMDS command and the line's pixels.
/// @param buf where
/// @param line the line
/// @return its words
static uint __not_in_flash_func(build_line)(uint32_t *buf, uint line) {
    bool vsync = line >= MODE_V_FRONT_PORCH && line < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH;
    uint32_t idle = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t sync = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t *p = buf;

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = idle;
    *p++ = HSTX_CMD_RAW_REPEAT | HDMI_ISLAND_PREAMBLE;
    *p++ = (sync & 0x3ff) | PREAMBLE_DATA_ISLAND;
    p = hdmi_audio_build_island(p, line, vsync);
    *p++ = HSTX_CMD_RAW_REPEAT |
           (MODE_H_SYNC_WIDTH - HDMI_ISLAND_PREAMBLE - HDMI_ISLAND_PIXELS);
    *p++ = sync;

    if (line >= FIRST_ACTIVE_LINE) {
        *p++ = HSTX_CMD_RAW_REPEAT |
               (MODE_H_BACK_PORCH - VIDEO_PREAMBLE_PIXELS - VIDEO_GUARD_PIXELS);
        *p++ = idle;
        *p++ = HSTX_CMD_RAW_REPEAT | VIDEO_PREAMBLE_PIXELS;
        *p++ = (idle & 0x3ff) | PREAMBLE_VIDEO;
        *p++ = HSTX_CMD_RAW | VIDEO_GUARD_PIXELS;
        *p++ = VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;
        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
        uint row = line - FIRST_ACTIVE_LINE;
        const uint32_t *src;
        int fbrow = (int)row;
        if (dvi_line_double) {
            int r = (int)(row >> 1) - dvi_double_top;
            fbrow = r >= 0 && r < dvi_double_rows ? r : -1000;
            if (fbrow >= 0 && dvi_pixel_double) {
                // Half-width rows: each byte twice (bytes are pixels left to right).
                const uint32_t *half = (const uint32_t *)&dvi_framebuf[r * (MODE_H_ACTIVE_PIXELS / 2)];
                for (uint i = 0; i < LINE_PIXEL_WORDS / 2; ++i) {
                    uint32_t w = half[i];
                    *p++ = (w & 0xff) * 0x0101u | ((w >> 8) & 0xff) * 0x01010000u;
                    *p++ = ((w >> 16) & 0xff) * 0x0101u | (w >> 24) * 0x01010000u;
                }
                if (dvi_arrow_on)
                    arrow_over((uint8_t *)(p - LINE_PIXEL_WORDS), fbrow);
                return p - buf;
            }
            src = fbrow >= 0 ? (const uint32_t *)&dvi_framebuf[r * MODE_H_ACTIVE_PIXELS] : black_row;
        } else {
            src = (const uint32_t *)&dvi_framebuf[row * MODE_H_ACTIVE_PIXELS];
        }
        for (uint i = 0; i < LINE_PIXEL_WORDS; ++i)
            *p++ = src[i];
        if (dvi_arrow_on)
            arrow_over((uint8_t *)(p - LINE_PIXEL_WORDS), fbrow);
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
        *p++ = idle;
        *p++ = HSTX_CMD_NOP;
    }
    return p - buf;
}

/// One DMA transfer per line. Each IRQ reloads the channel that just
/// finished with the line after the one now running.
static void __not_in_flash_func(dma_irq_handler)(void) {
    // The longest gap between interrupts (normally LINES_PER_BUF lines, 190
    // us), for the message main.c prints when the scanout is restarted.
    static uint32_t last_us;
    uint32_t now_us = timer_hw->timerawl;
    if (lines_built > 2 * LINES_PER_BUF && now_us - last_us > dvi_worst_gap_us)
        dvi_worst_gap_us = now_us - last_us;
    last_us = now_us;
    uint32_t pending = dma_hw->ints0 & ((1u << DMACH_PING) | (1u << DMACH_PONG));
    dma_hw->ints0 = pending;
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    // A stray interrupt (e.g. left over from aborting the channels) isn't
    // the line we're waiting for; if the channels really are out of step,
    // check_scanout() sees the scanout stop and starts it again.
    if (!(pending & (1u << ch_num)))
        return;
    dma_pong = !dma_pong;

    // If this channel is already running again, or both have finished, the
    // IRQ came more than a line late (something kept interrupts off) and
    // the other channel has restarted this one from where it stopped, past
    // the end of its buffer. The HSTX is now being fed junk; have
    // check_scanout() start over cleanly.
    if ((ch->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) ||
        pending == ((1u << DMACH_PING) | (1u << DMACH_PONG)))
        scanout_late = true;

    uint32_t *buf = line_buf[ch_num];
    ch->read_addr = (uintptr_t)buf;
    ch->transfer_count = build_lines(buf);
    uint32_t took = timer_hw->timerawl - now_us;
    if (took > dvi_worst_build_us)
        dvi_worst_build_us = took;
}

/// Sets the HSTX up: its TMDS encoders on the RGB332 bits, and the pins.
static void hstx_setup(void) {
    // RGB332: lane 0 (blue) takes pixel bits 1:0, lane 1 (green) bits 4:2,
    // lane 2 (red) bits 7:5. NBITS fields hold (bit count - 1).
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels arrive four to a 32-bit word; control symbols are a whole word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Clock period of 5 HSTX cycles, shifting 2 bits per cycle (DDR):
    // 126 MHz clk_hstx (main() divides it down from a faster clk_sys) -> 252
    // Mbps per lane -> 25.2 MHz pixel clock.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // HSTX outputs 0..7 are GPIO 12..19. Pico DVI Sock pinout:
    //   GP12 D0+  GP13 D0-   GP14 CK+  GP15 CK-
    //   GP16 D2+  GP17 D2-   GP18 D1+  GP19 D1-
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    static const int lane_to_output_bit[3] = {0, 6, 4};
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = lane_to_output_bit[lane];
        // Even bits in the first half of each HSTX cycle, odd bits in the second.
        uint32_t sel = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }
}

/// Loads both channels with the first lines and starts them. Both
/// channels move one buffer into the FIFO and chain to the other; when one
/// finishes, the IRQ reloads it while the other is running.
static void start_scanout(void) {
    lines_built = 0;  // so the gap across a restart isn't counted
    dma_pong = false;
    v_scanline = 0;
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                          line_buf[0], build_lines(line_buf[0]), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                          line_buf[1], build_lines(line_buf[1]), false);
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(DMACH_PING);
}

/// For debugging over SWD: how often the scanout had to be restarted (and how
/// often because the IRQ ran late), and the state of a channel that failed.
/// Restarts, restarts because the IRQ ran late, the longest gap between IRQs and the longest build (us).
///@{
volatile uint32_t dvi_restarts, dvi_late, dvi_worst_gap_us, dvi_worst_build_us;
///@}
/// A failed channel's control register and read address.
///@{
volatile uint32_t dvi_fail_ctrl, dvi_fail_read_addr;
///@}

/// If the scanout breaks (the IRQ ran late, a channel reported a bus error,
/// or the chain stopped), the picture is gone for good, because the HSTX
/// loses track of where its commands start. Every 50 ms, check, and if so,
/// reset the HSTX and start again from the top of the frame: the monitor
/// sees one bad frame.
/// @param t the timer
/// @return true (keep going)
static bool check_scanout(repeating_timer_t *t) {
    static uint stalls;
    const uint32_t errors = DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS;
    bool failed = false;
    for (uint ch = DMACH_PING; ch <= DMACH_PONG; ++ch) {
        if (dma_hw->ch[ch].ctrl_trig & errors) {
            dvi_fail_ctrl = dma_hw->ch[ch].ctrl_trig;
            dvi_fail_read_addr = dma_hw->ch[ch].read_addr;
            failed = true;
        }
    }
    // No lines built twice running (some 3000 are due) means the chain has
    // stopped, or its interrupts are being ignored as out of step.
    static uint32_t last_lines;
    uint32_t lines = lines_built;
    stalls = lines != last_lines ? 0 : stalls + 1;
    last_lines = lines;
    bool late = scanout_late;
    if (!failed && !late && stalls < 2)
        return true;
    stalls = 0;
    if (late)
        ++dvi_late;
    ++dvi_restarts;
    irq_set_enabled(DMA_IRQ_0, false);
    // Unchain the channels first: a channel being aborted can still trigger
    // the one it chains to, which would then be running while it is set up.
    for (uint ch = DMACH_PING; ch <= DMACH_PONG; ++ch)
        hw_write_masked(&dma_hw->ch[ch].al1_ctrl, ch << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB,
                        DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
    dma_hw->abort = (1u << DMACH_PING) | (1u << DMACH_PONG);
    // A channel has stopped once BUSY clears (the abort bits can clear
    // first, while a transfer is still in flight).
    while (dma_channel_is_busy(DMACH_PING) || dma_channel_is_busy(DMACH_PONG))
        tight_loop_contents();
    for (uint ch = DMACH_PING; ch <= DMACH_PONG; ++ch)
        dma_hw->ch[ch].al1_ctrl = dma_hw->ch[ch].al1_ctrl;  // error flags are write-1-to-clear
    // Reset the HSTX: that empties its FIFO and its command decoder, which
    // may be part way through a junk command and would otherwise stay out
    // of step with the lines for good.
    reset_block_num(RESET_HSTX);
    unreset_block_num_wait_blocking(RESET_HSTX);
    hstx_setup();
    scanout_late = false;
    start_scanout();
    return true;
}

void dvi_init(void) {
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);  // not in .bss, so not cleared at boot
    hdmi_audio_init();

    hstx_setup();
    for (uint pin = 12; pin <= 19; ++pin)
        gpio_set_function(pin, 0);  // function 0 is HSTX on these pins

    dma_claim_mask((1u << DMACH_PING) | (1u << DMACH_PONG));
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);  // scanout must never wait

    // Give DMA priority on the bus so CPU work never starves the scanout.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    start_scanout();

    static repeating_timer_t watchdog;
    add_repeating_timer_ms(-50, check_scanout, NULL, &watchdog);
}
