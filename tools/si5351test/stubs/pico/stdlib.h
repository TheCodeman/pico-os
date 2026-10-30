/// @file stdlib.h
/// @brief PC stand-in for the Pico SDK's time functions (si5351 GUI tests).
#pragma once
#include <stdint.h>
typedef uint64_t absolute_time_t;  ///< microseconds
absolute_time_t get_absolute_time(void);           ///< now
uint32_t to_ms_since_boot(absolute_time_t t);      ///< in ms
uint64_t to_us_since_boot(absolute_time_t t);      ///< in us
void sleep_ms(uint32_t ms);                        ///< waits
