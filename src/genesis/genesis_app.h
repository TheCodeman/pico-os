/**
 * @file genesis_app.h
 * @brief The genesis shell command: a Sega Genesis / Mega Drive on the HDMI screen, USB keyboard and gamepads, its cartridge read from the drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// genesis [-d folder] [rom]: runs rom until F10, or without one shows the
/// games in folder (default /GENESIS) to pick from (genesis_launcher.h),
/// coming back to the list after each until Esc or F10 there. F9 in a game
/// goes to the list. The ROM is looked for as given and then in folder.
/// @param argc the argument count
/// @param argv the arguments, as the shell passes them
void genesis_main(int argc, char **argv);
