/// @file mc.h
/// @brief PC stand-in for mc's keys and screen saving (sprite editor tests).
#pragma once
#include <stdbool.h>
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INS, KEY_DEL, KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_NONE = -1,
};
#define CTRL(c) ((c) & 0x1f)  ///< a control key
int key_poll(void);   ///< a key, or KEY_NONE
void scr_begin(void); ///< saves the shell screen
void scr_end(void);   ///< puts it back
