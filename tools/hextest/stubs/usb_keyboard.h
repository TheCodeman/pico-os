/// @file usb_keyboard.h
/// @brief PC stand-in for the USB keyboard's modifiers (hex editor tests).
#pragma once
#include <stdint.h>
uint8_t usb_keyboard_modifiers(void);  ///< the HID modifier bits held
