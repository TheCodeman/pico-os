/**
 * @file ssh_cmd.c
 * @brief ssh: a terminal session on a server, over ssh.c.
 *
 * What the server sends goes through the VT100/ANSI console (the server is
 * told it is an "xterm" of 80x30); UTF-8 is shown in code page 437 where it
 * has the character. Keys go to the server; as in OpenSSH, Enter then ~.
 * disconnects, ~? lists the escapes and ~~ sends a ~.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "net.h"
#include "pico/stdlib.h"
#include "serial_console.h"
#include "ssh.h"

#define KEEPALIVE_MS 60000  ///< idle this long: send something so NAT boxes keep us

/// The session.
static ssh_t *session;

// ---- UTF-8 to code page 437 ------------------------------------------------------

/// What CP437 bytes 0x80-0xFF are in Unicode.
static const uint16_t cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8,
    0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2,
    0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1,
    0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd,
    0x00bc, 0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562,
    0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534,
    0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560,
    0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580, 0x03b1, 0x00df, 0x0393,
    0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6,
    0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0,
    0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

/// The nearest CP437 byte (0x20 and up) for a code point.
/// @param u the code point
/// @return the byte
static uint8_t to_cp437(uint32_t u) {
    if (u < 0x80)
        return u;
    for (int i = 0; i < 128; ++i)
        if (cp437_high[i] == u)
            return 0x80 + i;
    switch (u) {
    case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032: return '\'';
    case 0x201c: case 0x201d: case 0x201e: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
    case 0x2212: return '-';
    case 0x2026: return '.';
    case 0x2022: case 0x25cf: return 0xf9;
    case 0x2190: return '<';
    case 0x2191: return '^';
    case 0x2192: return '>';
    case 0x2193: return 'v';
    case 0x00a9: return 'c';
    case 0x00ae: return 'r';
    case 0x00d7: return 'x';
    case 0x256d: case 0x250f: return 0xda;  // rounded and heavy corners
    case 0x256e: case 0x2513: return 0xbf;
    case 0x256f: case 0x251b: return 0xd9;
    case 0x2570: case 0x2517: return 0xc0;
    case 0x2523: return 0xc3;
    case 0x252b: return 0xb4;
    case 0x2533: return 0xc2;
    case 0x253b: return 0xc1;
    case 0x254b: return 0xc5;
    }
    if (u >= 0x2500 && u <= 0x257f)  // other lines: heavy, dashed
        return (u & 2) ? 0xb3 : 0xc4;
    if (u >= 0x2581 && u <= 0x2587)
        return 0xdc;
    if (u >= 0x2589 && u <= 0x258f)
        return 0xdd;
    if (u >= 0x2580 && u <= 0x259f)
        return 0xdb;
    return '?';
}

/// Converts UTF-8 to CP437 in place (it only gets shorter); keeps state
/// across calls for characters split between packets.
/// @param buf the text
/// @param n its length
/// @return the new length
static int utf8_to_cp437(uint8_t *buf, int n) {
    static uint32_t code;
    static int need;
    int out = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t c = buf[i];
        if (need) {
            if ((c & 0xc0) == 0x80) {
                code = code << 6 | (c & 0x3f);
                if (--need == 0)
                    buf[out++] = to_cp437(code);
                continue;
            }
            buf[out++] = '?';  // broken sequence
            need = 0;
        }
        if (c < 0x80) {
            buf[out++] = c;
        } else if ((c & 0xe0) == 0xc0) {
            code = c & 0x1f;
            need = 1;
        } else if ((c & 0xf0) == 0xe0) {
            code = c & 0x0f;
            need = 2;
        } else if ((c & 0xf8) == 0xf0) {
            code = c & 0x07;
            need = 3;
        } else {
            buf[out++] = '?';
        }
    }
    return out;
}

// ---- Screen and keys -------------------------------------------------------------

/// Shows text on the screen and the serial console.
/// @param s the text
/// @param n its length
static void show(const char *s, int n) {
    fflush(stdout);
    console_write(s, n);
    serial_console_write_some(s, n);
}

/// Answers to the console's status queries go to the server.
/// @param text the answer
/// @param len its length
static void reply_to_server(const char *text, int len) {
    if (session)
        ssh_write(session, text, len);
}

/// Sends to the server.
/// @param s the bytes
/// @param n their count
/// @return sent
static bool send(const char *s, int n) {
    return ssh_write(session, s, n);
}

/// Reads the rest of an escape sequence after ESC (a key like an arrow);
/// returns its length in seq.
/// @param seq the sequence
/// @return its length
static int read_escape(char *seq) {
    int n = 0;
    seq[n++] = 0x1b;
    int c = getchar_timeout_us(30000);
    if (c < 0)
        return n;
    seq[n++] = c;
    if (c == '[' || c == 'O') {
        while (n < 15) {
            c = getchar_timeout_us(30000);
            if (c < 0)
                break;
            seq[n++] = c;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')
                break;
        }
    }
    // Cursor keys in application mode (vim, less ...): ESC O A, not ESC [ A.
    if (n == 3 && seq[1] == '[' && strchr("ABCDHF", seq[2]) && console_app_cursor())
        seq[1] = 'O';
    return n;
}

/// ~?: the escape sequences.
static void escape_help(void) {
    static const char help[] =
        "\r\nSupported escape sequences (after Enter):\r\n"
        "  ~.  disconnect\r\n"
        "  ~?  this list\r\n"
        "  ~~  send a ~\r\n";
    show(help, sizeof help - 1);
}

// ---- The command -------------------------------------------------------------------

/// Prints how to use ssh.
static void usage(void) {
    printf("usage: ssh [-p port] [user@]host [command]\n"
           "A terminal on an SSH server (Enter ~. disconnects). The password is\n"
           "asked for, or a key in /SSH/ID_ECDSA or /SSH/ID_RSA is used; server\n"
           "keys are remembered in /SSH/KNOWN_HOSTS.\n");
}

void net_cmd_ssh(int argc, char **argv) {
    uint16_t port = 22;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else {
            usage();
            return;
        }
    }
    if (i >= argc || !port) {
        usage();
        return;
    }
    char user[64] = "", host[128];
    const char *at = strrchr(argv[i], '@');
    if (at) {
        snprintf(user, sizeof user, "%.*s", (int)(at - argv[i]), argv[i]);
        snprintf(host, sizeof host, "%s", at + 1);
    } else {
        snprintf(host, sizeof host, "%s", argv[i]);
    }
    char command[256] = "";
    for (++i; i < argc; ++i) {
        size_t len = strlen(command);
        snprintf(command + len, sizeof command - len, "%s%s", len ? " " : "", argv[i]);
    }
    if (!net_need_link())
        return;
    if (!user[0]) {
        printf("Login as: ");
        fflush(stdout);
        int n = 0;
        for (;;) {
            int c = getchar_timeout_us(100000);
            if (c < 0)
                continue;
            if (c == '\r' || c == '\n')
                break;
            if (c == 3 || c == 0x1b) {
                printf("\n");
                return;
            }
            if ((c == '\b' || c == 0x7f) && n > 0) {
                user[--n] = 0;
                printf("\b \b");
            } else if (c >= 0x20 && c < 0x7f && n < (int)sizeof user - 1) {
                user[n++] = c;
                user[n] = 0;
                putchar(c);
            }
            fflush(stdout);
        }
        printf("\n");
        if (!user[0])
            return;
    }

    session = ssh_open(host, port, user);
    if (!session)
        return;
    if (!ssh_shell(session, "xterm", CONSOLE_COLS, CONSOLE_ROWS, command[0] ? command : NULL)) {
        ssh_close(session);
        session = NULL;
        return;
    }
    printf("(Enter ~. disconnects, ~? lists the escapes)\n");
    fflush(stdout);
    console_set_reply(reply_to_server);

    bool line_start = true;  // after Enter: ~ may start an escape
    bool quit = false;
    absolute_time_t idle_until = make_timeout_time_ms(KEEPALIVE_MS);
    uint8_t buf[1024];
    for (;;) {
        int n = ssh_poll(session, buf, sizeof buf);
        if (n < 0)
            break;
        if (n > 0) {
            n = utf8_to_cp437(buf, n);
            show((const char *)buf, n);
            idle_until = make_timeout_time_ms(KEEPALIVE_MS);
        }
        int c = getchar_timeout_us(0);
        if (c >= 0) {
            idle_until = make_timeout_time_ms(KEEPALIVE_MS);
            bool ok = true;
            if (c == '~' && line_start) {
                int k;
                while ((k = getchar_timeout_us(100000)) < 0)
                    ;
                if (k == '.') {
                    quit = true;
                    break;
                }
                if (k == '?')
                    escape_help();
                else if (k == '~')
                    ok = send("~", 1);
                else
                    ok = send("~", 1) && send((char[]){(char)k}, 1);
                line_start = false;
            } else if (c == 0x1b) {
                char seq[16];
                int len = read_escape(seq);
                ok = send(seq, len);
                line_start = false;
            } else {
                if (c == '\b')
                    c = 0x7f;  // what Linux expects from Backspace (VERASE)
                ok = send((char[]){(char)c}, 1);
                line_start = c == '\r' || c == '\n';
            }
            if (!ok)
                break;
            continue;
        }
        if (n == 0) {
            if (time_reached(idle_until)) {
                if (!ssh_keepalive(session))
                    break;
                idle_until = make_timeout_time_ms(KEEPALIVE_MS);
            }
            sleep_us(500);
        }
    }
    console_set_reply(NULL);
    console_reset_modes();
    int status = ssh_exit_status(session);
    const char *sig = ssh_exit_signal(session);
    ssh_close(session);
    session = NULL;
    printf("\nConnection to %s closed", host);
    if (quit)
        printf(" (~.)");
    else if (sig)
        printf(" (killed by signal %s)", sig);
    else if (status > 0)
        printf(" (exit status %d)", status);
    printf(".\n");
}
