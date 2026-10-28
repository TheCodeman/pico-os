/**
 * @file rtl_usb.c
 * @brief The USB side of an RTL2832U dongle: see rtl_usb.h.
 *
 * TinyUSB finds this driver through usbh_app_driver_get_cb. It takes
 * interface 0 of a dongle with a known vendor and product ID and opens its
 * first bulk IN endpoint (0x81), on which the samples come.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "rtl_usb.h"

#include <string.h>

#include "hardware/sync.h"
#include "pico/time.h"
#include "tusb.h"
#include "host/usbh_pvt.h"  // after tusb.h: the class driver interface
#include "usb_hub_kick.h"
#include "usb_log.h"

#define CONTROL_TIMEOUT_MS 500  ///< a control request that takes longer has failed
#define STOP_TIMEOUT_MS    300  ///< the last bulk transfer should be done by then

/// The dongles known (RTL2832U, or RTL2838 in the OEM ones).
static const uint16_t known[][2] = {
    {0x0bda, 0x2832}, {0x0bda, 0x2838}, {0x0413, 0x6680}, {0x0413, 0x6f0f}, {0x0458, 0x707f},
    {0x0ccd, 0x00a9}, {0x0ccd, 0x00b3}, {0x0ccd, 0x00d3}, {0x0ccd, 0x00d7}, {0x0ccd, 0x00e0},
    {0x1554, 0x5020}, {0x15f4, 0x0131}, {0x15f4, 0x0133}, {0x185b, 0x0620}, {0x185b, 0x0650},
    {0x185b, 0x0680}, {0x1b80, 0xd393}, {0x1b80, 0xd394}, {0x1b80, 0xd395}, {0x1b80, 0xd397},
    {0x1b80, 0xd398}, {0x1b80, 0xd39d}, {0x1b80, 0xd3a4}, {0x1b80, 0xd3a8}, {0x1b80, 0xd3af},
    {0x1b80, 0xd3b0}, {0x1d19, 0x1101}, {0x1d19, 0x1102}, {0x1d19, 0x1103}, {0x1d19, 0x1104},
    {0x1f4d, 0xa803}, {0x1f4d, 0xb803}, {0x1f4d, 0xc803}, {0x1f4d, 0xd286}, {0x1f4d, 0xd803},
};

/// The dongle (address 0: none).
static volatile uint8_t dev_addr;
static uint8_t itf_num;       ///< its interface
static uint8_t ep_in;         ///< its bulk IN endpoint
static uint16_t ep_mps;       ///< that endpoint's packet size
static volatile bool ready;   ///< configured: requests may go
/// Its IDs.
///@{
static uint16_t dev_vid, dev_pid;
///@}

/// The control request mailbox, from core 0 to core 1.
enum { CTL_IDLE, CTL_PENDING, CTL_BUSY, CTL_DONE };
static volatile int ctl_state;            ///< CTL_*
static tusb_control_request_t ctl_setup;  ///< the request waiting
static uint8_t ctl_buf[64];               ///< its data
static volatile int ctl_result;           ///< bytes moved, or -1

/// The stream.
static uint8_t *ring;                    ///< RTL_SLOTS buffers (core 0's)
static uint16_t slot_len[RTL_SLOTS];     ///< each buffer's bytes
static volatile uint32_t slots_in;       ///< buffers ever filled
static volatile uint32_t slots_out;      ///< buffers ever given back
static volatile bool streaming;          ///< bulk transfers wanted
static volatile bool held;               ///< but not now
static volatile bool in_flight;          ///< a bulk transfer is running
static volatile uint32_t overruns;       ///< buffers lost: the ring was full
static volatile uint32_t total_bytes;    ///< bytes received

/// A known dongle?
/// @param vid its vendor ID
/// @param pid its product ID
/// @return true if it is one
static bool is_known(uint16_t vid, uint16_t pid) {
    for (unsigned i = 0; i < sizeof known / sizeof *known; ++i)
        if (known[i][0] == vid && known[i][1] == pid)
            return true;
    return false;
}

/// Core 1: starts the next bulk transfer if one is wanted and a buffer is free.
static void submit(void) {
    uint8_t a = dev_addr;
    if (!a || !ready || !streaming || held || in_flight || slots_in - slots_out >= RTL_SLOTS)
        return;
    if (!usbh_edpt_claim(a, ep_in))
        return;
    in_flight = true;
    if (!usbh_edpt_xfer(a, ep_in, ring + (slots_in % RTL_SLOTS) * RTL_SLOT_BYTES, RTL_SLOT_BYTES)) {
        in_flight = false;
        usbh_edpt_release(a, ep_in);
    }
}

// ---- The class driver (core 1, from tuh_task) -----------------------------------------------------

/// TinyUSB: the driver starts.
/// @return true
static bool drv_init(void) {
    dev_addr = 0;
    ready = false;
    return true;
}

/// TinyUSB: the driver stops.
/// @return true
static bool drv_deinit(void) {
    dev_addr = 0;
    ready = false;
    return true;
}

/// TinyUSB: an interface of a new device; ours if it is a dongle's first.
/// @param rhport the port
/// @param daddr the device's address
/// @param itf the interface's descriptor
/// @param max_len the bytes of descriptors from it
/// @return true if it is taken
static bool drv_open(uint8_t rhport, uint8_t daddr, tusb_desc_interface_t const *itf, uint16_t max_len) {
    (void)rhport;
    uint16_t vid, pid;
    if (dev_addr || itf->bInterfaceNumber != 0 || itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
        !tuh_vid_pid_get(daddr, &vid, &pid) || !is_known(vid, pid))
        return false;
    // Find the first bulk IN endpoint among the interface's descriptors.
    uint8_t const *p = (uint8_t const *)itf, *end = p + max_len;
    p = tu_desc_next(p);
    while (p < end && tu_desc_type(p) != TUSB_DESC_INTERFACE) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            if (ep->bmAttributes.xfer == TUSB_XFER_BULK && tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                if (!tuh_edpt_open(daddr, ep))
                    return false;
                ep_in = ep->bEndpointAddress;
                ep_mps = tu_edpt_packet_size(ep);
                itf_num = itf->bInterfaceNumber;
                dev_vid = vid;
                dev_pid = pid;
                in_flight = false;
                streaming = false;
                dev_addr = daddr;
                usb_log_printf("[USB: RTL-SDR dongle %04x:%04x, bulk endpoint %02x, %u-byte packets]\n", vid,
                               pid, ep_in, ep_mps);
                return true;
            }
        }
        p = tu_desc_next(p);
    }
    return false;
}

/// TinyUSB: the device is configured.
/// @param daddr its address
/// @param itf the interface
/// @return true
static bool drv_set_config(uint8_t daddr, uint8_t itf) {
    if (daddr == dev_addr)
        ready = true;
    usbh_driver_set_config_complete(daddr, itf);
    return true;
}

/// TinyUSB: a bulk transfer finished.
/// @param daddr the device
/// @param ep_addr the endpoint
/// @param result how it went
/// @param xferred the bytes received
/// @return true
static bool drv_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred) {
    if (daddr != dev_addr || ep_addr != ep_in)
        return true;
    in_flight = false;
    if (result == XFER_RESULT_SUCCESS && xferred) {
        slot_len[slots_in % RTL_SLOTS] = (uint16_t)xferred;
        total_bytes = total_bytes + xferred;
        __dmb();
        slots_in = slots_in + 1;
        if (slots_in - slots_out >= RTL_SLOTS)
            overruns = overruns + 1;  // the next one will have to wait for core 0
    }
    submit();
    return true;
}

/// TinyUSB: the device went.
/// @param daddr its address
static void drv_close(uint8_t daddr) {
    if (daddr != dev_addr)
        return;
    dev_addr = 0;
    ready = false;
    in_flight = false;
    streaming = false;
    if (ctl_state == CTL_BUSY) {
        ctl_result = -1;
        __dmb();
        ctl_state = CTL_DONE;
    }
    usb_log_printf("[USB: RTL-SDR dongle unplugged]\n");
}

/// The driver's table.
static const usbh_class_driver_t rtl_driver = {
    .name = "RTL-SDR",
    .init = drv_init,
    .deinit = drv_deinit,
    .open = drv_open,
    .set_config = drv_set_config,
    .xfer_cb = drv_xfer_cb,
    .close = drv_close,
};

/// TinyUSB: the application's own class drivers.
/// @param driver_count set to their number
/// @return them
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &rtl_driver;
}

// ---- Control requests -------------------------------------------------------------------------------

/// Core 1: a control request finished.
/// @param xfer it
static void ctl_complete(tuh_xfer_t *xfer) {
    ctl_result = xfer->result == XFER_RESULT_SUCCESS ? (int)xfer->actual_len : -1;
    __dmb();
    ctl_state = CTL_DONE;
}

void rtl_usb_job(void) {
    if (ctl_state == CTL_PENDING) {
        uint8_t a = dev_addr;
        if (!a || !ready) {
            ctl_result = -1;
            __dmb();
            ctl_state = CTL_DONE;
        } else if (!usb_hub_kick_busy()) {
            tuh_xfer_t x = {
                .daddr = a,
                .ep_addr = 0,
                .setup = &ctl_setup,
                .buffer = ctl_setup.wLength ? ctl_buf : NULL,
                .complete_cb = ctl_complete,
                .user_data = 0,
            };
            ctl_state = CTL_BUSY;  // before: it may finish inside the call
            if (!tuh_control_xfer(&x))
                ctl_state = CTL_PENDING;  // the control pipe is busy: next time
        }
    }
    submit();
}

int rtl_usb_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len) {
    if (len > sizeof ctl_buf || !dev_addr)
        return -1;
    absolute_time_t until = make_timeout_time_ms(CONTROL_TIMEOUT_MS);
    // One that timed out before may still finish: wait for it.
    while (ctl_state == CTL_BUSY && !time_reached(until))
        tight_loop_contents();
    if (ctl_state == CTL_BUSY)
        return -1;
    ctl_setup.bmRequestType = type;
    ctl_setup.bRequest = req;
    ctl_setup.wValue = value;
    ctl_setup.wIndex = index;
    ctl_setup.wLength = len;
    if (!(type & 0x80) && len)
        memcpy(ctl_buf, data, len);
    __dmb();
    ctl_state = CTL_PENDING;
    while (ctl_state != CTL_DONE && !time_reached(until))
        tight_loop_contents();
    uint32_t save = save_and_disable_interrupts();
    if (ctl_state == CTL_PENDING)
        ctl_state = CTL_IDLE;  // core 1 never took it
    restore_interrupts(save);
    if (ctl_state != CTL_DONE)
        return -1;  // still running: the next call waits for it
    int r = ctl_result;
    if (r > 0 && (type & 0x80))
        memcpy(data, ctl_buf, r < len ? r : len);
    ctl_state = CTL_IDLE;
    return r;
}

// ---- The stream ----------------------------------------------------------------------------------------

bool rtl_usb_present(void) {
    return dev_addr && ready;
}

void rtl_usb_ids(uint16_t *vid, uint16_t *pid) {
    *vid = dev_vid;
    *pid = dev_pid;
}

uint16_t rtl_usb_packet_size(void) {
    return ep_mps;
}

void rtl_usb_hold(bool on) {
    held = on;
}

bool rtl_usb_stream_start(uint8_t *buffers) {
    if (!rtl_usb_present())
        return false;
    ring = buffers;
    slots_in = slots_out = 0;
    overruns = total_bytes = 0;
    held = false;
    __dmb();
    streaming = true;
    return true;
}

void rtl_usb_stream_stop(void) {
    streaming = false;
    absolute_time_t until = make_timeout_time_ms(STOP_TIMEOUT_MS);
    while (in_flight && dev_addr && !time_reached(until))
        tight_loop_contents();
    if (in_flight && dev_addr) {
        // The dongle stopped sending with a transfer waiting: nothing else
        // could use the endpoint again, so start USB again.
        usb_log_printf("[USB: the RTL-SDR dongle stopped sending; restarting USB]\n");
        usb_request_restart();
    }
}

const uint8_t *rtl_usb_read(uint32_t *len) {
    if (slots_in == slots_out)
        return NULL;
    __dmb();
    uint32_t k = slots_out % RTL_SLOTS;
    *len = slot_len[k];
    return ring + k * RTL_SLOT_BYTES;
}

void rtl_usb_done(void) {
    if (slots_in != slots_out)
        slots_out = slots_out + 1;
}

uint32_t rtl_usb_overruns(void) {
    return overruns;
}

uint32_t rtl_usb_bytes(void) {
    return total_bytes;
}
