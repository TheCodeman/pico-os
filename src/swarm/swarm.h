/**
 * @file swarm.h
 * @brief The `swarm` command: an arcade space shooter. Aliens fly in to a formation and dive at the fighter; flagships can capture it with a tractor beam, and shooting the flagship brings it back as a twin fighter.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The shell's swarm command: `swarm` plays; `swarm sprites [file.spr]`
/// writes the game's sprites as a .SPR file for the sprite editor (a
/// /SWARM.SPR is used in place of the built-in ones when the game starts).
/// @param argc the argument count
/// @param argv the arguments
void swarm_command(int argc, char **argv);
