/**
 * @file c64_app.h
 * @brief The c64 shell command: a PAL Commodore 64 running its real ROMs, on the HDMI screen, USB keyboard and drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// c64 [-d folder] [-j 1|2] [file.prg]: runs until F10. The ROMs are found
/// in folder (default /C64); a PRG is looked for as given and then there.
void c64_main(int argc, char **argv);
