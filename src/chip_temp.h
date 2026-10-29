/**
 * @file chip_temp.h
 * @brief The RP2350's temperature, from its on-chip sensor, for the start-up banner and the clock menu.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The chip's temperature in degrees C, roughly: the sensor through the ADC
/// (16 readings averaged, as it is noisy), by the datasheet's formula, plus
/// CHIP_TEMP_OFFSET. Core 0; takes about 50 us.
/// @return degrees C
int chip_temp_c(void);

/// The same in degrees F (what the banner and the clock menu show).
/// @return degrees F
int chip_temp_f(void);
