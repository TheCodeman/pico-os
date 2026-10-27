/**
 * @file mouse.h
 * @brief A USB mouse (boot or report protocol, through a hub, plugged in any time; also the mouse half of a keyboard-and-mouse receiver).
 *
 * Core 1's USB task feeds it; programs on core 0 read where it is and what
 * happened.
 *
 * Positions are screen pixels, 0..639 and 0..479 (a text cell is 8 x 16).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The left button's bit.
#define MOUSE_LEFT   1
/// The right button's bit.
#define MOUSE_RIGHT  2
/// The middle button's bit.
#define MOUSE_MIDDLE 4

/// Mouse event types.
enum { MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_WHEEL };

/// A mouse event: its type, buttons, wheel, position and double-click flag.
typedef struct {
    uint8_t type;     ///< MOUSE_MOVE / DOWN / UP / WHEEL
    uint8_t button;   ///< DOWN and UP: the button that changed
    uint8_t buttons;  ///< all buttons held, after the event
    int8_t wheel;     ///< WHEEL: + away from you (up), - towards you
    /// the pointer's position in pixels
    ///@{
    int16_t x, y;
    ///@}
    bool double_click;  ///< DOWN: the same button, same place, quickly again
} mouse_event_t;

/// How many mice are plugged in.
/// @return the mice connected
int mouse_count(void);
/// Where the pointer is and the buttons held.
/// @param x set to the pointer's x
/// @param y set to its y
/// @param buttons set to the buttons held (MOUSE_ bits)
void mouse_get(int *x, int *y, int *buttons);
/// The next event; false if none. Moves are merged (only the last is kept
/// while nothing else happens).
/// @param e filled in
/// @return false if there was none
bool mouse_event(mouse_event_t *e);
void mouse_flush(void);  ///< forgets the events waiting
/// The wheel's clicks since the last call (+ up).
/// @return the clicks, + up
int mouse_wheel(void);
/// Puts the pointer somewhere.
/// @param argc the argument count
/// @param argv the arguments
/// @param x the new x
/// @param y the new y
void mouse_warp(int x, int y);
/// How fast the pointer moves: 1-10 (5 is half a pixel for each count the
/// mouse sends), and whether fast moves go further.
/// @param speed 1-10
/// @param acceleration fast moves go further
void mouse_set_speed(int speed, bool acceleration);
/// The pointer speed and acceleration now.
/// @param speed set to the speed
/// @param acceleration set to whether acceleration is on
void mouse_get_speed(int *speed, bool *acceleration);
/// The shell's mouse command, and /MOUSE.CFG (read when the drive appears).
void mouse_command(int argc, char **argv);
/// @param mode MOUSE_POINTER_OFF, _ARROW or _TEXT
/// Reads /MOUSE.CFG (the drive has appeared).
void mouse_load_config(void);
/// @return the MOUSE_POINTER_ mode

/// The pointer on the screen, drawn by core 0.
///   MOUSE_POINTER_OFF: none (the default).
///   MOUSE_POINTER_ARROW: an arrow that the video output (dvi.c) draws over
///     the picture as it sends each line, so it never touches the
///     framebuffer. Programs call mouse_pointer_update() in their loop to
///     move it.
///   MOUSE_POINTER_TEXT: the text cell under it in reversed colours, kept up
///     to date by the console (the shell, mc, BASIC's text screen).
enum { MOUSE_POINTER_OFF, MOUSE_POINTER_ARROW, MOUSE_POINTER_TEXT };
/// @param dev_addr the USB device
/// @param instance its HID interface
/// @param boot_protocol the interface is in boot protocol
/// @param desc its report descriptor
/// @param desc_len the descriptor's length
/// @return true if it is a mouse
/// Sets how the pointer is shown (MOUSE_POINTER_ modes).
void mouse_pointer(int mode);
/// How the pointer is shown now.
/// @param dev_addr the USB device
/// @param instance its HID interface
int mouse_pointer_mode(void);
/// Kept for programs that hide the arrow while drawing: the video output draws it, so there is nothing to do.
void mouse_pointer_hide(void);
/// @param dev_addr the USB device
/// @param instance its HID interface
/// @param report the report
/// @param len its length
/// @return true if it was a mouse's
/// Brings the arrow up to date (see mouse_pointer_update).
void mouse_pointer_show(void);
/// Moves the arrow to the mouse's position (call it in a program's loop).
void mouse_pointer_update(void);

/// For the USB code (usb_keyboard.c): an interface came (true if it is a
/// mouse), one went, and a report (true if it was a mouse's).
bool mouse_mounted(uint8_t dev_addr, uint8_t instance, bool boot_protocol, const uint8_t *desc,
                   uint16_t desc_len);
/// For the USB code: a HID interface went away.
void mouse_unmounted(uint8_t dev_addr, uint8_t instance);
void mouse_reset(void);  ///< USB starting again: no mice until they mount
/// For the USB code: a HID report; true if it was a mouse's.
bool mouse_report(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len);
