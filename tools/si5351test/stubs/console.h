/// @file console.h
/// @brief PC stand-in for the console (si5351 GUI tests).
#pragma once
#include <stdbool.h>
typedef struct { int unused; } stdio_driver_t;  ///< a stdio driver
extern stdio_driver_t console_stdio_driver;     ///< the console's
void console_freeze(bool on);                   ///< no cursor
void stdio_set_driver_enabled(stdio_driver_t *d, bool on);  ///< output on or off
