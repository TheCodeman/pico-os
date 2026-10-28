/// @file stdlib.h
/// @brief PC stand-in for the Pico SDK's time and console functions (swarm tests).
#pragma once
#include <stdint.h>
uint32_t time_us_32(void);           ///< microseconds
void sleep_ms(uint32_t ms);          ///< waits
int getchar_timeout_us(uint32_t us); ///< a character, or -1
#define tight_loop_contents() ((void)0)  ///< nothing
