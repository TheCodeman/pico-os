/**
 * @file net.h
 * @brief WiFi (the Pico Plus 2 W's CYW43439 radio) and the network commands.
 *
 * lwIP runs in a low-priority interrupt on core 0 (threadsafe_background), so
 * the network keeps working while a command, the editor or BASIC is running.
 * Code on core 0 that calls lwIP must hold the lock: cyw43_arch_lwip_begin()
 * and cyw43_arch_lwip_end().
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Starts the radio. Call once at boot, after the console is up (it prints a
/// line saying whether WiFi was found).
void net_init(void);

/// Call from the main loop: reports connects and drops, and retries a saved
/// network that was lost.
void net_task(void);

/// The USB drive has been mounted: connects to the network saved in WIFI.CFG.
void net_drive_ready(void);

/// Shell commands.
void net_cmd_wifi(int argc, char **argv);
/// The wget command: downloads a file over HTTP or HTTPS.
void net_cmd_wget(int argc, char **argv);
/// The telnet command (and with no host, the BBS directory).
void net_cmd_telnet(int argc, char **argv);
/// telnet_menu.c: a full-screen list of BBSes (a dialing directory) to call.
void telnet_menu(const char *path);
/// The scp command: copies files over SSH.
void net_cmd_scp(int argc, char **argv);
/// The ssh command: a terminal on an SSH server.
void net_cmd_ssh(int argc, char **argv);

/// Leaves the WiFi network (before a restart), without forgetting it.
void net_shutdown(void);

/// Prints a few words on the WiFi state ("connecting to X"), no newline.
void net_print_status(void);

/// True once WiFi is up with an IP address; otherwise prints why not.
bool net_need_link(void);

// ---- A TCP client connection, for wget and telnet ---------------------------

/// A TCP connection (opaque).
typedef struct net_conn net_conn_t;

/// Looks up `host` (a name or dotted address) and connects. Prints progress
/// and errors; Ctrl-C or Esc gives up. Returns NULL on failure.
net_conn_t *net_conn_open(const char *host, uint16_t port);

/// Copies up to `max` received bytes into `buf`. Returns the count (0 if
/// nothing has arrived yet), or -1 once the other end has closed and
/// everything has been read.
int net_conn_read(net_conn_t *c, void *buf, size_t max);

/// Queues `len` bytes to send, waiting for buffer space. False if the
/// connection has gone.
bool net_conn_write(net_conn_t *c, const void *data, size_t len);

/// Closes the connection and frees it.
void net_conn_close(net_conn_t *c);

/// Looks up `host` (a name or dotted address). Prints errors; Ctrl-C or Esc
/// gives up. False on failure. (lwIP is IPv4-only here, so its ip_addr_t is
/// struct ip4_addr; naming it keeps lwIP's headers out of this one.)
struct ip4_addr;
/// Looks up `host` into `addr`; false (with a message) on failure.
bool net_resolve(const char *host, struct ip4_addr *addr);

/// True if a key is waiting that means "stop" (Ctrl-C or Esc); other keys are
/// thrown away.
bool net_stop_key(void);

// ---- TLS on top of a connection (tls.c), for HTTPS ----------------------------

/// A TLS session on a connection (opaque).
typedef struct net_tls net_tls_t;

/// Indexes CACERT.PEM if needed (a few seconds the first time). Call before
/// connecting, so the server isn't kept waiting. False (with a message) if
/// there's no usable file.
bool net_tls_prepare(void);

/// Does the TLS handshake for `host` (sent as SNI and checked against the
/// certificate). With `verify`, the server's certificate must chain to a root
/// in CACERT.PEM on the drive. Prints errors; returns NULL on failure.
net_tls_t *net_tls_start(net_conn_t *c, const char *host, bool verify);

/// As net_conn_read and net_conn_write, for the decrypted data.
int net_tls_read(net_tls_t *t, void *buf, size_t max);
/// Sends data over TLS; false on failure.
bool net_tls_write(net_tls_t *t, const void *data, size_t len);

/// Says goodbye to the server and frees the TLS state (not the connection).
void net_tls_end(net_tls_t *t);

// ---- Clock --------------------------------------------------------------------

/// Starts SNTP (pool.ntp.org) to set the clock; called once WiFi is up.
void net_clock_start(void);

/// Sets the clock (called by SNTP).
void net_set_time(uint32_t sec, uint32_t us);

/// The date command.
void net_cmd_date(int argc, char **argv);

/// The ntp command: asks an NTP server for the time now and sets the clock.
void net_cmd_ntp(int argc, char **argv);
