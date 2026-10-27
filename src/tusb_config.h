/**
 * @file tusb_config.h
 * @brief TinyUSB configuration: host only, on the RP2350's native USB port, with a hub, up to four HID interfaces (keyboards) and mass storage (flash drives).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

// Set to 1 to see TinyUSB's enumeration and transfer errors (printed by
// core 0 through usb_log_printf()).
#undef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#define CFG_TUSB_DEBUG_PRINTF usb_log_printf

/// TinyUSB's TU_ASSERT executes a breakpoint once a debugger has been attached
/// (DHCSR.C_DEBUGEN stays set after OpenOCD until a power cycle), and that
/// stops the core for good. Some of its asserts fire harmlessly here (e.g.
/// re-arming a hub's status endpoint that is already armed), so make a failed
/// assertion just return, as it does when no debugger was ever attached.
#define TU_ASSERT(...) TU_VERIFY(__VA_ARGS__)

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

#define CFG_TUH_ENABLED             1
#define CFG_TUH_RPI_PIO_USB         0
#define CFG_TUH_ENUMERATION_BUFSIZE 512  ///< gamepads' report descriptors can be long

/// Hubs with 7 or more ports usually have two hub chips inside.
#define CFG_TUH_HUB    2
#define CFG_TUH_HID    8  ///< a keyboard's interfaces, a mouse and two gamepads (and spares)
#define CFG_TUH_CDC    0
#define CFG_TUH_MSC    1
#define CFG_TUH_VENDOR 0

/// Up to two hubs plus keyboards and a flash drive, with room to spare.
#define CFG_TUH_DEVICE_MAX 8

#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#define CFG_TUH_MSC_MAXLUN 4

#endif
