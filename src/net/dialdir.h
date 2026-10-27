/**
 * @file dialdir.h
 * @brief A BBS dialing directory: the <BBS name=".." ip=".." port=".." protocol=".."/> entries of an EtherTerm phonebook (dialdirectory.xml).
 *
 * Plain C, so it can be tested on a PC.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

/// One BBS of the directory: name, host, login name, port, protocol and favourite mark.
typedef struct {
    char name[40];  ///< the BBS's name
    char host[48];  ///< its host name or address
    char user[24];   ///< from "user@host" (SSH logins), else empty
    uint16_t port;  ///< its port
    uint8_t ssh;     ///< 0 telnet, 1 SSH
    uint8_t fav;     ///< set by the menu
} dial_entry_t;

/// The entries in text (0-terminated) into out (up to max; out NULL: just
/// counts them). Ones that aren't telnet or SSH, or have no host, are left
/// out and counted in *skipped.
/// @param text the XML, 0-terminated
/// @param out where the entries go, or NULL to count them
/// @param max room in out
/// @param skipped set to the entries left out, or NULL
/// @return the entries found
int dialdir_parse(const char *text, dial_entry_t *out, int max, int *skipped);
