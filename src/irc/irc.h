/**
 * @file irc.h
 * @brief The `irc` command: an IRC chat client in graphics.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The irc command: `irc` (the connect box, filled in from /IRC.CFG) or
/// `irc server [port] [nick] [#channel...]` (connects at once).
/// @param argc the argument count
/// @param argv the arguments
void irc_command(int argc, char **argv);
