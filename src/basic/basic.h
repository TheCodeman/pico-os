/**
 * @file basic.h
 * @brief A GW-BASIC style interpreter.
 *
 * Programs are kept as text lines in SRAM and interpreted directly; LOAD
 * reads plain-text and GW-BASIC tokenised files, SAVE writes plain text.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// Runs the BASIC prompt until SYSTEM (or BYE). If `file` is given it is
/// loaded and run first.
void basic_main(const char *file);

