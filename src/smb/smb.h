/**
 * @file smb.h
 * @brief A small SMB2/SMB3 client: enough to use files on a Windows or Samba share (smb_vfs.c puts a share into the file tree).
 *
 * Dialects 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1 (with SHA-512 pre-authentication
 * integrity); NTLMv2 login (or guest); every request is signed when there is
 * a session key (HMAC-SHA256 for 2.x, AES-CMAC for 3.x). No encryption: a
 * share that demands it is refused with a message. Names are code page 437,
 * as FatFs gives them, with / or \ between folders.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// A connection to an SMB share (opaque).
typedef struct smb smb_t;

/// An open file or folder on the share.
typedef struct {
    uint8_t id[16];  ///< the server's file id
} smb_fid_t;

/// A file's size, times and attributes.
typedef struct {
    uint64_t size;  ///< the file's size in bytes
    uint32_t attrib;  ///< FILE_ATTRIBUTE_*: 0x01 read-only, 0x02 hidden, 0x04 system, 0x10 folder
    uint64_t mtime;   ///< last write, Windows FILETIME (100 ns since 1601)
} smb_info_t;

/// The directory attribute in smb_info_t.
#define SMB_ATTR_DIR 0x10

/// NTSTATUS values (0 is success) the rest of the code looks at.
#define SMB_OK                 0x00000000u
/// NTSTATUS: no more files in the listing.
#define SMB_NO_MORE_FILES      0x80000006u
/// NTSTATUS: end of file.
#define SMB_END_OF_FILE        0xC0000011u
/// NTSTATUS: no such file.
#define SMB_NOT_FOUND          0xC0000034u
/// NTSTATUS: no such folder.
#define SMB_PATH_NOT_FOUND     0xC000003Au
/// NTSTATUS: the name is taken.
#define SMB_NAME_COLLISION     0xC0000035u
/// NTSTATUS: access denied.
#define SMB_ACCESS_DENIED      0xC0000022u
/// NTSTATUS: it is a folder.
#define SMB_FILE_IS_DIRECTORY  0xC00000BAu
/// NTSTATUS: it is not a folder.
#define SMB_NOT_A_DIRECTORY    0xC0000103u
/// NTSTATUS: the folder is not empty.
#define SMB_DIR_NOT_EMPTY      0xC0000101u
/// NTSTATUS: another program has the file open.
#define SMB_SHARING_VIOLATION  0xC0000043u
/// NTSTATUS: the disk is full.
#define SMB_DISK_FULL          0xC000007Fu
/// NTSTATUS: the name isn't valid.
#define SMB_NAME_INVALID       0xC0000033u
#define SMB_CONNECTION_LOST    0xC000020Cu  ///< our own: the TCP connection is gone
#define SMB_CANCELLED          0xC0000120u  ///< our own: Ctrl-C or Esc
#define SMB_NOT_SUPPORTED_ENCRYPT 0xE0000001u  ///< our own: the share wants encryption

/// Open modes for smb_open: SMB_READ and/or SMB_WRITE, one disposition,
/// and SMB_DIRECTORY for a folder.
#define SMB_READ       0x01
/// smb_open: open for writing.
#define SMB_WRITE      0x02
/// smb_open: open a folder.
#define SMB_DIRECTORY  0x04
#define SMB_OPEN       0x00  ///< must exist
#define SMB_CREATE_NEW 0x10  ///< must not exist
#define SMB_TRUNCATE   0x20  ///< create or empty it
#define SMB_OPEN_ALWAYS 0x30  ///< create if missing

/// Connects and logs in (user "" for guest). NULL on failure, with the
/// reason in err.
/// @param host the server
/// @param port its port (445)
/// @param share the share's name
/// @param domain the domain, or ""
/// @param user the user ("" for guest)
/// @param password the password
/// @param err set to why, on failure
/// @param err_size its size
/// @return the connection, or NULL
smb_t *smb_connect(const char *host, uint16_t port, const char *share, const char *domain,
                   const char *user, const char *password, char *err, int err_size);
/// Logs off and closes the connection.
/// @param s the connection
void smb_disconnect(smb_t *s);
/// True while the connection is up.
/// @param s the connection
/// @return true while it is up
bool smb_alive(smb_t *s);
/// e.g. "SMB 3.1.1, signed".
/// @param s the connection
/// @param out where the text goes
/// @param size its size
void smb_describe(smb_t *s, char *out, int size);

/// Opens a file or folder (SMB_ mode bits); its info if `info` isn't NULL. 0 or an NTSTATUS.
/// @param s the connection
/// @param path the path on the share
/// @param mode SMB_ mode bits
/// @param fid set to the open file
/// @param info set to its info, or NULL
/// @return 0 or an NTSTATUS
uint32_t smb_open(smb_t *s, const char *path, int mode, smb_fid_t *fid, smb_info_t *info);
/// Closes a file or folder. 0 or an NTSTATUS.
/// @param s the connection
/// @param fid the file
/// @return 0 or an NTSTATUS
uint32_t smb_close(smb_t *s, const smb_fid_t *fid);
/// Reads up to `len` bytes at `offset` into `buf` (*got: how many). 0 or an NTSTATUS.
/// @param s the connection
/// @param fid the file
/// @param offset where to read
/// @param buf where the bytes go
/// @param len how many
/// @param got set to how many were read
/// @return 0 or an NTSTATUS
uint32_t smb_read(smb_t *s, const smb_fid_t *fid, uint64_t offset, void *buf, uint32_t len,
                  uint32_t *got);
/// Writes `len` bytes from `buf` at `offset` (*done: how many). 0 or an NTSTATUS.
/// @param s the connection
/// @param fid the file
/// @param offset where to write
/// @param buf the bytes
/// @param len how many
/// @param done set to how many were written
/// @return 0 or an NTSTATUS
uint32_t smb_write(smb_t *s, const smb_fid_t *fid, uint64_t offset, const void *buf, uint32_t len,
                   uint32_t *done);
/// Flushes the file to disk on the server. 0 or an NTSTATUS.
/// @param s the connection
/// @param fid the file
/// @return 0 or an NTSTATUS
uint32_t smb_flush(smb_t *s, const smb_fid_t *fid);

/// One batch of a folder listing (restart: from the beginning). Calls fn for
/// each entry ("." and ".." left out); SMB_NO_MORE_FILES at the end.
typedef void (*smb_entry_fn)(void *ctx, const char *name, const smb_info_t *info);
/// Lists one batch of a folder (see smb_entry_fn).
/// @param s the connection
/// @param dir the open folder
/// @param restart start from the beginning
/// @param fn called for each entry
/// @param ctx passed to fn
/// @return 0, SMB_NO_MORE_FILES at the end, or an NTSTATUS
uint32_t smb_list(smb_t *s, const smb_fid_t *dir, bool restart, smb_entry_fn fn, void *ctx);

/// A file's or folder's info. 0 or an NTSTATUS.
/// @param s the connection
/// @param path the path
/// @param info filled in
/// @return 0 or an NTSTATUS
uint32_t smb_stat(smb_t *s, const char *path, smb_info_t *info);
/// Makes a folder. 0 or an NTSTATUS.
/// @param s the connection
/// @param path the new folder
/// @return 0 or an NTSTATUS
uint32_t smb_mkdir(smb_t *s, const char *path);
/// @param s the connection
/// @param path the path
/// @return 0 or an NTSTATUS
uint32_t smb_delete(smb_t *s, const char *path);  ///< a file or an empty folder
/// Renames or moves a file or folder. 0 or an NTSTATUS.
/// @param s the connection
/// @param from the old path
/// @param to the new path
/// @return 0 or an NTSTATUS
uint32_t smb_rename(smb_t *s, const char *from, const char *to);

/// What an NTSTATUS means, for messages.
/// @param status the NTSTATUS
/// @return its description
const char *smb_status_text(uint32_t status);

/// From the port (smb_port_pico.c, or a test program): random bytes and the
/// time (seconds since 1970, 0 if not known).
/// @param buf where the bytes go
/// @param len how many
void smb_random(void *buf, size_t len);
/// The time now, in seconds since 1970 (0 if not known).
/// @return seconds since 1970, or 0
uint64_t smb_unix_time(void);
