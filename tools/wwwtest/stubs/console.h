/// @file console.h
/// @brief PC stand-in for the console (web browser tests): cells in an array.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdio/driver.h"
extern stdio_driver_t console_stdio_driver;  ///< the console's
void console_set_cell(int row, int col, uint8_t ch, uint8_t attr);     ///< puts a cell
void console_get_cell(int row, int col, uint8_t *ch, uint8_t *attr);   ///< reads one
void console_get_cursor(int *row, int *col);                           ///< the cursor
