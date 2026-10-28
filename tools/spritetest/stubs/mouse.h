/// @file mouse.h
/// @brief PC stand-in for the mouse (sprite editor tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MOUSE_LEFT 1    ///< left button
#define MOUSE_RIGHT 2   ///< right button
#define MOUSE_MIDDLE 4  ///< middle button
enum { MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_WHEEL };
enum { MOUSE_POINTER_OFF, MOUSE_POINTER_ARROW, MOUSE_POINTER_TEXT };
/// A mouse event.
typedef struct {
    uint8_t type, button, buttons;  ///< what, which, held
    int8_t wheel;                   ///< wheel steps
    int16_t x, y;                   ///< where
    bool double_click;              ///< a double click
} mouse_event_t;
bool mouse_event(mouse_event_t *e);  ///< the next event
void mouse_pointer(int mode);        ///< the pointer's mode
void mouse_pointer_update(void);     ///< keeps it drawn
