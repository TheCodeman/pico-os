/// @file stdlib.h
/// @brief PC stand-in for the Pico SDK's time and character functions (web browser tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;               ///< microseconds
#define nil_time ((absolute_time_t)0)           ///< long ago
uint64_t time_us_64(void);                      ///< now
void sleep_ms(uint32_t ms);                     ///< waits
absolute_time_t make_timeout_time_ms(uint32_t ms);  ///< ms from now
bool time_reached(absolute_time_t t);           ///< is it past?
int getchar_timeout_us(uint32_t us);            ///< a key, or -1
