/**
 * @file shell.h
 * @brief Command shell: a line editor with history, and file commands for the USB flash drive (FatFs drive 0:).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

#include "ff.h"

/// Shows the first prompt, once start-up is over. Until then messages print
/// without one, and drive messages take the start-up form.
void shell_start(void);

/// Runs a command line as if typed (for BASIC's SHELL). False for commands
/// that can't run from inside BASIC (basic itself, the editor).
/// @param line the command line
/// @return false for commands that can't run inside BASIC
bool shell_run_command(const char *line);

/// Runs any command line (mc uses it; the line may run basic or edit).
/// @param line the command line
void shell_exec(const char *line);

/// Rewrites the start-up screen's WiFi line with the current state, if it is
/// still on screen just above the first prompt. False if it isn't.
/// @return false if the line has scrolled away
bool shell_update_wifi_line(void);

/// Feeds one input character (keyboard or serial). Arrow and editing keys
/// arrive as ANSI escape sequences, which the shell decodes itself.
/// @param c the character
void shell_key(int c);

/// Wrap any output printed outside the shell (USB messages and so on):
/// begin moves to a fresh line, end redraws the prompt and the line being
/// typed.
void shell_async_begin(void);
/// Ends output printed outside the shell (see shell_async_begin).
void shell_async_end(void);

/// Short description of a FatFs result, for error messages.
/// @param r the result
/// @return its description
const char *fresult_name(FRESULT r);

/// The flash drive came or went. Mounts or unmounts it and says so.
/// @param present the drive is there now
void shell_drive_changed(bool present);

/// True while a script runs (run FILE, /AUTORUN): commands mustn't stop to
/// ask for typing (mount uses a saved login or gives up).
extern bool shell_batch;
/// Runs /AUTORUN if the drive has one (not if the key typed during start-up
/// was Esc). True if there was one: the start-up screen has scrolled on.
/// @param boot_key the key typed during start-up, or -1
/// @return true if /AUTORUN ran
bool shell_autorun(int boot_key);

/// True while the flash drive is mounted as 0:.
/// @return true while the drive is mounted
bool shell_drive_mounted(void);

/// Background work for shell commands (memtest soak); call from the main loop.
void shell_task(void);
