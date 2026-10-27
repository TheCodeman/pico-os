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
void smb_cmd_mount(int argc, char **argv);
/// umount [/NET]
void smb_cmd_umount(int argc, char **argv);

// The path is on a mounted share (else the USB drive).
#include <stdbool.h>
/// True if `path` is on a mounted share (else the USB drive).
bool smb_path_on_share(const char *path);
/// The folders shares are mounted at ("/NET"), at most max of them.
int smb_mount_points(char (*points)[24], int max);
