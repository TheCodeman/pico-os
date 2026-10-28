/// @file stdlib.h
/// @brief PC stand-in for the Pico SDK's time functions (SDR tests): a clock that only moves when the test sleeps.
#pragma once
#include <stdint.h>
typedef uint64_t absolute_time_t;  ///< microseconds
absolute_time_t get_absolute_time(void);           ///< now
uint32_t to_ms_since_boot(absolute_time_t t);      ///< in ms
uint32_t time_us_32(void);                         ///< now, in us
void sleep_ms(uint32_t ms);                        ///< waits
void sleep_us(uint64_t us);                        ///< waits
