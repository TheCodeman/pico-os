/**
 * @file usb_keyboard.h
 * @brief USB keyboards on the native USB port (directly or through a hub), turned into characters.
 *
 * Runs on core 1; core 0 reads the characters through stdio.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdio/driver.h"

/// Core 0, before core 1 starts: sets up the character queue.
void usb_keyboard_init(void);

/// Core 1: starts the USB host stack. Its interrupts then run on core 1,
/// away from the HDMI scanout on core 0.
void usb_keyboard_start(void);

/// Core 1, as often as possible: runs TinyUSB and key repeat.
void usb_keyboard_task(void);

/// Any core: how many keyboards are connected right now.
/// @return the keyboards connected
int usb_keyboard_count(void);
/// Any core: the modifier keys held now (HID bits: KEYBOARD_MODIFIER_LEFTCTRL
/// and so on), and whether any other key is held.
/// @return the HID modifier bits held
uint8_t usb_keyboard_modifiers(void);
/// True if a key other than a modifier is held now.
/// @return true if a key is held
bool usb_keyboard_key_held(void);
/// Any core: every key held now, for programs that want the keyboard as keys
/// rather than characters (the C64's matrix): up to six HID usage codes (0 for
/// none, from the last report) and the modifiers. False if no keyboard is
/// connected.
/// @param keys set to up to six HID usage codes
/// @param mods set to the modifier bits
/// @return false if no keyboard is connected
bool usb_keyboard_held_keys(uint8_t keys[6], uint8_t *mods);
/// Any core: Caps Lock (its LED follows).
/// @return true if Caps Lock is on
bool usb_keyboard_caps_lock(void);
/// Turns Caps Lock on or off (its LED follows).
/// @param on Caps Lock on or off
void usb_keyboard_set_caps_lock(bool on);

/// Any core: a USB joystick or gamepad (a HID one; Xbox-style XInput pads
/// aren't HID): false if none is connected, else its stick (0-255 each way,
/// 127 centred; the D-pad counts when it is a hat switch) and buttons, bit 0
/// the first.
/// @param x set to X, 0-255
/// @param y set to Y, 0-255
/// @param buttons set to the buttons held
/// @return false if no gamepad is connected
bool usb_gamepad_read(uint8_t *x, uint8_t *y, uint32_t *buttons);
/// The same for gamepad n (0 or 1: the first and second plugged in; each
/// keeps its number until it is unplugged). usb_gamepad_read is gamepad 0.
/// @param n 0 or 1
/// @param x set to X, 0-255
/// @param y set to Y, 0-255
/// @param buttons set to the buttons held
/// @return false if that gamepad isn't connected
bool usb_gamepad_read_n(unsigned n, uint8_t *x, uint8_t *y, uint32_t *buttons);

/// Add with stdio_set_driver_enabled() so getchar() reads the keyboard.
/// Arrow and editing keys arrive as ANSI escape sequences (ESC [ A and so on).
extern stdio_driver_t usb_keyboard_stdio_driver;
