/**
 * @file usb_log.c
 * @brief Implementation of usb_log.h: uSB diagnostics.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "usb_log.h"

#include <stdarg.h>
#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#include "hardware/sync.h"
#include "psram_heap.h"
#include "tusb.h"
#include "usb_hub_kick.h"

/// The text waiting for core 0 to show it: a ring in the heap at start-up,
/// in PSRAM once that has been checked (usb_log_use_psram), so the shell
/// keeps none of its SRAM.
#define LOG_SIZE 2048
/// The log (PSRAM).
static char *log_buf;
/// Characters ever put in and taken out.
///@{
static uint32_t log_head, log_tail;
///@}
static spin_lock_t *log_lock;        ///< core 1 writes, core 0 reads

void usb_log_init(void) {
    log_lock = spin_lock_init(spin_lock_claim_unused(true));
    log_buf = malloc(LOG_SIZE);
}

void usb_log_use_psram(void) {
    char *p = psram_malloc(LOG_SIZE);
    if (!p)
        return;
    uint32_t save = spin_lock_blocking(log_lock);
    char *old = log_buf;
    if (old)
        memcpy(p, old, LOG_SIZE);  // (the same places in the ring)
    log_buf = p;
    spin_unlock(log_lock, save);
    free(old);
}

int usb_log_printf(const char *format, ...) {
    char buf[160];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    uint32_t save = spin_lock_blocking(log_lock);
    for (int i = 0; log_buf && i < n && i < (int)sizeof buf - 1 && log_head - log_tail < LOG_SIZE; ++i)
        log_buf[log_head++ % LOG_SIZE] = buf[i];
    spin_unlock(log_lock, save);
    return n;
}

int usb_log_getc(void) {
    int c = -1;
    uint32_t save = spin_lock_blocking(log_lock);
    if (log_tail != log_head)
        c = (unsigned char)log_buf[log_tail++ % LOG_SIZE];
    spin_unlock(log_lock, save);
    return c;
}

/// Every device TinyUSB finishes enumerating, whatever its class.
/// @param dev_addr its address
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    usb_log_printf("[USB: device %04x:%04x connected at address %u]\n", vid, pid, dev_addr);
    usb_hub_kick_device_mounted(dev_addr);
}

/// TinyUSB: a device gone.
/// @param dev_addr its address
void tuh_umount_cb(uint8_t dev_addr) {
    usb_log_printf("[USB: address %u disconnected]\n", dev_addr);
    usb_hub_kick_device_unmounted(dev_addr);
}
