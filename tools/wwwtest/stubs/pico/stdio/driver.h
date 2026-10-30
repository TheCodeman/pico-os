/// @file driver.h
/// @brief PC stand-in for a Pico SDK stdio driver (web browser tests).
#pragma once
#include <stdbool.h>
/// A stdio driver: where output goes.
typedef struct stdio_driver {
    void (*out_chars)(const char *buf, int len);  ///< writes
} stdio_driver_t;
void stdio_set_driver_enabled(stdio_driver_t *d, bool on);  ///< output on or off
