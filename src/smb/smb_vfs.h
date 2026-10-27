/**
 * @file smb_vfs.h
 * @brief Network shares in the file tree (see smb_vfs.c), and their commands.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// mount //server[:port]/share [/NET] [-u [domain\]user] [-g]; no arguments
/// lists the mounts.
/// @param argc the argument count
/// @param argv the arguments
void smb_cmd_mount(int argc, char **argv);
/// umount [/NET]
/// @param argc the argument count
/// @param argv the arguments
void smb_cmd_umount(int argc, char **argv);

// The path is on a mounted share (else the USB drive).
#include <stdbool.h>
/// True if `path` is on a mounted share (else the USB drive).
/// @param path the path
/// @return true if it is on a share
bool smb_path_on_share(const char *path);
/// The folders shares are mounted at ("/NET"), at most max of them.
/// @param points where the folders go
/// @param max room in points
/// @return how many
int smb_mount_points(char (*points)[24], int max);
