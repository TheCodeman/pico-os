/// @file tusb.h
/// @brief PC stand-in for TinyUSB's HID key codes (swarm tests).
#pragma once
enum {
    HID_KEY_A = 0x04, HID_KEY_D = 0x07, HID_KEY_M = 0x10, HID_KEY_P = 0x13, HID_KEY_S = 0x16,
    HID_KEY_W = 0x1a, HID_KEY_X = 0x1b, HID_KEY_Z = 0x1d, HID_KEY_1 = 0x1e, HID_KEY_ENTER = 0x28,
    HID_KEY_ESCAPE = 0x29, HID_KEY_BACKSPACE = 0x2a, HID_KEY_SPACE = 0x2c, HID_KEY_F10 = 0x43,
    HID_KEY_ARROW_RIGHT = 0x4f, HID_KEY_ARROW_LEFT = 0x50, HID_KEY_ARROW_DOWN = 0x51,
    HID_KEY_ARROW_UP = 0x52, HID_KEY_KEYPAD_ENTER = 0x58,
};
#define KEYBOARD_MODIFIER_LEFTCTRL 0x01   ///< left Ctrl
#define KEYBOARD_MODIFIER_RIGHTCTRL 0x10  ///< right Ctrl
