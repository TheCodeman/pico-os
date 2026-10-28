/// @file usb_keyboard.h
/// @brief PC stand-in for the USB keyboard and gamepads (swarm tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool usb_keyboard_held_keys(uint8_t keys[6], uint8_t *mods);  ///< keys held
bool usb_gamepad_read_n(unsigned n, uint8_t *x, uint8_t *y, uint32_t *buttons);  ///< a gamepad
