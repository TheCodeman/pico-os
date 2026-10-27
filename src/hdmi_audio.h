/**
 * @file hdmi_audio.h
 * @brief HDMI data islands: InfoFrames, audio clock regeneration and 48 kHz stereo audio, sent in the horizontal sync period of every scanline.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The HDMI audio sample rate.
#define HDMI_AUDIO_RATE 48000

/// Pixels one data island takes: 8 preamble + 2 guard + 2 packets + 2 guard.
#define HDMI_ISLAND_PREAMBLE 8
/// Pixels in one data island (guard bands and a packet).
#define HDMI_ISLAND_PIXELS   (2 + 2 * 32 + 2)

/// Builds the constant packets. Call before scanout starts.
void hdmi_audio_init(void);

/// Writes the data island for one scanline as HSTX commands: a RAW command
/// followed by HDMI_ISLAND_PIXELS words. Returns the end of what it wrote.
/// Called from the scanout IRQ. The island sits inside the (active-low) hsync
/// pulse; vsync_active says whether vsync is also asserted on this line.
uint32_t *hdmi_audio_build_island(uint32_t *dst, uint32_t line, bool vsync_active);

/// Producer side, for a core other than the one running the scanout IRQ:
/// queues 4 stereo frames as one audio sample packet. Returns false while the
/// queue is full.
bool hdmi_audio_push(const int16_t frames[4][2]);

/// Producer side: true if hdmi_audio_push() would accept a packet now.
bool hdmi_audio_has_room(void);

/// How many times a packet was due but none was queued (core 1 was late).
extern volatile uint32_t hdmi_audio_underruns;
