/**
 * @file sdr.h
 * @brief The `sdr` command: a receiver for an RTL-SDR dongle (sdr_ui.c).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The `sdr` shell command: `sdr [MHz] [wfm|nfm|am]` opens the receiver,
/// `sdr info` describes the dongle.
/// @param argc the arguments' count
/// @param argv the arguments
void sdr_command(int argc, char **argv);
