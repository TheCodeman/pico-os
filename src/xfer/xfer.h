/**
 * @file xfer.h
 * @brief XMODEM, YMODEM and ZMODEM file transfer over a byte stream.
 *
 * The caller supplies the serial port, the files and the progress display,
 * so this has no hardware or FatFs in it (tools/xfertest runs it on a PC
 * against lrzsz).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The file transfer protocols.
typedef enum {
    XFER_XMODEM,     ///< 128-byte blocks; CRC-16, or a checksum if the receiver asks
    XFER_XMODEM_1K,  ///< 1024-byte blocks (receiving, X and X-1K are the same)
    XFER_YMODEM,     ///< batch: names and sizes are sent; 1024-byte blocks
    XFER_ZMODEM,     ///< streaming, batch, CRC-16/32, restart where it stopped
} xfer_proto_t;

#define XFER_TIMEOUT (-1)  ///< getc: nothing arrived in time
#define XFER_ABORT   (-2)  ///< getc: the user cancelled

/// A transfer's progress, for its window.
typedef struct {
    const char *name;  ///< the file being sent or received
    uint32_t done;     ///< bytes so far
    uint32_t size;     ///< its size, 0 if not known
    int errors;        ///< retries so far
    int files;         ///< files finished
} xfer_progress_t;

/// What a transfer needs from its caller: the line, the files and progress.
typedef struct {
    void *ctx;  ///< passed to each function
    /// A byte from the line, waiting up to timeout_ms (0: don't wait); or
    /// XFER_TIMEOUT, or XFER_ABORT when the user wants to stop.
    int (*getc)(void *ctx, unsigned timeout_ms);
    void (*write)(void *ctx, const uint8_t *data, int len);  ///< writes bytes to the line

    /// Files. open_read gives the size and the name to send (no folders).
    /// open_write gets the sender's name (folders already removed) and size
    /// (0 if unknown); `resume` asks to carry on with a partial copy. It sets
    /// *offset to where writing starts (the partial size, else 0) and
    /// saved_as to the name used. NULL: can't, skip the file.
    void *(*open_read)(void *ctx, const char *path, uint32_t *size, char *name, int name_size);
    /// Opens a file to receive into (see above).
    void *(*open_write)(void *ctx, const char *name, uint32_t size, bool resume, uint32_t *offset,
                        ///@{
                        char *saved_as, int saved_as_size);
                        ///@}
    int (*read)(void *ctx, void *file, uint8_t *buf, int len);  ///< count, <0 error
    bool (*seek)(void *ctx, void *file, uint32_t offset);  ///< moves to a place in a file being sent
    bool (*write_file)(void *ctx, void *file, const uint8_t *buf, int len);  ///< writes received bytes to a file
    /// ok: the file arrived whole (else a receiver may delete it).
    void (*close)(void *ctx, void *file, bool ok);

    void (*progress)(void *ctx, const xfer_progress_t *p);  ///< shows the progress (may be NULL)
} xfer_io_t;

/// How a transfer ended.
typedef struct {
    bool ok;  ///< true if every file got through
    int files;           ///< files sent or received whole
    uint32_t bytes;      ///< their bytes
    char message[100];   ///< what happened, for the user
} xfer_result_t;

/// Sends the files. XMODEM sends only the first (the receiver names it).
/// @param io the line, files and progress
/// @param proto the protocol
/// @param paths the files
/// @param count how many
/// @param result filled in
void xfer_send(const xfer_io_t *io, xfer_proto_t proto, const char *const *paths, int count,
               xfer_result_t *result);

/// Receives files. XMODEM needs a name for the one file; the others use the
/// sender's names.
/// @param io the line, files and progress
/// @param proto the protocol
/// @param xmodem_name the file to save as (XMODEM only)
/// @param result filled in
void xfer_receive(const xfer_io_t *io, xfer_proto_t proto, const char *xmodem_name,
                  xfer_result_t *result);

/// The protocol's name, e.g. "ZMODEM".
/// @param proto the protocol
/// @return its name
const char *xfer_proto_name(xfer_proto_t proto);
