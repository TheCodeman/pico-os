/**
 * @file clock_setting.c
 * @brief Implementation of clock_setting.h: the start-up clock in the flash's last sector, and the failed-boot fallback.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "clock_setting.h"

#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"

const unsigned clock_setting_mhz[CLOCK_SETTING_COUNT] = {126, 252, 378};

/// The flash's last sector holds the setting (nothing else uses the end of
/// the flash; the firmware is a few MB at the start).
#define SECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/// What the sector holds: a magic number, the clock, and the clock inverted
/// as a check (an erased sector is all ones and fails it).
typedef struct {
    uint32_t magic, mhz, check;
} record;
#define MAGIC 0x434c4b31u  // "CLK1"

/// Watchdog scratch register 0 (the SDK uses 4 to 7 for its reboots): a
/// marker and how many starts in a row at the saved clock haven't reached
/// the shell. It keeps its value through a reset but not a power cycle.
#define BOOTS_MARK  0xc10c0000u
#define BOOTS_MASK  0xffff0000u
/// Starts at the saved clock that may fail before the default is used.
#define MAX_FAILS   2
/// The watchdog while starting at the saved clock: long enough for the
/// start-up (which waits up to 10 s for the drive) and fed while it waits.
#define WATCHDOG_MS 15000

static bool fell_back;
static bool watching;

static bool supported(unsigned mhz) {
    for (int i = 0; i < CLOCK_SETTING_COUNT; i++)
        if (clock_setting_mhz[i] == mhz)
            return true;
    return false;
}

unsigned clock_setting_saved(void) {
    const record *r = (const record *)(XIP_BASE + SECTOR_OFFSET);
    if (r->magic == MAGIC && r->check == ~r->mhz && supported(r->mhz))
        return r->mhz;
    return OS_CLOCK_MHZ;
}

unsigned clock_setting_boot(void) {
    unsigned mhz = clock_setting_saved();
    if (mhz == OS_CLOCK_MHZ) {
        watchdog_hw->scratch[0] = 0;
        return mhz;
    }
    uint32_t s = watchdog_hw->scratch[0];
    unsigned fails = (s & BOOTS_MASK) == BOOTS_MARK ? s & 0xffff : 0;
    if (fails >= MAX_FAILS) {
        fell_back = true;  // stays so (the count isn't cleared) until a power cycle or a new setting
        return OS_CLOCK_MHZ;
    }
    watchdog_hw->scratch[0] = BOOTS_MARK | (fails + 1);
    watchdog_enable(WATCHDOG_MS, true);
    watching = true;
    return mhz;
}

void clock_setting_booted(void) {
    if (watching) {
        watchdog_disable();
        watching = false;
        watchdog_hw->scratch[0] = 0;
    }
}

bool clock_setting_fell_back(void) {
    return fell_back;
}

void __no_inline_not_in_flash_func(clock_flash_timing)(unsigned mhz) {
    if (mhz <= 300)
        return;  // the boot ROM's clk_sys / 3 is fine up to 252 MHz (84 MHz)
    // At 378 MHz, / 4 (94.5 MHz): / 3 would be 126, too near the chip's 133.
    // The read delay is in half clk_sys cycles.
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t = qmi_hw->m[0].timing;
    t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    t |= 4u << QMI_M0_TIMING_CLKDIV_LSB | 3u << QMI_M0_TIMING_RXDELAY_LSB;
    qmi_hw->m[0].timing = t;
    __dsb();
    restore_interrupts(irq);
}

/// The clock the board runs at now, for write_sector (which can't ask: that
/// code is in flash).
static unsigned running_mhz;

/// Run by flash_safe_execute, with the other core paused and interrupts off,
/// from SRAM: erasing and programming put the flash's timing back to the
/// boot ROM's, which at 378 MHz has to be undone before flash is read again.
static void __no_inline_not_in_flash_func(write_sector)(void *param) {
    flash_range_erase(SECTOR_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SECTOR_OFFSET, param, FLASH_PAGE_SIZE);
    clock_flash_timing(running_mhz);
}

bool clock_setting_save(unsigned mhz) {
    if (!supported(mhz))
        return false;
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof page);
    record r = {MAGIC, mhz, ~mhz};
    memcpy(page, &r, sizeof r);
    running_mhz = (clock_get_hz(clk_sys) + 500000) / 1000000;
    if (flash_safe_execute(write_sector, page, 1000) != PICO_OK)
        return false;
    watchdog_hw->scratch[0] = 0;  // a new setting gets its own tries
    return clock_setting_saved() == mhz;
}
