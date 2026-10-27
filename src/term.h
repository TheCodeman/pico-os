/**
 * @file term.h
 * @brief term: a serial terminal on COM1 (UART1) with a settings page.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// The term / serial shell command: term [baud] [format, e.g. 8N1].
void term_command(int argc, char **argv);
