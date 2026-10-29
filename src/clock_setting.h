/**
 * @file clock_setting.h
 * @brief The system clock the board starts at: 126, 252 or 378 MHz (all make HDMI's 126 MHz), kept in the flash's last sector so it is there before anything else starts.
 *
 * main() asks for it first thing (clock_setting_boot) and sets the clock up
 * for it; the clock command changes it (clock_setting_save) and restarts the
 * board. A setting other than the default boots with a watchdog running: if
 * the board doesn't get as far as the shell twice in a row, it starts at the
 * default instead (the setting stays, so clock shows what happened).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

/// The clocks the board can run at, in MHz.
#define CLOCK_SETTING_COUNT 3
extern const unsigned clock_setting_mhz[CLOCK_SETTING_COUNT];

/// Core 0, first thing in main(): the clock to start at (the saved one, or
/// the default if none is saved or it failed to start twice). Starts the
/// watchdog when that isn't the default.
/// @return the clock in MHz
unsigned clock_setting_boot(void);

/// Core 0, once the shell is running: the clock started, so the failed-boot
/// count goes back to 0 and the watchdog stops.
void clock_setting_booted(void);

/// The clock saved for the next start (the default if none is).
/// @return the clock in MHz
unsigned clock_setting_saved(void);

/// True if this start fell back to the default because the saved clock
/// failed to start twice.
/// @return true after a fallback
bool clock_setting_fell_back(void);

/// Core 0: sets the flash's timing for a system clock of mhz (at 378 MHz the
/// boot ROM's would run it too fast), from SRAM with interrupts off. Call it
/// before raising the clock, and after anything that sets the flash up again
/// (setting the PSRAM up, writing the flash).
/// @param mhz the system clock, in MHz
void clock_flash_timing(unsigned mhz);

/// Saves mhz for the next start, in the flash's last sector. Core 1 is
/// paused and interrupts are off while the flash is written (about 50 ms:
/// the picture may jump once).
/// @param mhz one of clock_setting_mhz
/// @return false if mhz isn't one of them or the write failed
bool clock_setting_save(unsigned mhz);
