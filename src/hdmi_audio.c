/**
 * @file hdmi_audio.c
 * @brief Implementation of hdmi_audio.h: hDMI data islands: InfoFrames, audio clock regeneration and 48 kHz stereo audio, sent in the horizontal sync period of every scanline.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "hdmi_audio.h"

#include "hardware/sync.h"
#include "pico/platform.h"

// HDMI 1.4 section 5.2.3: packets are 32 pixels. Channel 0 carries HSYNC,
// VSYNC, one header bit and a "not first pixel of the island" flag; channels
// 1 and 2 carry the even and odd bits of the four 64-bit subpackets. Every
// 4-bit value is sent as a TERC4 symbol.

#define HSTX_CMD_RAW (0x0u << 12)

/// Pixel clock and N/CTS for 48 kHz at 25.2 MHz (HDMI 1.4 table 7-3).
#define ACR_N   6144
#define ACR_CTS 25200
#define LINE_RATE (25200000 / 800)  ///< 31.5 kHz
/// One audio packet carries 4 frames, so one is due every LINE_RATE*4/48000 lines.
#define AUDIO_PACKET_COST (LINE_RATE * 4)

#define ACR_EVERY_LINES 32  ///< about 1 kHz, the rate 128*fs/N asks for

typedef struct {
    uint32_t first;  ///< word 0 when the packet opens a data island
    uint32_t w[32];
} hdmi_packet_t;

/// In RAM because the scanout IRQ reads it, and an XIP cache miss
/// there would add latency.
static const uint16_t __not_in_flash("hdmi_audio") terc4[16] = {
    0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
    0x2cc, 0x139, 0x19c, 0x2c6, 0x28e, 0x271, 0x163, 0x2c3,
};
#define GUARD_BAND_CH12 0x133u

static hdmi_packet_t null_packet[2];  ///< indexed by vsync_active
static hdmi_packet_t acr_packet, avi_infoframe, audio_infoframe;

/// 64 packets is about 5 ms of audio, enough to ride out USB work on core 1.
#define RING_LEN 64
static hdmi_packet_t ring[RING_LEN];
static volatile uint32_t ring_head, ring_tail;
static uint32_t audio_credit;

/// BCH ECC from HDMI 1.4 figure 5-5: g(x) = 1 + x^6 + x^7 + x^8, data LSB first.
/// A nibble at a time, from a table of what 4 bit steps do (the packets are
/// encoded on core 1 alongside the sound's sources, so this is kept quick).
static const uint8_t __not_in_flash("hdmi_audio") ecc_nibble[16] = {
    0x00, 0xb3, 0x61, 0xd2, 0xc2, 0x71, 0xa3, 0x10, 0x83, 0x30, 0xe2, 0x51, 0x41, 0xf2, 0x20, 0x93,
};

static inline uint8_t bch_ecc(const uint8_t *data, int len) {
    uint ecc = 0;
    for (int i = 0; i < len; ++i) {
        ecc = (ecc >> 4) ^ ecc_nibble[(ecc ^ data[i]) & 15];
        ecc = (ecc >> 4) ^ ecc_nibble[(ecc ^ (data[i] >> 4)) & 15];
    }
    return (uint8_t)ecc;
}

static void __not_in_flash_func(encode_packet)(hdmi_packet_t *out, const uint8_t header[3],
                          const uint8_t sub[4][7], bool vsync_active) {
    uint8_t hb[3] = {header[0], header[1], header[2]};
    uint32_t h = hb[0] | hb[1] << 8 | hb[2] << 16 | (uint32_t)bch_ecc(hb, 3) << 24;
    // Each subpacket with its ECC as 64 bits, LSB first: two bits a pixel.
    uint32_t w[4][2];
    for (int k = 0; k < 4; ++k) {
        const uint8_t *s = sub[k];
        w[k][0] = s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
        w[k][1] = s[4] | s[5] << 8 | s[6] << 16 | (uint32_t)bch_ecc(s, 7) << 24;
    }
    // Sync is active low, so hsync is 0 inside the island; vsync likewise.
    uint ch0_sync = vsync_active ? 0 : 2;
    for (int half = 0; half < 2; ++half) {
        uint32_t a = w[0][half], b = w[1][half], c = w[2][half], d = w[3][half];
        for (int j = 0; j < 16; ++j) {
            uint ch0 = 8 | (h & 1) << 2 | ch0_sync;
            uint ch1 = (a & 1) | (b & 1) << 1 | (c & 1) << 2 | (d & 1) << 3;
            uint ch2 = (a >> 1 & 1) | (b >> 1 & 1) << 1 | (c >> 1 & 1) << 2 | (d >> 1 & 1) << 3;
            h >>= 1;
            a >>= 2;
            b >>= 2;
            c >>= 2;
            d >>= 2;
            uint32_t rest = (uint32_t)terc4[ch1] << 10 | (uint32_t)terc4[ch2] << 20;
            out->w[half * 16 + j] = terc4[ch0] | rest;
            if (half == 0 && j == 0)
                out->first = terc4[ch0 & 7] | rest;
        }
    }
}

/// InfoFrames put a checksum in PB0 so that header + payload bytes sum to 0.
static void encode_infoframe(hdmi_packet_t *out, uint8_t type, uint8_t version,
                             const uint8_t *payload, int len) {
    uint8_t header[3] = {type, version, (uint8_t)len};
    uint8_t pb[28] = {0};
    uint8_t sum = type + version + len;
    for (int i = 0; i < len; ++i) {
        pb[i + 1] = payload[i];
        sum += payload[i];
    }
    pb[0] = (uint8_t)-sum;
    uint8_t sub[4][7];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 7; ++i)
            sub[k][i] = pb[k * 7 + i];
    encode_packet(out, header, sub, false);
}

void hdmi_audio_init(void) {
    static const uint8_t zero_header[3] = {0};
    static const uint8_t zero_sub[4][7] = {{0}};
    encode_packet(&null_packet[0], zero_header, zero_sub, false);
    encode_packet(&null_packet[1], zero_header, zero_sub, true);

    const uint8_t acr_header[3] = {0x01, 0, 0};
    uint8_t acr_sub[4][7];
    for (int k = 0; k < 4; ++k) {
        const uint8_t s[7] = {0,
                              (ACR_CTS >> 16) & 0x0f, (ACR_CTS >> 8) & 0xff, ACR_CTS & 0xff,
                              (ACR_N >> 16) & 0x0f, (ACR_N >> 8) & 0xff, ACR_N & 0xff};
        for (int i = 0; i < 7; ++i)
            acr_sub[k][i] = s[i];
    }
    encode_packet(&acr_packet, acr_header, acr_sub, false);

    // AVI InfoFrame v2: RGB, 4:3 picture, VIC 1 (640x480p60).
    const uint8_t avi[13] = {0x00, 0x18, 0x00, 1, 0};
    encode_infoframe(&avi_infoframe, 0x82, 2, avi, sizeof avi);

    // Audio InfoFrame: 2 channels, coding and rate taken from the stream.
    const uint8_t aif[10] = {0x01, 0x00, 0x00, 0x00, 0x00};
    encode_infoframe(&audio_infoframe, 0x84, 1, aif, sizeof aif);
}

/// IEC 60958 channel status, one bit per frame of a 192-frame block:
/// consumer, linear PCM, 48 kHz (bit 25), 16-bit words (bit 33).
static bool channel_status(uint frame) {
    return frame == 25 || frame == 33;
}

bool __not_in_flash_func(hdmi_audio_has_room)(void) {
    return ring_head - ring_tail != RING_LEN;
}

bool __not_in_flash_func(hdmi_audio_push)(const int16_t frames[4][2]) {
    static uint frame_index;
    if (ring_head - ring_tail == RING_LEN)
        return false;

    uint8_t header[3] = {0x02, 0x0f, 0};  // layout 0, all four subpackets present
    uint8_t sub[4][7];
    for (int k = 0; k < 4; ++k) {
        if (frame_index == 0)
            header[2] |= 1u << (4 + k);  // B: start of a channel status block
        uint c = channel_status(frame_index);
        uint32_t l = ((uint32_t)(uint16_t)frames[k][0] << 8) & 0xffffff;
        uint32_t r = ((uint32_t)(uint16_t)frames[k][1] << 8) & 0xffffff;
        // Even parity over sample, V, U and C; V = 0 (valid), U = 0.
        uint pl = __builtin_parity(l) ^ c;
        uint pr = __builtin_parity(r) ^ c;
        sub[k][0] = l;
        sub[k][1] = l >> 8;
        sub[k][2] = l >> 16;
        sub[k][3] = r;
        sub[k][4] = r >> 8;
        sub[k][5] = r >> 16;
        sub[k][6] = c << 2 | pl << 3 | c << 6 | pr << 7;
        frame_index = (frame_index + 1) % 192;
    }
    encode_packet(&ring[ring_head % RING_LEN], header, sub, false);
    __dmb();
    ring_head = ring_head + 1;
    return true;
}

static uint32_t *__not_in_flash_func(copy_packet)(uint32_t *dst, const hdmi_packet_t *p,
                                                  bool first) {
    *dst++ = first ? p->first : p->w[0];
    for (int i = 1; i < 32; ++i)
        *dst++ = p->w[i];
    return dst;
}

/// Picks the next audio packet if one is due and queued. The copy happens
/// before the slot is released so the producer can't overwrite it mid-copy.
volatile uint32_t hdmi_audio_underruns;

static uint32_t *__not_in_flash_func(put_audio_or_null)(uint32_t *dst, bool first) {
    if (audio_credit >= AUDIO_PACKET_COST && ring_head == ring_tail)
        ++hdmi_audio_underruns;
    if (audio_credit >= AUDIO_PACKET_COST && ring_head != ring_tail) {
        __dmb();
        dst = copy_packet(dst, &ring[ring_tail % RING_LEN], first);
        __dmb();
        ring_tail = ring_tail + 1;
        audio_credit -= AUDIO_PACKET_COST;
        return dst;
    }
    return copy_packet(dst, &null_packet[0], first);
}

uint32_t *__not_in_flash_func(hdmi_audio_build_island)(uint32_t *dst, uint32_t line,
                                                       bool vsync_active) {
    uint32_t guard = terc4[0xc | (vsync_active ? 0 : 2)] |
                     GUARD_BAND_CH12 << 10 | GUARD_BAND_CH12 << 20;
    *dst++ = HSTX_CMD_RAW | HDMI_ISLAND_PIXELS;
    *dst++ = guard;
    *dst++ = guard;

    // Audio is due 48000 * 4 / 4 times a second; keep at most two packets of
    // credit so an empty queue doesn't turn into a burst later.
    audio_credit += HDMI_AUDIO_RATE;
    if (audio_credit > 2 * AUDIO_PACKET_COST)
        audio_credit = 2 * AUDIO_PACKET_COST;

    if (vsync_active) {
        // Packets are pre-encoded with vsync idle, so send nulls here.
        dst = copy_packet(dst, &null_packet[1], true);
        dst = copy_packet(dst, &null_packet[1], false);
    } else {
        const hdmi_packet_t *scheduled = NULL;
        if (line == 0)
            scheduled = &avi_infoframe;
        else if (line == 1)
            scheduled = &audio_infoframe;
        else if (line % ACR_EVERY_LINES == 4)
            scheduled = &acr_packet;
        dst = put_audio_or_null(dst, true);
        if (scheduled)
            dst = copy_packet(dst, scheduled, false);
        else
            dst = put_audio_or_null(dst, false);
    }

    *dst++ = guard;
    *dst++ = guard;
    return dst;
}
