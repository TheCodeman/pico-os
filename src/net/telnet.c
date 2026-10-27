/**
 * @file telnet.c
 * @brief telnet: a VT100 terminal session.
 *
 * What the server sends goes straight to the console (which understands
 * VT100/ANSI); keys go to the server.
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

/// Interpret As Command.
#define IAC  255
/// DON'T.
#define DONT 254
/// DO.
#define DO   253
/// WON'T.
#define WONT 252
/// WILL.
#define WILL 251
/// Subnegotiation begins.
#define SB   250
/// Subnegotiation ends.
#define SE   240

/// Binary transmission.
#define OPT_BINARY 0
/// Echo.
#define OPT_ECHO   1
/// Suppress go-ahead.
#define OPT_SGA    3
/// Terminal type.
#define OPT_TTYPE  24
/// Window size.
#define OPT_NAWS   31

#define QUIT_KEY 0x1d  ///< Ctrl-]

/// The connection.
static net_conn_t *conn;
/// The server echoes what we type.
static bool server_echoes;
/// Terminal types sent.
static int ttype_sent;
/// Each option's state, ours and the server's: 0 not agreed yet, 1 on, 2 off.
///@{
static uint8_t us[256], them[256];
///@}

/// Sends bytes.
/// @param p them
/// @param n how many
static void send(const void *p, size_t n) {
    net_conn_write(conn, p, n);
}

/// The console's answers to the server's questions (where the cursor is,
/// what terminal this is): BBSes use them to tell ANSI and UTF-8 terminals
/// apart, and take silence for UTF-8.
/// @param text the answer
/// @param len its length
static void reply_to_server(const char *text, int len) {
    if (conn)
        send(text, len);
}

/// Sends IAC verb option.
/// @param verb DO, WILL and the like
/// @param opt the option
static void send3(uint8_t verb, uint8_t opt) {
    uint8_t msg[3] = {IAC, verb, opt};
    send(msg, 3);
}

/// Sends our window size.
static void send_naws(void) {
    uint8_t msg[] = {IAC, SB, OPT_NAWS, 0, CONSOLE_COLS, 0, CONSOLE_ROWS, IAC, SE};
    send(msg, sizeof msg);
}

/// The server offers to do something (WILL) or stop (WONT). Answers are only
/// sent when they change something, so the two sides never loop.
/// @param opt the option
/// @param will WILL
static void server_will(uint8_t opt, bool will) {
    bool ok = will && (opt == OPT_ECHO || opt == OPT_SGA || opt == OPT_BINARY);
    if (opt == OPT_ECHO)
        server_echoes = ok;
    if (them[opt] != (ok ? 1 : 2)) {
        them[opt] = ok ? 1 : 2;
        send3(ok ? DO : DONT, opt);
    }
}

/// The server asks us to do something (DO) or stop (DONT).
/// @param opt the option
/// @param doit DO
static void server_do(uint8_t opt, bool doit) {
    bool ok = doit && (opt == OPT_TTYPE || opt == OPT_NAWS || opt == OPT_SGA || opt == OPT_BINARY);
    if (us[opt] != (ok ? 1 : 2)) {
        us[opt] = ok ? 1 : 2;
        send3(ok ? WILL : WONT, opt);
    }
    if (ok && opt == OPT_NAWS)
        send_naws();
}

/// A subnegotiation: answers terminal type requests.
/// @param sb its bytes after IAC SB
/// @param n their count
static void subnegotiation(const uint8_t *sb, int n) {
    if (n >= 2 && sb[0] == OPT_TTYPE && sb[1] == 1) {  // SEND
        // Offer ANSI first (what BBSes look for), then VT100.
        const char *name = ttype_sent++ ? "VT100" : "ANSI";
        uint8_t msg[16] = {IAC, SB, OPT_TTYPE, 0};
        int len = 4 + (int)strlen(name);
        memcpy(msg + 4, name, strlen(name));
        msg[len++] = IAC;
        msg[len++] = SE;
        send(msg, len);
    }
}

/// A key typed (or arriving on the serial port) to the server; false if the
/// connection has gone.
/// @param c the key
/// @return still connected
static bool send_key(int c) {
    uint8_t out[2] = {(uint8_t)c, 0};
    int len = 1;
    if (c == '\r') {
        out[1] = '\n';  // telnet's end of line is CR LF
        len = 2;
    } else if (c == IAC) {
        out[1] = IAC;
        len = 2;
    }
    if (!server_echoes && c >= ' ' && c < 0x7f)
        putchar(c);
    else if (!server_echoes && c == '\r')
        printf("\r\n");
    return net_conn_write(conn, out, len);
}

/// After Esc: the rest of the sequence into seq (which starts with the Esc);
/// its length. True in *answer if it is a terminal's answer (a cursor
/// position, ESC [ n ; n R, or what terminal it is, ESC [ ? ... c). Those
/// come from a terminal program on the serial port, which sees the session
/// too and answers the server's questions for its own screen (BBSes then
/// take this one for UTF-8): the console has answered already, so they are
/// dropped.
/// @param seq the sequence
/// @param max room in seq
/// @param answer set to it being an answer
/// @return its length
static int escape_sequence(char *seq, int max, bool *answer) {
    int n = 1;
    seq[0] = 0x1b;
    *answer = false;
    int c = getchar_timeout_us(20000);
    if (c == PICO_ERROR_TIMEOUT)
        return n;
    seq[n++] = (char)c;
    if (c != '[')
        return n;
    bool query = false, digits_only = true;
    while (n < max) {
        c = getchar_timeout_us(20000);
        if (c == PICO_ERROR_TIMEOUT)
            return n;
        seq[n++] = (char)c;
        if (c == '?' && n == 3)
            query = true;
        else if (c >= 0x40 && c <= 0x7e) {
            *answer = (c == 'R' && digits_only && n > 3) || (c == 'c' && query);
            return n;
        } else if (!(c >= '0' && c <= '9') && c != ';')
            digits_only = false;
    }
    return n;
}

/// Feeds received bytes through the telnet protocol; text goes to the screen.
/// @param p the bytes
/// @param n their count
static void receive(const uint8_t *p, int n) {
    static int state;  // 0 text, 1 after IAC, 2 after WILL/WONT/DO/DONT, 3 in SB, 4 IAC in SB
    static uint8_t verb, sb[32];
    static int sb_len;
    for (int i = 0; i < n; ++i) {
        uint8_t b = p[i];
        switch (state) {
        case 0:
            if (b == IAC)
                state = 1;
            else
                putchar(b);
            break;
        case 1:
            if (b == IAC) {
                putchar(b);
                state = 0;
            } else if (b >= WILL && b <= DONT) {
                verb = b;
                state = 2;
            } else if (b == SB) {
                sb_len = 0;
                state = 3;
            } else {
                state = 0;  // NOP, GA and the like
            }
            break;
        case 2:
            if (verb == WILL || verb == WONT)
                server_will(b, verb == WILL);
            else
                server_do(b, verb == DO);
            state = 0;
            break;
        case 3:
            if (b == IAC)
                state = 4;
            else if (sb_len < (int)sizeof sb)
                sb[sb_len++] = b;
            break;
        case 4:
            if (b == SE) {
                subnegotiation(sb, sb_len);
                state = 0;
            } else {
                if (sb_len < (int)sizeof sb)
                    sb[sb_len++] = b;  // IAC IAC inside SB
                state = 3;
            }
            break;
        }
    }
}

void net_cmd_telnet(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-m")) {  // the directory (telnet_menu.c)
        telnet_menu(argc > 2 ? argv[2] : NULL);
        return;
    }
    if (!net_need_link())
        return;
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 23;
    conn = net_conn_open(argv[1], port);
    if (!conn)
        return;
    printf("Connected. Ctrl-] quits.\n");
    console_set_reply(reply_to_server);
    server_echoes = false;
    ttype_sent = 0;
    memset(us, 0, sizeof us);
    memset(them, 0, sizeof them);
    if (port == 23) {
        // Say up front what we'd like; servers that don't do telnet
        // negotiation only see these on port 23.
        uint8_t hello[] = {IAC, DO, OPT_SGA, IAC, WILL, OPT_NAWS, IAC, WILL, OPT_TTYPE};
        send(hello, sizeof hello);
        them[OPT_SGA] = us[OPT_NAWS] = us[OPT_TTYPE] = 1;
    }
    uint8_t buf[512];
    bool open = true;
    while (open) {
        bool idle = true;
        int n = net_conn_read(conn, buf, sizeof buf);
        if (n < 0) {
            open = false;
            printf("\x1b[0m\nConnection closed by %s.\n", argv[1]);
            break;
        }
        if (n > 0) {
            receive(buf, n);
            fflush(stdout);
            idle = false;
        }
        int c;
        while ((c = getchar_timeout_us(idle ? 2000 : 0)) != PICO_ERROR_TIMEOUT) {
            if (c == QUIT_KEY) {
                open = false;
                printf("\x1b[0m\nDisconnected.\n");
                break;
            }
            char seq[24];
            int n = 1;
            bool answer = false;
            seq[0] = (char)c;
            if (c == 0x1b)
                n = escape_sequence(seq, sizeof seq, &answer);
            bool sent = true;
            for (int i = 0; i < n && sent && !answer; ++i)
                sent = send_key((uint8_t)seq[i]);
            if (!sent) {
                open = false;
                printf("\x1b[0m\nConnection lost.\n");
                break;
            }
            idle = false;
        }
        fflush(stdout);
    }
    console_set_reply(NULL);
    net_conn_close(conn);
    conn = NULL;
    // Undo anything the server left set: scroll region, hidden cursor, colours.
    printf("\x1b[r\x1b[?25h\x1b[0m");
}
