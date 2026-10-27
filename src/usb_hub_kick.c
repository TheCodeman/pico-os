/**
 * @file usb_hub_kick.c
 * @brief Implementation of usb_hub_kick.h: works around devices that stay attached to a hub across a soft reset.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "usb_hub_kick.h"

#include <string.h>

#include "pico/time.h"
#include "tusb.h"
#include "host/hcd.h"  // after tusb.h, which these rely on
#include "host/hub.h"
#include "usb_log.h"
#include "usb_storage.h"

#define FIRST_HUB_ADDR (CFG_TUH_DEVICE_MAX + 1)  ///< TinyUSB gives hubs the top addresses
#define MAX_PORTS      7                          ///< one status-change byte's worth
#define SCAN_EVERY_MS  1000  ///< how often a hub's ports are checked
#define QUIET_MS       2000  ///< no mount/unmount/attach for this long before checking
#define MOUNT_WAIT_MS  6000  ///< longest to wait for one device before the next
#define STUCK_SCANS    8     ///< scans an enabled port's device may go unmounted
#define SETUP_TIMEOUT_MS 30000  ///< longest the checks wait for a device to finish setting up

static struct {
    bool mounted;
    uint8_t unmounted[MAX_PORTS + 1];  ///< scans in a row each port had a device but nothing mounted
    absolute_time_t next_scan;
} hubs[CFG_TUH_HUB];

enum { IDLE, GET_STATUS, CLEAR_CHANGE, RESET_PORT, WAIT_MOUNT };
static int step = IDLE;
static uint8_t hub;            ///< index into hubs[] of the hub being worked on
static uint8_t port;           ///< port the current step is on
static uint8_t occupied;       ///< ports with a mounted device behind them
static uint8_t todo;           ///< ports to hand to TinyUSB
static uint8_t gone;           ///< ports whose mounted device has been unplugged
static uint16_t gone_status[MAX_PORTS + 1];  ///< what the port showed, for the log
/// Scans in a row each port's mounted device has looked unplugged. One odd
/// reading alone isn't trusted: removing a device that is still there makes
/// it drop out and come back.
static uint8_t gone_seen[CFG_TUH_HUB][MAX_PORTS + 1];
static volatile bool waiting;  ///< a control transfer is in flight
static absolute_time_t wait_started;
/// Longest a hub may take to answer a status request. Sometimes one is never
/// answered, and TinyUSB has no timeout for it; then the only way on is to
/// start USB again (usb_restart_wanted()).
#define ANSWER_TIMEOUT_MS 3000
static volatile bool restart_wanted;
static volatile bool failed;   ///< ...and it failed (e.g. no such port)
static volatile uint32_t mounts;
static volatile uint16_t app_mounted;  ///< bit n: address n has had tuh_mount_cb
static volatile uint32_t last_activity_ms;
static uint32_t mounts_before;
static absolute_time_t wait_until;
static hub_port_status_response_t status;

static void xfer_done(tuh_xfer_t *xfer) {
    failed = xfer->result != XFER_RESULT_SUCCESS;
    waiting = false;
}

bool usb_hub_kick_busy(void) {
    return waiting;
}

void usb_hub_kick_device_mounted(uint8_t dev_addr) {
    app_mounted |= 1u << dev_addr;
    mounts = mounts + 1;
    last_activity_ms = to_ms_since_boot(get_absolute_time());
}

void usb_hub_kick_device_unmounted(uint8_t dev_addr) {
    app_mounted &= ~(1u << dev_addr);
    last_activity_ms = to_ms_since_boot(get_absolute_time());
}

static void post_remove(uint8_t h_addr, uint8_t p) {
    hcd_event_t event = {
        .rhport = 0,
        .event_id = HCD_EVENT_DEVICE_REMOVE,
        .connection = {.hub_addr = h_addr, .hub_port = p},
    };
    hcd_event_handler(&event, false);
}

/// TinyUSB marks a device configured before its class drivers have finished
/// setting it up (a flash drive's MSC driver does bulk transfers then), and
/// calls tuh_mount_cb only once they have. In between, a control transfer of
/// ours could land on top of a bulk one, so don't start any.
///
/// A device that never finishes (e.g. a drive that browned out half way)
/// would block the checks for good, and then an unplug or replug would never
/// be noticed. So after SETUP_TIMEOUT_MS stop waiting for it: the checks go
/// on, and if it is unplugged the usual path removes it. (Removing it by
/// force while TinyUSB is still enumerating it makes TinyUSB panic.)
static bool device_setting_up(void) {
    static absolute_time_t since[CFG_TUH_DEVICE_MAX + 1];
    static bool given_up[CFG_TUH_DEVICE_MAX + 1];
    bool any = false;
    for (uint8_t a = 1; a <= CFG_TUH_DEVICE_MAX; ++a) {
        if (!tuh_mounted(a) || (app_mounted & (1u << a))) {
            since[a] = nil_time;
            given_up[a] = false;
            continue;
        }
        if (is_nil_time(since[a]))
            since[a] = get_absolute_time();
        if (absolute_time_diff_us(since[a], get_absolute_time()) < SETUP_TIMEOUT_MS * 1000ll) {
            any = true;
        } else if (!given_up[a]) {
            given_up[a] = true;
            usb_log_printf("[USB: address %u is taking too long to set up]\n", a);
        }
    }
    return any;
}

static uint8_t hub_addr(void) {
    return FIRST_HUB_ADDR + hub;
}

static uint8_t next_todo(uint8_t p) {
    for (; p <= MAX_PORTS; ++p)
        if (todo & (1u << p))
            return p;
    return 0;
}

/// Starts a control transfer to the hub. If the control pipe is busy it
/// returns false and the step is simply tried again on the next call.
static void start(bool ok) {
    if (ok) {
        waiting = true;
        failed = false;
        wait_started = get_absolute_time();
    }
}

/// Ports of hub `h` that have a device TinyUSB has mounted.
static uint8_t mounted_ports(uint8_t h_addr) {
    uint8_t ports = 0;
    for (uint8_t a = 1; a <= CFG_TUH_DEVICE_MAX; ++a) {
        if (!tuh_mounted(a))
            continue;
        hcd_devtree_info_t info;
        hcd_devtree_get_info(a, &info);
        if (info.hub_addr == h_addr && info.hub_port <= MAX_PORTS)
            ports |= 1u << info.hub_port;
    }
    return ports;
}

static bool quiet(void) {
    return to_ms_since_boot(get_absolute_time()) - last_activity_ms >= QUIET_MS;
}

void usb_hub_kick_task(void) {
    for (int i = 0; i < CFG_TUH_HUB; ++i) {
        bool now = tuh_mounted(FIRST_HUB_ADDR + i);
        if (now && !hubs[i].mounted) {
            memset(hubs[i].unmounted, 0, sizeof hubs[i].unmounted);
            hubs[i].next_scan = make_timeout_time_ms(SCAN_EVERY_MS);
            last_activity_ms = to_ms_since_boot(get_absolute_time());
        }
        hubs[i].mounted = now;
        if (!now && step != IDLE && hub == i) {
            // The hub went away mid-sequence; its transfers die with it.
            step = IDLE;
            waiting = false;
        }
    }
    if (waiting && absolute_time_diff_us(wait_started, get_absolute_time()) >
                       ANSWER_TIMEOUT_MS * 1000ll) {
        usb_log_printf("[USB: the hub stopped answering; restarting USB]\n");
        restart_wanted = true;
        return;
    }
    if (waiting || (step != IDLE && step != WAIT_MOUNT && usb_storage_busy()))
        return;

    switch (step) {
    case IDLE:
        // Control transfers share the RP2350's one non-interrupt endpoint
        // with the flash drive's bulk transfers, so never start one while the
        // drive is busy (TinyUSB would panic).
        if (!quiet() || usb_storage_busy() || device_setting_up())
            return;
        for (int i = 0; i < CFG_TUH_HUB; ++i) {
            if (hubs[i].mounted && time_reached(hubs[i].next_scan)) {
                hubs[i].next_scan = make_timeout_time_ms(SCAN_EVERY_MS);
                hub = i;
                port = 1;
                todo = 0;
                gone = 0;
                occupied = mounted_ports(hub_addr());
                step = GET_STATUS;
                start(hub_port_get_status(hub_addr(), port, &status, xfer_done, 0));
                return;
            }
        }
        break;

    case GET_STATUS:
        if (failed) {
            port = MAX_PORTS + 1;  // past the hub's last port
        } else {
            // A device that has sat there unmounted for several quiet scans is
            // one nobody is handling: either its connection change was never
            // delivered (still pending, port not enabled), or its enumeration
            // stalled (port enabled). Either way, hand it to TinyUSB afresh;
            // an attach for a port TinyUSB is stuck on makes it start over.
            uint8_t *n = &hubs[hub].unmounted[port];
            uint8_t bit = 1u << port;
            // A mounted device whose port now shows nothing connected, or a
            // fresh connection change (unplugged and plugged back in), has
            // gone. TinyUSB's hub driver reports this too when the hub's
            // status changes arrive; a second remove for the port is harmless.
            bool looks_gone = !status.status.connection || status.change.connection;
            if ((occupied & bit) && looks_gone) {
                // A latched connection change is the hub's own record of an
                // unplug; a bare "not connected" must be seen twice running.
                if (status.change.connection || ++gone_seen[hub][port] >= 2) {
                    gone |= bit;
                    gone_seen[hub][port] = 0;
                }
                gone_status[port] = status.status.value | (status.change.connection ? 0x8000 : 0);
            } else {
                gone_seen[hub][port] = 0;
            }
            if (status.status.connection && !(occupied & bit)) {
                if (++*n >= (status.status.port_enable ? STUCK_SCANS : 2)) {
                    todo |= 1u << port;
                    *n = 0;
                }
            } else {
                *n = 0;
            }
            ++port;
        }
        if (port <= MAX_PORTS) {
            start(hub_port_get_status(hub_addr(), port, &status, xfer_done, 0));
            return;
        }
        for (uint8_t p = 1; p <= MAX_PORTS; ++p) {
            if (!(gone & (1u << p)))
                continue;
            // Same event TinyUSB's hub driver sends: unmounts whatever was on
            // the port (tuh_umount_cb and the class drivers' umount run).
            post_remove(hub_addr(), p);
            usb_log_printf("[USB: hub port %u: device unplugged (%s, status %04x)]\n", p,
                           gone_status[p] & 1 ? "reconnected" : "nothing connected",
                           gone_status[p]);
            last_activity_ms = to_ms_since_boot(get_absolute_time());
        }
        if (todo) {
            usb_log_printf("[USB: setting up the device%s on hub port", todo & (todo - 1) ? "s" : "");
            for (uint8_t p = 1; p <= MAX_PORTS; ++p)
                if (todo & (1u << p))
                    usb_log_printf(" %u", p);
            usb_log_printf(" that the hub didn't report]\n");
        }
        port = 0;
        step = WAIT_MOUNT;  // moves straight on to the first port below
        wait_until = get_absolute_time();
        mounts_before = mounts;
        break;

    case CLEAR_CHANGE:
        step = RESET_PORT;
        start(hub_port_reset(hub_addr(), port, xfer_done, 0));
        break;

    case RESET_PORT: {
        // The port reset is under way; TinyUSB's enumeration waits for it to
        // finish, exactly as after its own hub driver's reset.
        hcd_event_t event = {
            .rhport = 0,
            .event_id = HCD_EVENT_DEVICE_ATTACH,
            .connection = {.hub_addr = hub_addr(), .hub_port = port},
        };
        hcd_event_handler(&event, false);
        last_activity_ms = to_ms_since_boot(get_absolute_time());  // no scans mid-enumeration
        mounts_before = mounts;
        wait_until = make_timeout_time_ms(MOUNT_WAIT_MS);
        step = WAIT_MOUNT;
        break;
    }

    case WAIT_MOUNT:
        if (mounts == mounts_before && !time_reached(wait_until))
            return;
        port = next_todo(port + 1);
        if (!port) {
            last_activity_ms = to_ms_since_boot(get_absolute_time());
            step = IDLE;
            return;
        }
        step = CLEAR_CHANGE;
        start(hub_port_clear_feature(hub_addr(), port, HUB_FEATURE_PORT_CONNECTION_CHANGE,
                                     xfer_done, 0));
        break;
    }
}

bool usb_restart_wanted(void) {
    return restart_wanted;
}

void usb_request_restart(void) {
    restart_wanted = true;
}

void usb_hub_kick_reset(void) {
    restart_wanted = false;
    waiting = false;
    step = IDLE;
    memset(hubs, 0, sizeof hubs);
    memset(gone_seen, 0, sizeof gone_seen);
    last_activity_ms = to_ms_since_boot(get_absolute_time());
}
