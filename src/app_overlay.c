/**
 * @file app_overlay.c
 * @brief The apps' shared SRAM area: see app_overlay.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "app_overlay.h"

#include <string.h>

#include "hardware/sync.h"

/// From the linker (src/ld/section_extra_post_data.incl).
extern char __app_overlay_start[], __app_overlay_end[];
extern char __load_start_app_ov_c64[], __load_stop_app_ov_c64[];
extern char __load_start_app_ov_apple2[], __load_stop_app_ov_apple2[];
extern char __load_start_app_ov_genesis[], __load_stop_app_ov_genesis[];

static int owner = APP_OV_NONE;
static void (*owner_release)(void);

static void take(int app, void (*release)(void)) {
    if (owner_release && owner != app) {
        void (*r)(void) = owner_release;
        owner_release = NULL;
        r();  // e.g. the radio stops (and stops using the area)
    }
    owner = app;
    owner_release = release;
}

void app_overlay_load(int app) {
    take(app, NULL);
    const char *from = NULL, *to = NULL;
    switch (app) {
    case APP_OV_C64: from = __load_start_app_ov_c64, to = __load_stop_app_ov_c64; break;
    case APP_OV_APPLESOFT:  // the same 65C02 as the Apple IIe
    case APP_OV_APPLE2E: from = __load_start_app_ov_apple2, to = __load_stop_app_ov_apple2; break;
    case APP_OV_GENESIS: from = __load_start_app_ov_genesis, to = __load_stop_app_ov_genesis; break;
    }
    if (from)
        memcpy(__app_overlay_start, from, to - from);
    __dsb();
    __isb();
}

void *app_overlay_claim(int app, size_t *size, void (*release)(void)) {
    take(app, release);
    *size = __app_overlay_end - __app_overlay_start;
    return __app_overlay_start;
}

int app_overlay_owner(void) {
    return owner;
}
