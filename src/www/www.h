/**
 * @file www.h
 * @brief www: a text-mode web browser, in the manner of lynx.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The www (lynx) command: www [address or file].
/// @param argc the argument count
/// @param argv the arguments
void www_command(int argc, char **argv);
