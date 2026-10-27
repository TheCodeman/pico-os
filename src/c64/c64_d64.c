/**
 * @file c64_d64.c
 * @brief Implementation of c64_d64.h: 1541 disk images (.d64, 35 tracks) read at the file level, for the KERNAL LOAD trap: finding a file by name (with * and ?
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "c64_d64.h"

#include <string.h>

/// Sectors per track: 21 on 1-17, 19 on 18-24, 18 on 25-30, 17 on 31-35.
/// @param track the track (1-35)
/// @return its sectors
static int sectors(int track) {
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

/// A sector of the image.
/// @param img the image
/// @param size its size
/// @param track the track
/// @param sec the sector
/// @return its 256 bytes, or NULL
static const uint8_t *sector(const uint8_t *img, size_t size, int track, int sec) {
    if (track < 1 || track > 35 || sec < 0 || sec >= sectors(track))
        return NULL;
    size_t at = 0;
    for (int t = 1; t < track; ++t)
        at += (size_t)sectors(t);
    at = (at + (size_t)sec) * 256;
    return at + 256 <= size ? img + at : NULL;
}

bool c64_d64_size_ok(size_t size) {
    return size == C64_D64_SIZE || size == C64_D64_SIZE + 683;
}

bool c64_name_match(const uint8_t *typed, int len, const uint8_t name[16]) {
    if (len >= 2 && typed[1] == ':' && (typed[0] == '0' || typed[0] == '1'))
        typed += 2, len -= 2;
    int i;
    for (i = 0; i < len && i < 16; ++i) {
        if (typed[i] == '*')
            return true;
        if (typed[i] != '?' && typed[i] != name[i])
            return false;
    }
    if (i < len)  // typed longer than 16: only a "*" can follow
        return typed[i] == '*';
    return i == 16 || name[i] == 0xa0;
}

/// Calls fn for each directory entry in use; stops when it returns true.
typedef bool (*entry_fn)(void *ctx, const uint8_t *entry);

/// Calls fn for each file in the directory until it returns true.
/// @param img the image
/// @param size its size
/// @param fn the function
/// @param ctx passed to fn
static void each_entry(const uint8_t *img, size_t size, entry_fn fn, void *ctx) {
    int track = 18, sec = 1;
    for (int n = 0; n < 19 && track; ++n) {
        const uint8_t *s = sector(img, size, track, sec);
        if (!s)
            return;
        for (int e = 0; e < 8; ++e) {
            const uint8_t *entry = s + e * 32;
            if ((entry[2] & 0x07) && (entry[2] & 0x80) && fn(ctx, entry))
                return;
        }
        track = s[0];
        sec = s[1];
    }
}

/// What find_entry looks for, and finds.
typedef struct {
    const uint8_t *typed;  ///< the name typed
    int len;  ///< its length
    bool want_prg;  ///< only PRG files
    const uint8_t *found;  ///< the entry found
} find_ctx;

/// each_entry's function for finding a file.
/// @param ctx the find_ctx
/// @param entry a directory entry
/// @return true when found
static bool find_entry(void *ctx, const uint8_t *entry) {
    find_ctx *f = ctx;
    if (f->want_prg && (entry[2] & 0x07) != 2)
        return false;
    if (!c64_name_match(f->typed, f->len, entry + 5))
        return false;
    f->found = entry;
    return true;
}

int c64_d64_load(const uint8_t *img, size_t size, const uint8_t *typed, int len, uint8_t *buf) {
    find_ctx f = {typed, len, true, NULL};
    each_entry(img, size, find_entry, &f);
    if (!f.found) {
        f.want_prg = false;
        each_entry(img, size, find_entry, &f);
    }
    if (!f.found)
        return -1;
    int track = f.found[3], sec = f.found[4], n = 0;
    for (int count = 0; track && count < 683; ++count) {
        const uint8_t *s = sector(img, size, track, sec);
        if (!s)
            break;
        int last = s[0] ? 255 : s[1];  // the last sector says how much of it is used
        int bytes = last - 1;
        if (bytes < 0)
            bytes = 0;
        if (n + bytes > 0x10002)
            bytes = 0x10002 - n;
        memcpy(buf + n, s + 2, (size_t)bytes);
        n += bytes;
        track = s[0];
        sec = s[1];
    }
    return n >= 2 ? n : -1;
}

// ---- Listings ------------------------------------------------------------------

/// Adds a byte to the listing.
/// @param l the listing
/// @param b the byte
static void put(c64_listing *l, uint8_t b) {
    if (l->len < 0x10002)
        l->buf[l->len++] = b;
}

/// Adds a string to the listing.
/// @param l the listing
/// @param s the string
static void put_str(c64_listing *l, const char *s) {
    while (*s)
        put(l, (uint8_t)*s++);
}

/// A line: its link (filled in when the next starts), number, and text.
/// @param l the listing
/// @param number the line's number
/// @return where its link is, for line_end
static int line_start(c64_listing *l, unsigned number) {
    int at = l->len;
    put(l, 0);
    put(l, 0);
    put(l, number & 0xff);
    put(l, (number >> 8) & 0xff);
    return at;
}

/// Ends a line and fills in its link.
/// @param l the listing
/// @param at where its link is
static void line_end(c64_listing *l, int at) {
    put(l, 0);
    l->addr = (uint16_t)(l->addr + (l->len - at));
    if (at + 1 < l->len) {
        l->buf[at] = l->addr & 0xff;
        l->buf[at + 1] = l->addr >> 8;
    }
}

/// Adds a quoted name padded to 16.
/// @param l the listing
/// @param name the name
static void put_name(c64_listing *l, const uint8_t name[16]) {
    int n = 0;
    put(l, '"');
    while (n < 16 && name[n] != 0xa0)
        put(l, name[n++]);
    put(l, '"');
    for (; n < 16; ++n)
        put(l, ' ');
}

void c64_listing_start(c64_listing *l, uint8_t *buf, const uint8_t name[16], const uint8_t id[5]) {
    l->buf = buf;
    l->len = 0;
    l->addr = 0x0401;
    put(l, 0x01);
    put(l, 0x04);
    int at = line_start(l, 0);
    put(l, 0x12);  // reverse on
    put(l, '"');  // the disk's name is padded inside its quotes
    for (int i = 0; i < 16; ++i)
        put(l, name[i] == 0xa0 ? ' ' : name[i]);
    put(l, '"');
    put(l, ' ');
    for (int i = 0; i < 5; ++i)
        put(l, id[i] == 0xa0 ? ' ' : id[i]);
    line_end(l, at);
}

void c64_listing_file(c64_listing *l, unsigned blocks, const uint8_t name[16], const char *type) {
    int at = line_start(l, blocks);
    // The names line up: LIST puts a space after the number, the line adds 3
    // under 10 blocks, 2 under 100 and 1 after.
    for (int n = blocks < 10 ? 3 : blocks < 100 ? 2 : 1; n > 0; --n)
        put(l, ' ');
    put_name(l, name);
    put(l, ' ');
    put_str(l, type);
    line_end(l, at);
}

int c64_listing_end(c64_listing *l, unsigned free_blocks) {
    int at = line_start(l, free_blocks);
    put_str(l, "BLOCKS FREE.");
    line_end(l, at);
    put(l, 0);
    put(l, 0);
    return l->len;
}

/// What list_entry adds to.
typedef struct {
    c64_listing *l;  ///< the listing
} list_ctx;

/// each_entry's function for the directory listing.
/// @param ctx the list_ctx
/// @param entry a directory entry
/// @return false: go on
static bool list_entry(void *ctx, const uint8_t *entry) {
    static const char *const types[] = {"DEL", "SEQ", "PRG", "USR", "REL", "???", "???", "???"};
    c64_listing *l = ((list_ctx *)ctx)->l;
    char type[5];
    memcpy(type, types[entry[2] & 7], 4);
    type[3] = (entry[2] & 0x40) ? '<' : 0;
    type[4] = 0;
    c64_listing_file(l, entry[30] | entry[31] << 8, entry + 5, type);
    return false;
}

int c64_d64_directory(const uint8_t *img, size_t size, uint8_t *buf) {
    static const uint8_t blank[16] = {0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0,
                                      0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0, 0xa0};
    const uint8_t *bam = sector(img, size, 18, 0);
    c64_listing l;
    c64_listing_start(&l, buf, bam ? bam + 0x90 : blank, bam ? bam + 0xa2 : blank);
    list_ctx ctx = {&l};
    each_entry(img, size, list_entry, &ctx);
    unsigned free_blocks = 0;
    for (int t = 1; bam && t <= 35; ++t)
        if (t != 18)
            free_blocks += bam[4 * t];
    return c64_listing_end(&l, free_blocks);
}

// ---- Writing ----------------------------------------------------------------------

/// A sector of the image, for writing.
/// @param img the image
/// @param size its size
/// @param track the track
/// @param sec the sector
/// @return its 256 bytes, or NULL
static uint8_t *sector_rw(uint8_t *img, size_t size, int track, int sec) {
    return (uint8_t *)sector(img, size, track, sec);
}

/// Whether a sector is free in the BAM.
/// @param bam the BAM sector
/// @param t the track
/// @param s the sector
/// @return true if free
static bool sector_free(const uint8_t *bam, int t, int s) {
    return bam[4 * t + 1 + s / 8] & (1 << (s % 8));
}

/// Marks a sector used or free in the BAM, keeping the track's count.
/// @param bam the BAM sector
/// @param t the track
/// @param s the sector
/// @param used used or free
static void mark(uint8_t *bam, int t, int s, bool used) {
    uint8_t *byte = &bam[4 * t + 1 + s / 8], bit = (uint8_t)(1 << (s % 8));
    if (used && (*byte & bit)) {
        *byte &= ~bit;
        --bam[4 * t];
    } else if (!used && !(*byte & bit)) {
        *byte |= bit;
        ++bam[4 * t];
    }
}

/// A free sector for a file: from track 17 outwards, as the 1541 does
/// (towards track 1, then from 19 up), never on 18 (the directory's).
/// @param bam the BAM sector
/// @param track set to the track
/// @param sec set to the sector
/// @return false if the disk is full
static bool alloc(uint8_t *bam, int *track, int *sec) {
    static const int8_t order[34] = {17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
                                     19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
    for (int i = 0; i < 34; ++i) {
        int t = order[i];
        if (!bam[4 * t])
            continue;
        for (int s = 0; s < sectors(t); ++s)
            if (sector_free(bam, t, s)) {
                mark(bam, t, s, true);
                *track = t;
                *sec = s;
                return true;
            }
    }
    return false;
}

unsigned c64_d64_blocks_free(const uint8_t *img, size_t size) {
    const uint8_t *bam = sector(img, size, 18, 0);
    unsigned n = 0;
    for (int t = 1; bam && t <= 35; ++t)
        if (t != 18)
            n += bam[4 * t];
    return n;
}

/// The name without a "0:" and as 16 characters padded with $A0.
/// @param typed the name typed
/// @param len its length
/// @param name set to the padded name
/// @return the name's length
static int typed_name(const uint8_t *typed, int len, uint8_t name[16]) {
    if (len >= 2 && typed[1] == ':' && (typed[0] == '0' || typed[0] == '1'))
        typed += 2, len -= 2;
    if (len > 16)
        len = 16;
    memset(name, 0xa0, 16);
    memcpy(name, typed, (size_t)len);
    return len;
}

int c64_d64_scratch(uint8_t *img, size_t size, const uint8_t *typed, int len) {
    uint8_t *bam = sector_rw(img, size, 18, 0);
    if (!bam)
        return 0;
    int gone = 0, track = 18, sec = 1;
    for (int n = 0; n < 19 && track; ++n) {
        uint8_t *s = sector_rw(img, size, track, sec);
        if (!s)
            break;
        for (int e = 0; e < 8; ++e) {
            uint8_t *entry = s + e * 32;
            if (!(entry[2] & 0x07) || !c64_name_match(typed, len, entry + 5))
                continue;
            int t = entry[3], c = entry[4];
            for (int k = 0; t && k < 683; ++k) {
                const uint8_t *f = sector(img, size, t, c);
                if (!f)
                    break;
                mark(bam, t, c, false);
                t = f[0];
                c = f[1];
            }
            entry[2] = 0;
            ++gone;
        }
        track = s[0];
        sec = s[1];
    }
    return gone;
}

int c64_d64_save(uint8_t *img, size_t size, const uint8_t *typed, int len, const uint8_t *data, int n) {
    uint8_t *bam = sector_rw(img, size, 18, 0);
    if (!bam || n < 2)
        return C64_D64_SAVE_FAILED;
    bool replace = len >= 1 && typed[0] == '@';
    if (replace) {
        ++typed, --len;
    }
    uint8_t name[16];
    int name_len = typed_name(typed, len, name);
    if (!name_len || memchr(name, '*', 16) || memchr(name, '?', 16))
        return C64_D64_SAVE_FAILED;
    // A file of that name goes first ("@0:" or not: drive 8 here overwrites).
    c64_d64_scratch(img, size, name, name_len);
    unsigned blocks = (unsigned)(n + 253) / 254;
    if (c64_d64_blocks_free(img, size) < blocks)
        return C64_D64_DISK_FULL;

    // A free directory entry, adding a directory sector on track 18 if
    // every one is used.
    uint8_t *entry = NULL;
    int track = 18, sec = 1;
    uint8_t *last = NULL;
    for (int k = 0; k < 19 && track && !entry; ++k) {
        uint8_t *s = sector_rw(img, size, track, sec);
        if (!s)
            return C64_D64_SAVE_FAILED;
        for (int e = 0; e < 8 && !entry; ++e)
            if (!(s[e * 32 + 2] & 0x07))
                entry = s + e * 32;
        last = s;
        track = s[0];
        sec = s[1];
    }
    if (!entry) {
        int ds = -1;
        for (int s = 1; s < sectors(18) && ds < 0; ++s)
            if (sector_free(bam, 18, s))
                ds = s;
        if (ds < 0 || !last)
            return C64_D64_DIR_FULL;
        mark(bam, 18, ds, true);
        last[0] = 18;
        last[1] = (uint8_t)ds;
        uint8_t *s = sector_rw(img, size, 18, ds);
        if (!s)
            return C64_D64_SAVE_FAILED;
        memset(s, 0, 256);
        s[1] = 0xff;
        entry = s;
    }

    // The data, 254 bytes a sector, each linked to the next.
    int first_t = 0, first_s = 0, prev_t = 0, prev_s = 0;
    for (int at = 0; at < n; at += 254) {
        int t, s;
        if (!alloc(bam, &t, &s))
            return C64_D64_DISK_FULL;  // can't happen: checked above
        if (prev_t) {
            uint8_t *p = sector_rw(img, size, prev_t, prev_s);
            if (!p)
                return C64_D64_SAVE_FAILED;
            p[0] = (uint8_t)t;
            p[1] = (uint8_t)s;
        } else {
            first_t = t;
            first_s = s;
        }
        uint8_t *d = sector_rw(img, size, t, s);
        if (!d)
            return C64_D64_SAVE_FAILED;
        int chunk = n - at < 254 ? n - at : 254;
        memset(d, 0, 256);
        memcpy(d + 2, data + at, (size_t)chunk);
        d[0] = 0;
        d[1] = (uint8_t)(chunk + 1);  // the last sector: the index of its last byte
        prev_t = t;
        prev_s = s;
    }
    uint8_t keep[2] = {entry[0], entry[1]};  // the first entry's bytes are the sector's link
    memset(entry, 0, 32);
    entry[0] = keep[0];
    entry[1] = keep[1];
    entry[2] = 0x82;  // PRG, closed
    entry[3] = (uint8_t)first_t;
    entry[4] = (uint8_t)first_s;
    memcpy(entry + 5, name, 16);
    entry[30] = blocks & 0xff;
    entry[31] = (uint8_t)(blocks >> 8);
    return 0;
}
