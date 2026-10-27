/**
 * @file sim4703.h
 * @brief A simulated Si4703 for PC tests: registers, tuning, seeking, stations and their RDS.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "si4703.h"

/// A simulated station.
typedef struct {
    uint32_t khz;      ///< its frequency
    uint8_t rssi;      ///< its signal, dBuV
    bool stereo;       ///< in stereo
    uint16_t pi;       ///< its PI code
    uint8_t pty;       ///< its programme type
    const char *ps;    ///< its name (8 characters)
    const char *rt;    ///< its radio text
} sim_station_t;

extern bool sim_present;        ///< the chip answers
extern int sim_resets;          ///< resets seen
extern int sim_bad_every;       ///< every n-th RDS group has an uncorrectable block (0: none)

/// Sets the stations on the air.
/// @param s the stations
/// @param n their count
void sim_set_stations(const sim_station_t *s, int n);

/// The bus callbacks for the simulated chip.
/// @return them
si4703_bus_t sim_bus(void);

/// The simulated chip's registers (for checks).
/// @return its 16 registers
const uint16_t *sim_regs(void);
