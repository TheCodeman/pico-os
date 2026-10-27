/**
 * @file ssh.h
 * @brief A small SSH-2 client: enough to log in and run one command on a server (scp uses it to run "scp -t" / "scp -f" there).
 *
 * Key exchange curve25519-sha256 or ecdh-sha2-nistp256; host keys
 * ecdsa-sha2-nistp256, rsa-sha2-256 or rsa-sha2-512; cipher aes128-ctr or
 * aes256-ctr with hmac-sha2-256; no compression. Crypto is mbedTLS.
 * Host keys are checked against /SSH/KNOWN_HOSTS on the drive.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// An SSH session (opaque).
typedef struct ssh ssh_t;

/// Connects, checks the host key (asking about an unknown one), and logs in
/// as `user`, asking for the password on the console. Prints progress and
/// errors; Ctrl-C gives up. NULL on failure.
ssh_t *ssh_open(const char *host, uint16_t port, const char *user);

/// Opens a session channel running `command`. False (with a message) on
/// failure.
bool ssh_exec(ssh_t *s, const char *command);

/// Opens a terminal session: a pty of `term` type and size, then a shell
/// (command NULL) or the command. Its stdout and stderr both come through
/// ssh_poll, and Ctrl-C/Esc no longer stop waits (they go to the server).
bool ssh_shell(ssh_t *s, const char *term, int cols, int rows, const char *command);

/// Without waiting: copies received output into buf. The count (0: nothing
/// yet), or -1 once the session has ended (or failed, with a message).
int ssh_poll(ssh_t *s, void *buf, size_t max);

/// Sends an SSH_MSG_IGNORE so idle connections stay up. False if it's gone.
bool ssh_keepalive(ssh_t *s);

/// The signal that ended the command ("INT", "KILL", ...), or NULL.
const char *ssh_exit_signal(ssh_t *s);

/// Reads up to `max` bytes the command wrote to stdout (its stderr is
/// printed). Waits up to the timeout for some; returns the count, 0 at end of
/// output, or -1 on an error, Ctrl-C or timeout (with a message).
int ssh_read(ssh_t *s, void *buf, size_t max);

/// Sends `len` bytes to the command's stdin. False on failure.
bool ssh_write(ssh_t *s, const void *data, size_t len);

/// Tells the command there is no more input.
void ssh_send_eof(ssh_t *s);

/// The command's exit status once the server has sent it, else -1.
int ssh_exit_status(ssh_t *s);

/// Closes the connection and frees everything.
void ssh_close(ssh_t *s);
