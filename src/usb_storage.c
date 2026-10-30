/**
 * @file usb_storage.c
 * @brief Implementation of usb_storage.h: uSB flash drives (mass storage) on the native USB port, directly or through the hub.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "usb_storage.h"

#include <time.h>

#include "ff.h"  // before diskio.h, which uses its types
#include "diskio.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "tusb.h"
#include "usb_hub_kick.h"
#include "usb_log.h"

/// The sector size.
#define SECTOR_SIZE 512
/// A transfer that takes longer than this means the drive has stopped
/// answering (normal ones take milliseconds; a slow first write, a second).
#define TRANSFER_TIMEOUT_MS 10000

/// The first drive to attach is FatFs drive 0; others are ignored.
static volatile uint8_t drive_addr;  ///< 0 = no drive
/// Its size in sectors.
static volatile uint32_t drive_sectors;

/// One request at a time, from core 0 to core 1.
enum { REQ_IDLE, REQ_PENDING, REQ_BUSY, REQ_DONE };
static struct {
    bool write;  ///< a write
    uint8_t *buf;  ///< the data
    uint32_t lba;  ///< the first sector
    uint16_t count;  ///< the sector count
    bool ok;  ///< it succeeded
    volatile int state;  ///< REQ_IDLE and the like
} req;  ///< the request

/// TinyUSB: a drive attached.
/// @param dev_addr its address
void tuh_msc_mount_cb(uint8_t dev_addr) {
    uint32_t block_size = tuh_msc_get_block_size(dev_addr, 0);
    uint32_t blocks = tuh_msc_get_block_count(dev_addr, 0);
    usb_log_printf("[USB: storage at address %u, %lu blocks of %lu bytes]\n", dev_addr,
                   (unsigned long)blocks, (unsigned long)block_size);
    if (drive_addr) {
        usb_log_printf("[USB: ignoring it, a drive is already in use]\n");
        return;
    }
    if (block_size != SECTOR_SIZE) {
        usb_log_printf("[USB: ignoring it, only 512-byte blocks are supported]\n");
        return;
    }
    drive_sectors = blocks;
    __dmb();
    drive_addr = dev_addr;
}

/// TinyUSB: a drive gone.
/// @param dev_addr its address
void tuh_msc_umount_cb(uint8_t dev_addr) {
    if (dev_addr != drive_addr)
        return;
    drive_addr = 0;
    drive_sectors = 0;
    // Fail a transfer that was in flight so core 0 doesn't wait forever.
    if (req.state == REQ_PENDING || req.state == REQ_BUSY) {
        req.ok = false;
        __dmb();
        req.state = REQ_DONE;
    }
}

/// How the drive is being used, read over SWD when looking into speed.
/// Requests, sectors moved, and the largest request.
///@{
volatile uint32_t usb_storage_requests, usb_storage_sectors_moved, usb_storage_largest;
///@}
/// Microseconds waiting and transferring.
///@{
volatile uint32_t usb_storage_wait_us, usb_storage_xfer_us;
///@}
/// When the transfer started.
static uint32_t xfer_start_us;

/// When the request times out.
static absolute_time_t req_deadline;
static spin_lock_t *req_lock;  ///< guards PENDING -> BUSY / IDLE

/// The drive has stopped answering in the middle of a transfer. TinyUSB can't
/// cancel a single transfer on the RP2350 (stopping one behind its back has
/// panicked it), so USB is started again from scratch (see main.c): that
/// fails the transfer, the shell carries on, and the drive is set up anew.
static void report_stuck_drive(void) {
    usb_log_printf("[USB: the drive has stopped answering; restarting USB]\n");
    usb_request_restart();
}

/// TinyUSB: a transfer finished.
/// @param dev_addr its address
/// @param cb_data the result
/// @return true
static bool transfer_done(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    usb_storage_xfer_us += time_us_32() - xfer_start_us;
    req.ok = cb_data->csw->status == MSC_CSW_STATUS_PASSED;
    __dmb();
    req.state = REQ_DONE;
    return true;
}

void usb_storage_task(void) {
    static bool reported;
    if (req.state == REQ_BUSY && time_reached(req_deadline)) {
        if (!reported)
            report_stuck_drive();
        reported = true;
        return;
    }
    if (req.state != REQ_BUSY)
        reported = false;
    // The RP2350 host does control and bulk transfers on one endpoint and
    // TinyUSB panics if they overlap, so wait while the hub check has a
    // control transfer in flight.
    if (req.state != REQ_PENDING || usb_hub_kick_busy())
        return;
    // Core 0 may give up on a request that hasn't started (see transfer()),
    // so claim it under the lock.
    uint32_t save = spin_lock_blocking(req_lock);
    bool mine = req.state == REQ_PENDING;
    if (mine)
        req.state = REQ_BUSY;
    spin_unlock(req_lock, save);
    if (!mine)
        return;
    uint8_t addr = drive_addr;
    bool started = false;
    if (addr) {
        req_deadline = make_timeout_time_ms(TRANSFER_TIMEOUT_MS);
        xfer_start_us = time_us_32();
        started = req.write
                      ? tuh_msc_write10(addr, 0, req.buf, req.lba, req.count, transfer_done, 0)
                      : tuh_msc_read10(addr, 0, req.buf, req.lba, req.count, transfer_done, 0);
    }
    if (!started) {
        req.ok = false;
        __dmb();
        req.state = REQ_DONE;
    }
}

bool usb_storage_busy(void) {
    uint8_t addr = drive_addr;
    return req.state != REQ_IDLE || (addr && !tuh_msc_ready(addr));
}

bool usb_storage_ready(void) {
    return drive_addr != 0;
}

uint32_t usb_storage_sectors(void) {
    return drive_sectors;
}

/// Core 0: hands a request to core 1 and waits. Interrupts stay enabled, so
/// the HDMI scanout carries on while it waits.
/// A request USB doesn't even start within this long (the hub check stuck
/// waiting for the hub, say) fails, rather than hanging the shell.
#define START_TIMEOUT_MS 5000

/// A read or write, done on core 1.
/// @param write write
/// @param buf the data
/// @param lba the first sector
/// @param count the sector count
/// @return it worked
static bool transfer(bool write, const BYTE *buf, LBA_t lba, UINT count) {
    if (!drive_addr || count == 0 || count > UINT16_MAX)
        return false;
    if (!req_lock)
        req_lock = spin_lock_instance(spin_lock_claim_unused(true));
    req.write = write;
    req.buf = (uint8_t *)buf;
    req.lba = (uint32_t)lba;
    req.count = (uint16_t)count;
    usb_storage_requests++;
    usb_storage_sectors_moved += count;
    if (count > usb_storage_largest)
        usb_storage_largest = count;
    uint32_t wait0 = time_us_32();
    __dmb();
    req.state = REQ_PENDING;
    absolute_time_t give_up = make_timeout_time_ms(START_TIMEOUT_MS);
    bool told = false;
    while (req.state != REQ_DONE) {
        if (time_reached(give_up)) {
            uint32_t save = spin_lock_blocking(req_lock);
            bool unstarted = req.state == REQ_PENDING;
            if (unstarted)
                req.state = REQ_IDLE;
            spin_unlock(req_lock, save);
            if (unstarted) {
                printf("[USB: the drive can't be reached right now (the hub isn't answering)]\n");
                return false;
            }
            // Under way but not finishing: say so (the main loop, which
            // normally prints USB messages, is waiting here too).
            if (!told && absolute_time_diff_us(give_up, get_absolute_time()) >
                             (TRANSFER_TIMEOUT_MS - START_TIMEOUT_MS) * 1000ll) {
                printf("[USB: the drive has stopped answering; unplug it and plug it back in]\n");
                told = true;
            }
        }
        tight_loop_contents();
    }
    __dmb();
    usb_storage_wait_us += time_us_32() - wait0;
    bool ok = req.ok;
    req.state = REQ_IDLE;
    return ok;
}

// FatFs disk functions. Only drive 0 exists.

/// FatFs: the drive's status.
/// @param pdrv the drive
/// @return 0 or STA_NOINIT
DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && drive_addr ? 0 : STA_NOINIT;
}

/// FatFs: starts the drive.
/// @param pdrv the drive
/// @return its status
DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

/// FatFs: reads sectors.
/// @param pdrv the drive
/// @param buff where
/// @param sector the first
/// @param count how many
/// @return RES_OK or an error
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !drive_addr)
        return RES_NOTRDY;
    return transfer(false, buff, sector, count) ? RES_OK : RES_ERROR;
}

#if !FF_FS_READONLY
/// FatFs: writes sectors.
/// @param pdrv the drive
/// @param buff the data
/// @param sector the first
/// @param count how many
/// @return RES_OK or an error
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !drive_addr)
        return RES_NOTRDY;
    return transfer(true, buff, sector, count) ? RES_OK : RES_ERROR;
}
#endif

/// FatFs: sync, sizes and the like.
/// @param pdrv the drive
/// @param cmd the command
/// @param buff its data
/// @return RES_OK or an error
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || !drive_addr)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;  // writes complete before disk_write returns
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = drive_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;  // erase block size unknown
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

#if !FF_FS_NORTC && !FF_FS_READONLY
/// File dates come from the clock (local time, see tz in net/clock.c), or 2026-01-01
/// 00:00 until it has been set.
/// @return the FAT time
DWORD get_fattime(void) {
    time_t now = time(NULL);
    if (now < 1700000000)
        return (DWORD)(2026 - 1980) << 25 | (DWORD)1 << 21 | (DWORD)1 << 16;
    struct tm tm;
    localtime_r(&now, &tm);
    return (DWORD)(tm.tm_year - 80) << 25 | (DWORD)(tm.tm_mon + 1) << 21 |
           (DWORD)tm.tm_mday << 16 | (DWORD)tm.tm_hour << 11 | (DWORD)tm.tm_min << 5 |
           (DWORD)(tm.tm_sec / 2);
}
#endif
