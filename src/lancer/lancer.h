/**
 * @file lancer.h
 * @brief The `lancer` command: a side-scrolling space shooter. The ship flies through caves and fortresses with a beam it charges by holding fire and a pod it can launch and call back, and meets a boss at the end of each stage.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's lancer command: `lancer` plays; `lancer sprites [file.spr]`
/// writes the game's sprites as a .SPR file for the sprite editor (a
/// /LANCER.SPR is used in place of the built-in ones when the game starts).
/// @param argc the argument count
/// @param argv the arguments
void lancer_command(int argc, char **argv);
