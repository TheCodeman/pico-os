/**
 * @file app_overlay.h
 * @brief One SRAM area that the running app has to itself: an app keeps no SRAM while it isn't running.
 *
 * An app's code that must run from SRAM (away from the flash/PSRAM cache)
 * and its big variables go in its own section with APP_OV_FUNC/APP_OV_DATA;
 * the linker places all these sections at the same address
 * (src/ld/section_extra_post_data.incl, an OVERLAY) and keeps each app's in
 * flash. app_overlay_load copies an app's in when it starts. The radio uses
 * the area for its decoder while it plays (as raw memory), and gives it up
 * when another app loads its own.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#if defined(__arm__)
/// Puts function `f` in `app`'s part of the shared SRAM area.
#define APP_OV_FUNC(app, f) __attribute__((section("app_ov_" #app "." #f))) f
/// Puts a variable in `app`'s part of the shared SRAM area (set up again each time the app starts).
#define APP_OV_DATA(app)    __attribute__((section("app_ov_" #app ".data")))
#else  // PC builds of the apps
/// PC builds: functions stay where they are.
#define APP_OV_FUNC(app, f) f
/// PC builds: variables stay where they are.
#define APP_OV_DATA(app)
#endif

/// The users of the shared SRAM area.
enum { APP_OV_NONE, APP_OV_C64, APP_OV_APPLESOFT, APP_OV_APPLE2E, APP_OV_GENESIS, APP_OV_RADIO };

/// Makes the area the app's: stops whoever had it (the radio), then copies
/// the app's code and data in (zeroed variables included).
void app_overlay_load(int app);
/// The area as raw memory for `app` (the radio), and its size. `release` is
/// called if another app wants it.
void *app_overlay_claim(int app, size_t *size, void (*release)(void));
/// The app the shared area belongs to now (APP_OV_NONE if none).
int app_overlay_owner(void);
