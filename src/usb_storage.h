/**
 * @file usb_storage.h
 * @brief USB flash drives (mass storage) on the native USB port, directly or through the hub.
 *
 * TinyUSB runs on core 1; FatFs on core 0 reads and writes sectors through
 * the FatFs disk functions here, which hand each request to core 1 and wait
 * for it.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Core 1: call from the USB loop, after tuh_task().
void usb_storage_task(void);

/// Core 1: true while a drive transfer is queued or running. Control
/// transfers must wait: on the RP2350 host they share one endpoint with bulk
/// transfers.
/// @return true while a transfer is queued or running
bool usb_storage_busy(void);

/// Any core: true while a drive is attached and ready.
/// @return true while a drive is ready
bool usb_storage_ready(void);

/// Any core: capacity of the attached drive in 512-byte sectors (0 if none).
/// @return the sectors, or 0
uint32_t usb_storage_sectors(void);
