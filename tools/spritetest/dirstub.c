/**
 * @file dirstub.c
 * @brief Folders for the PC stand-in of FatFs (kept apart: dirent.h has its own DIR).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdio.h>

/// Opens a folder.
/// @param path it
/// @return it, or NULL
void *pc_opendir(const char *path) { return opendir(path); }

/// The next entry.
/// @param d the folder
/// @param name set to its name ("" at the end)
/// @param n room in name
/// @param is_dir set to whether it is a folder
void pc_readdir(void *d, char *name, int n, int *is_dir) {
    struct dirent *e = readdir(d);
    name[0] = 0;
    if (e) {
        snprintf(name, n, "%s", e->d_name);
        *is_dir = e->d_type == DT_DIR;
    }
}

/// Closes a folder.
/// @param d it
void pc_closedir(void *d) { closedir(d); }
