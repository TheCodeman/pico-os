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

/// No correction: the datasheet's formula is about right on the test board.
/// (An earlier note said it read 25 C low, but the ADC's reference checks
/// out: VSYS / 3 on GPIO 43 reads a steady 4.72 V, as USB's 5 V less the
/// board's diode should; and the reading follows the clock, about 38 C at
/// 126 MHz and 43 C at 378 with everything running.)
#define CHIP_TEMP_OFFSET 0

int chip_temp_c(void) {
    static bool started;
    if (!started) {
        adc_init();  // BASIC's ADC() and TEMP() share the ADC; both pick their input each time
        started = true;
    }
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    for (int i = 0; i < 4; ++i)  // the first readings after a change of input are off
        adc_read();
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
