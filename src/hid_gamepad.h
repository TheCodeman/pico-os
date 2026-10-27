/**
 * @file hid_gamepad.h
 * @brief Finding a joystick's or gamepad's stick, hat switch and buttons in its HID report descriptor, and reading them from its reports.
 *
 * Portable C (the PC tests run it too); usb_keyboard.c uses it for any HID
 * device that isn't a keyboard.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Where one control is in a gamepad's HID report: bit offset, size and range.
typedef struct {
    uint16_t bit, size;  ///< where in the report (after the ID byte), in bits
    int32_t min, max;    ///< logical range
    bool is_signed, valid;  ///< whether the value is signed; whether the field was found
} hid_field;

/// A gamepad's report layout: its axes, hat and buttons, read from its HID report descriptor.
typedef struct {
    uint8_t report_id;  ///< 0: the device doesn't number its reports
    hid_field x, y;     ///< Generic Desktop X and Y (absolute)
    hid_field hat;      ///< Hat Switch: 8 directions from min, anything else centred
    uint16_t button_bit;  ///< the first button's bit in the report
    uint8_t buttons;    ///< how many (Button page usages 1..buttons, from button_bit)
} hid_gamepad_layout;

/// True if the descriptor describes a joystick or gamepad: a report with an
/// absolute X and Y, or a hat switch, as inputs (mice, whose X and Y are
/// relative, and keyboards don't count).
bool hid_gamepad_parse(const uint8_t *desc, size_t len, hid_gamepad_layout *g);

/// Reads one report: x and y 0-255 (127 when centred; the hat switch counts
/// when it is pressed), and the buttons, bit 0 the first. False if the
/// report isn't the one with the stick in it.
bool hid_gamepad_decode(const hid_gamepad_layout *g, const uint8_t *report, size_t len,
                        uint8_t *x, uint8_t *y, uint32_t *buttons);
