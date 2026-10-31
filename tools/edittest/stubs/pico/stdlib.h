/// @file stdlib.h
/// @brief PC stand-in for the Pico SDK's time and input functions (text editor tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;               ///< microseconds
#define PICO_ERROR_TIMEOUT (-1)                 ///< no byte in time
int getchar_timeout_us(uint32_t us);            ///< uitest.c: the next byte typed
absolute_time_t make_timeout_time_ms(uint32_t ms);  ///< now + ms
bool time_reached(absolute_time_t t);           ///< it is t or later
