/**
 * @file c64_d64.h
 * @brief 1541 disk images (.d64, 35 tracks) read at the file level, for the KERNAL LOAD trap: finding a file by name (with * and ?
 *
 * patterns) and making the directory into the BASIC program LOAD"$",8 gives,
 * and saving and deleting files. Portable C.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C64_D64_SIZE 174848  ///< 683 sectors; images with error bytes are longer

/// True if size is a .d64 image's (with or without the error bytes).
/// @param size the file's size
/// @return true if it is a .d64's
bool c64_d64_size_ok(size_t size);

/// Whether a 16-character name (padded with $A0) matches what was typed:
/// PETSCII, "*" matches the rest and "?" any character; a "0:" in front is
/// dropped.
/// @param typed the name typed (PETSCII)
/// @param len its length
/// @param name the directory entry's name
/// @return true if it matches
bool c64_name_match(const uint8_t *typed, int len, const uint8_t name[16]);

/// Loads the first PRG file (or any type, if none is a PRG) matching the name
/// into buf (its two address bytes first, up to 0x10002 bytes); returns its
/// length, or -1 if there's none.
/// @param img the .d64 image
/// @param size its size
/// @param typed the name typed
/// @param len its length
/// @param buf where the file goes
/// @return the file's length, or -1 if there's none
int c64_d64_load(const uint8_t *img, size_t size, const uint8_t *typed, int len, uint8_t *buf);

/// The directory as a BASIC program at $0401, into buf (with its address
/// bytes, up to 0x10002); returns its length.
/// @param img the .d64 image
/// @param size its size
/// @param buf where the program goes
/// @return its length
int c64_d64_directory(const uint8_t *img, size_t size, uint8_t *buf);

/// Writing: SAVE puts a PRG file on the disk (a file of the same name goes
/// first; "@0:" in front is allowed), returning 0 or one of these.
#define C64_D64_DISK_FULL   (-1)
/// c64_d64_save: the directory is full.
#define C64_D64_DIR_FULL    (-2)
/// c64_d64_save: the disk is full or the file couldn't be written.
#define C64_D64_SAVE_FAILED (-3)
/// Saves `n` bytes of `data` as PRG file `typed` (`len` bytes; a file of that name is replaced). 0, or C64_D64_DISK_FULL, _DIR_FULL or _SAVE_FAILED.
/// @param img the .d64 image
/// @param size its size
/// @param typed the file's name
/// @param len its length
/// @param data the file's bytes
/// @param n how many
/// @return 0 or a C64_D64_ error
int c64_d64_save(uint8_t *img, size_t size, const uint8_t *typed, int len, const uint8_t *data, int n);
/// Deletes the files matching the name; returns how many.
/// @param img the .d64 image
/// @param size its size
/// @param typed the name pattern
/// @param len its length
/// @return the files deleted
int c64_d64_scratch(uint8_t *img, size_t size, const uint8_t *typed, int len);
/// Free blocks on the .d64 image.
/// @param img the .d64 image
/// @param size its size
/// @return the free blocks
unsigned c64_d64_blocks_free(const uint8_t *img, size_t size);

/// Making a directory listing of anything (the board's folder too).
typedef struct {
    uint8_t *buf;  ///< the file's bytes
    int len;  ///< its length
    uint16_t addr;  ///< where the next line goes in the C64's memory
} c64_listing;
/// The heading line: the disk's name and its ID (16 and 5 characters, PETSCII).
/// @param l the listing being made
/// @param buf where it goes
/// @param name the disk's name
/// @param id the disk's ID
void c64_listing_start(c64_listing *l, uint8_t *buf, const uint8_t name[16], const uint8_t id[5]);
/// A file: its size in blocks, name (16, $A0-padded) and type ("PRG").
/// @param l the listing
/// @param blocks the file's size
/// @param name its name
/// @param type its type
void c64_listing_file(c64_listing *l, unsigned blocks, const uint8_t name[16], const char *type);
/// The last line, "BLOCKS FREE."; returns the program's length.
/// @param l the listing
/// @param free_blocks the blocks free
/// @return the program's length
int c64_listing_end(c64_listing *l, unsigned free_blocks);
