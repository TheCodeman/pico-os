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
/// @param path the path
/// @return true if its last part has wildcards
bool glob_wild(const char *path);

/// True if `name` matches `pattern` (`*` and `?`, case not minded).
/// @param pattern the pattern
/// @param name the name
/// @return true if it matches
bool glob_match(const char *pattern, const char *name);

/// The paths with the wildcards expanded (folders too if dirs; hidden and
/// system files never); a pattern that matches nothing says "No files match
/// ...". Returns the count; free the list with glob_free.
/// @param args the arguments
/// @param count how many
/// @param dirs match folders too
/// @param out set to the list made
/// @return how many paths are in it
int glob_expand(char *const *args, int count, bool dirs, char ***out);
/// Frees a list from glob_expand.
/// @param list the list
/// @param n its length
void glob_free(char **list, int n);

/// The two paths name the same file (from the current folder, any case).
/// @param a one path
/// @param b the other
/// @return true if they are the same file
bool glob_same(const char *a, const char *b);
/// inner is outer or somewhere inside it.
/// @param inner the inner path
/// @param outer the outer one
/// @return true if inner is outer or inside it
bool glob_inside(const char *inner, const char *outer);
/// An absolute path without . and .. parts.
/// @param path the path
/// @param out where the absolute path goes
/// @param size its size
void glob_absolute(const char *path, char *out, size_t size);

/// Prints the question and returns the key pressed, in lower case.
/// @param question the question
/// @return the key, in lower case
int glob_ask(const char *question);
