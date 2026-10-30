/// @file pc_printf.h
/// @brief Forced into each file of the web browser tests: printing goes
///        through the stdio drivers that are on, as on the board.
#pragma once
#include <stdio.h>
int pc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  ///< printf
#define printf(...) pc_printf(__VA_ARGS__)  ///< (so the browser can catch it)
