/**
 * @file apple2e.h
 * @brief The apple2e shell command: an enhanced Apple IIe running its real ROM, with a Disk II card, on the HDMI screen, USB keyboard and drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// apple2e [-d folder] [disk1 [disk2]]: runs until F10. The ROMs are found in
/// folder (default /APPLE2E); disks are .dsk/.do (DOS order) or .po (ProDOS
/// order) 140 KB images, looked for as given and then in folder.
/// @param argc the argument count
/// @param argv the arguments, as the shell passes them
void apple2e_main(int argc, char **argv);
