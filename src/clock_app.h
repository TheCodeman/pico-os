/**
 * @file clock_app.h
 * @brief The clock shell command: the processor clock the board starts at (126, 252 or 378 MHz).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// clock [126 | 252 | 378 | status]: without an argument a menu to pick the
/// clock; with one, saves it and restarts the board; status prints them.
/// @param argc the argument count
/// @param argv the arguments, as the shell passes them
void clock_main(int argc, char **argv);
