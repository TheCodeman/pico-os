/**
 * @file usb_hub_kick.h
 * @brief Works around devices that stay attached to a hub across a soft reset.
 *
 * TinyUSB learns about a hub's devices only from the hub's status-change
 * interrupt endpoint. After a power cycle that works, but after a reset of
 * the Pico alone (watchdog, reset button) the hub and its devices keep their
 * power: the ports still show their connection change from before, yet the
 * interrupt endpoint never delivers it, so the devices are never set up.
 *
 * Every second, while USB has been quiet for 2 s and nothing is being set up
 * or transferring to the drive, this reads each hub port's status over the
 * control pipe, so plugging and unplugging work even when the hub's status
 * changes don't arrive:
 * - a mounted device whose port shows it gone (or replugged) is reported to
 *   TinyUSB as removed;
 * - a device that stays connected without being mounted (2 scans if its port
 *   isn't enabled, 8 if an enumeration seems to have stalled) is one nobody
 *   is handling, so this does what TinyUSB's hub driver would have: clears
 *   the change, resets the port and posts an attach event, one port at a
 *   time, waiting for each device to mount.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Core 1: call from the USB loop, after tuh_task().
void usb_hub_kick_task(void);

/// Core 1: true while one of this module's control transfers is in flight.
/// Bulk transfers must wait for it (see usb_storage_task()).
/// @return true while a control transfer of ours is in flight
bool usb_hub_kick_busy(void);

/// Called from tuh_mount_cb() / tuh_umount_cb() for every device.
/// @param dev_addr the device's address
void usb_hub_kick_device_mounted(uint8_t dev_addr);
/// Called from tuh_umount_cb() for every device.
/// @param dev_addr the device's address
void usb_hub_kick_device_unmounted(uint8_t dev_addr);

/// Core 1: true when USB has got stuck (a request the hub never answered, or a
/// drive transfer that never finished) and should be started again:
/// tuh_deinit(), tuh_init(), then usb_hub_kick_reset(). Every device is then
/// set up afresh; anything waiting on the old ones fails.
/// @return true when USB should be started again
bool usb_restart_wanted(void);
/// Asks for USB to be started again (see usb_restart_wanted).
void usb_request_restart(void);
/// Forgets everything about the devices (after tuh_init).
void usb_hub_kick_reset(void);
