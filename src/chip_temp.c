/**
 * @file chip_temp.c
 * @brief Implementation of chip_temp.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "chip_temp.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/adc.h"

/// The datasheet's formula reads about 25 C low on the test board (see
/// BASIC's TEMP() in docs/BASIC_MANUAL.md: probably the board's ADC reference
/// isn't exactly 3.3 V), so that much is added. It errs hot if anything.
#define CHIP_TEMP_OFFSET 25

int chip_temp_c(void) {
    static bool started;
    if (!started) {
        adc_init();  // BASIC's ADC() and TEMP() share the ADC; both pick their input each time
        started = true;
    }
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; ++i)
        sum += adc_read();
    float volts = sum / 16.0f * 3.3f / 4096;
    float c = 27 - (volts - 0.706f) / 0.001721f + CHIP_TEMP_OFFSET;
    return (int)(c + (c < 0 ? -0.5f : 0.5f));
}

int chip_temp_f(void) {
    int c = chip_temp_c();
    return c * 9 / 5 + 32;
}
