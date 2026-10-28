/**
 * @file rtl_usb.h
 * @brief The USB side of an RTL2832U dongle (RTL-SDR): a TinyUSB host class driver that claims it, vendor control requests from core 0, and its stream of IQ samples into a ring of buffers.
 *
 * TinyUSB runs on core 1, so everything here that talks to the dongle runs
 * there too: core 0 posts one control request at a time and waits for it
 * (rtl_usb_control), and rtl_usb_job, a core 1 job (core1_job.h), starts
 * it. The samples come on the dongle's bulk IN endpoint, one transfer of
 * RTL_SLOT_BYTES at a time into the next free slot of a ring that core 0
 * reads with rtl_usb_read and gives back with rtl_usb_done.
 *
 * The RP2350's USB host runs bulk transfers on the same hardware endpoint as
 * every control transfer (src/usb_patch), so a control request waits for the
 * bulk transfer in flight (at most RTL_SLOT_BYTES of samples, about 4 ms).
 * While several requests go together (tuning), rtl_usb_hold stops new bulk
 * transfers so that they run back to back; the samples meanwhile are lost.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RTL_SLOTS      8     ///< buffers in the sample ring
#define RTL_SLOT_BYTES 2048  ///< bytes in each (one bulk transfer): 1024 IQ pairs

/// A dongle is plugged in and set up (its driver has its interface).
/// @return true if there is one
bool rtl_usb_present(void);
/// The dongle's USB vendor and product IDs.
/// @param vid set to its vendor ID
/// @param pid set to its product ID
void rtl_usb_ids(uint16_t *vid, uint16_t *pid);
/// The bulk endpoint's packet size: 64 at the RP2350's full speed.
/// @return the endpoint's wMaxPacketSize
uint16_t rtl_usb_packet_size(void);

/// Core 1 (a core1_job_start job): starts a waiting control request and
/// keeps the bulk transfers going. Must be running while the dongle is used.
void rtl_usb_job(void);

/// Core 0: a control transfer, waiting for it to finish (up to 500 ms).
/// @param type bmRequestType (0x40 vendor out, 0xc0 vendor in, 0x80 standard in)
/// @param req bRequest
/// @param value wValue
/// @param index wIndex
/// @param data the bytes to send, or where the bytes read go
/// @param len how many (at most 64)
/// @return the bytes moved, or -1 if it failed or there is no dongle
int rtl_usb_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len);

/// Core 0: while on, no new bulk transfer starts (so control requests run
/// back to back); off lets the stream go on.
/// @param on hold the stream
void rtl_usb_hold(bool on);

/// Core 0: starts reading samples into `ring` (RTL_SLOTS x RTL_SLOT_BYTES).
/// The dongle must already be sending (its FIFO enabled), or the bulk
/// transfer would wait for ever and with it every control request.
/// @param ring the buffers (SRAM, as the USB interrupt copies into them)
/// @return false if there is no dongle
bool rtl_usb_stream_start(uint8_t *ring);
/// Core 0: stops starting transfers and waits for the one in flight (the
/// dongle must still be sending). USB is restarted if it never finishes.
void rtl_usb_stream_stop(void);
/// Core 0: the oldest filled buffer, or NULL if none has come.
/// @param len set to its length in bytes
/// @return the buffer (give it back with rtl_usb_done)
const uint8_t *rtl_usb_read(uint32_t *len);
/// Core 0: finished with the buffer rtl_usb_read gave.
void rtl_usb_done(void);
/// Buffers lost because core 0 had all of them (since stream start).
/// @return the count
uint32_t rtl_usb_overruns(void);
/// Bytes of samples received since stream start.
/// @return the count
uint32_t rtl_usb_bytes(void);
