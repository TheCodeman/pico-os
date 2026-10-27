/**
 * @file glob.h
 * @brief Wildcards for the file commands (cp, mv, rm, ls): * and ?
 *
 * in the last part of a path, any case, as in DOS (so *.* also matches names
 * with no dot).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "pico/stdlib.h"  // getchar_timeout_us, for glob_ask

/// The last part of the path has * or ?.
bool glob_wild(const char *path);

/// True if `name` matches `pattern` (`*` and `?`, case not minded).
bool glob_match(const char *pattern, const char *name);

/// The paths with the wildcards expanded (folders too if dirs; hidden and
/// system files never); a pattern that matches nothing says "No files match
/// ...". Returns the count; free the list with glob_free.
int glob_expand(char *const *args, int count, bool dirs, char ***out);
/// Frees a list from glob_list.
void glob_free(char **list, int n);

/// The two paths name the same file (from the current folder, any case).
bool glob_same(const char *a, const char *b);
/// inner is outer or somewhere inside it.
bool glob_inside(const char *inner, const char *outer);
/// An absolute path without . and .. parts.
void glob_absolute(const char *path, char *out, size_t size);

/// Prints the question and returns the key pressed, in lower case.
int glob_ask(const char *question);
