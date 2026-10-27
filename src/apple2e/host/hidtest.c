/**
 * @file hidtest.c
 * @brief Tests for the HID gamepad parser (src/hid_gamepad.c) with hand-written report descriptors of the usual shapes.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "hid_gamepad.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

/// A gamepad: X Y Z Rz (8 bits, 0-255), a 4-bit hat with a null state, then
/// 12 buttons. max255 is how the device writes 255 as logical maximum.
static size_t pad(uint8_t *d, bool one_byte_max) {
    static const uint8_t head[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x15, 0x00};
    static const uint8_t axes[] = {0x75, 0x08, 0x95, 0x04, 0x09, 0x30, 0x09, 0x31,
                                   0x09, 0x32, 0x09, 0x35, 0x81, 0x02};
    static const uint8_t rest[] = {
        0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01,
        0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,              // hat, null state
        0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x0c, 0x81, 0x02,                          // 12 buttons
        0xc0};
    size_t n = 0;
    memcpy(d + n, head, sizeof head), n += sizeof head;
    if (one_byte_max)
        d[n++] = 0x25, d[n++] = 0xff;
    else
        d[n++] = 0x26, d[n++] = 0xff, d[n++] = 0x00;
    memcpy(d + n, axes, sizeof axes), n += sizeof axes;
    memcpy(d + n, rest, sizeof rest), n += sizeof rest;
    return n;
}

int main(void) {
    hid_gamepad_layout g;
    uint8_t d[256], x, y;
    uint32_t b;

    for (int variant = 0; variant < 2; ++variant) {
        size_t n = pad(d, variant);
        CHECK(hid_gamepad_parse(d, n, &g));
        CHECK(g.report_id == 0);
        CHECK(g.x.valid && g.x.bit == 0 && g.x.size == 8 && g.x.min == 0 && g.x.max == 255);
        CHECK(g.y.valid && g.y.bit == 8);
        CHECK(g.hat.valid && g.hat.bit == 32 && g.hat.size == 4);
        CHECK(g.buttons == 12 && g.button_bit == 36);
        // Stick left and down, hat centred (8), buttons 1 and 3.
        uint8_t r1[6] = {0, 255, 0x80, 0x80, 0x08 | 0x50, 0x00};
        CHECK(hid_gamepad_decode(&g, r1, sizeof r1, &x, &y, &b));
        CHECK(x == 0 && y == 255 && b == 0x5);
        // Hat right overrides the centred stick; button 12.
        uint8_t r2[6] = {128, 128, 0x80, 0x80, 0x02, 0x80};
        CHECK(hid_gamepad_decode(&g, r2, sizeof r2, &x, &y, &b));
        CHECK(x == 255 && y == 127 && b == 0x800);
    }

    // Numbered reports, two buttons and padding first, then signed X and Y.
    static const uint8_t pad2[] = {
        0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
        0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0, 0xc0};
    CHECK(hid_gamepad_parse(pad2, sizeof pad2, &g));
    CHECK(g.report_id == 1 && g.buttons == 2 && g.button_bit == 0);
    CHECK(g.x.bit == 8 && g.x.is_signed && g.x.min == -127 && g.x.max == 127);
    uint8_t r3[4] = {1, 0x02, 0x81, 0x7f};
    CHECK(hid_gamepad_decode(&g, r3, sizeof r3, &x, &y, &b));
    CHECK(x == 0 && y == 255 && b == 2);
    uint8_t r4[4] = {1, 0x00, 0x00, 0x00};
    CHECK(hid_gamepad_decode(&g, r4, sizeof r4, &x, &y, &b));
    CHECK(x == 127 && y == 127 && b == 0);
    uint8_t r5[4] = {2, 0x00, 0x00, 0x00};
    CHECK(!hid_gamepad_decode(&g, r5, sizeof r5, &x, &y, &b));  // another report

    // A mouse (relative X and Y) and a boot keyboard aren't gamepads.
    static const uint8_t mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
        0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0};
    CHECK(!hid_gamepad_parse(mouse, sizeof mouse, &g));
    static const uint8_t keyboard[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
        0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
        0x81, 0x00, 0xc0};
    CHECK(!hid_gamepad_parse(keyboard, sizeof keyboard, &g));

    printf(failures ? "hid_gamepad: %d failed\n" : "hid_gamepad: all passed\n", failures);
    return failures != 0;
}
