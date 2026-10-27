/**
 * @file term.c
 * @brief term: a serial terminal on COM1 (UART1, the port BASIC calls COM1; never the debug UART).
 *
 * What arrives goes to the screen through the VT100/ANSI console, so the far
 * end's colours and cursor moves work; keys go out. The bottom row is a
 * status line and rows 1-29 scroll.
 *
 * Ctrl-A starts a command (S settings, X exit, B break, C clear, L log,
 * F send a file, U upload and D download with XMODEM/YMODEM/ZMODEM, E echo,
 * H help, Ctrl-A sends Ctrl-A); F1 opens the settings and F10 exits too.
 * A ZMODEM sender at the far end (sz) starts a download by itself. Settings
 * are kept in /TERM.CFG.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_port.h"  // port_serial_*: BASIC's COM1 driver
#include "console.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "serial_console.h"
#include "xfer.h"

/// The settings file.
#define CFG_FILE   "/TERM.CFG"
/// The log file's name.
#define LOG_FILE   "TERM.LOG"
#define TEXT_ROWS  (CONSOLE_ROWS - 1)  ///< the last row is the status line
#define STATUS_ATTR 0x1f                ///< bright white on blue, as the title bar
/// The control-key code of a letter.
#define CTRL(c)    ((c) & 0x1f)

/// The terminal's settings.
typedef struct {
    unsigned baud;  ///< the speed
    /// Data bits and stop bits.
    ///@{
    int data, stop;
    ///@}
    char parity;  ///< N, E or O
    int tx;       ///< TX pin 4, 8, 20 or 24; RX is the next pin
    bool flow;    ///< RTS/CTS on TX+2 / TX+3
    bool echo;    ///< show what is typed
    int enter;    ///< Enter sends 0 CR, 1 LF, 2 CR LF
    bool add_lf;  ///< a received CR gets an LF after it
    bool bs_del;  ///< Backspace sends DEL (127), else BS (8)
} term_cfg_t;

/// The settings at first: 115200 8N1 on GP4/GP5.
static const term_cfg_t defaults = {115200, 8, 1, 'N', 4, false, false, 0, false, true};
/// The settings.
static term_cfg_t cfg;

/// The speeds offered.
static const unsigned bauds[] = {300,   1200,  2400,   4800,   9600,   19200,
                                 38400, 57600, 115200, 230400, 460800, 921600};
/// Their count.
#define NBAUDS (int)(sizeof bauds / sizeof *bauds)
/// The TX pins offered.
static const int tx_pins[] = {4, 8, 20, 24};
/// What Enter sends, as names.
static const char *const enter_names[] = {"CR", "LF", "CR LF"};

static FIL *log_file;  ///< while logging (SRAM is short)
/// Logging what arrives.
static bool logging;
/// To this file.
static char log_name[64];
/// When the log was last flushed.
static absolute_time_t log_synced;
static char message[192];  ///< shown in the status line (cut to fit) until a key
/// The status line must be drawn again.
static bool status_dirty = true;
static bool port_ok;  ///< COM1 is open with cfg

// ---- Settings file --------------------------------------------------------------

/// Loads /TERM.CFG.
static void cfg_load(void) {
    cfg = defaults;
    FIL f;
    if (f_open(&f, CFG_FILE, FA_READ) != FR_OK)
        return;
    char line[64];
    while (f_gets(line, sizeof line, &f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        int n = atoi(v);
        if (!strcmp(line, "baud") && n >= 50)
            cfg.baud = n;
        else if (!strcmp(line, "data") && n >= 5 && n <= 8)
            cfg.data = n;
        else if (!strcmp(line, "parity") && strchr("NEO", v[0]))
            cfg.parity = v[0];
        else if (!strcmp(line, "stop") && (n == 1 || n == 2))
            cfg.stop = n;
        else if (!strcmp(line, "tx") && (n == 4 || n == 8 || n == 20 || n == 24))
            cfg.tx = n;
        else if (!strcmp(line, "flow"))
            cfg.flow = n;
        else if (!strcmp(line, "echo"))
            cfg.echo = n;
        else if (!strcmp(line, "enter") && n >= 0 && n <= 2)
            cfg.enter = n;
        else if (!strcmp(line, "add_lf"))
            cfg.add_lf = n;
        else if (!strcmp(line, "bs_del"))
            cfg.bs_del = n;
    }
    f_close(&f);
}

/// Saves /TERM.CFG.
/// @return saved
static bool cfg_save(void) {
    FIL f;
    if (f_open(&f, CFG_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    int n = f_printf(&f,
                     "baud=%u\ndata=%d\nparity=%c\nstop=%d\ntx=%d\nflow=%d\necho=%d\n"
                     "enter=%d\nadd_lf=%d\nbs_del=%d\n",
                     cfg.baud, cfg.data, cfg.parity, cfg.stop, cfg.tx, cfg.flow, cfg.echo,
                     cfg.enter, cfg.add_lf, cfg.bs_del);
    return f_close(&f) == FR_OK && n > 0;
}

// ---- The port ---------------------------------------------------------------------

/// Opens COM1 with cfg; prints why not and returns false on failure.
/// @return opened
static bool port_open(void) {
    port_serial_close();
    port_ok = false;
    const char *why = port_serial_open(cfg.baud, cfg.data, cfg.parity, cfg.stop, cfg.tx, cfg.tx + 1);
    if (!why && cfg.flow)
        why = port_serial_flow(true);
    if (why) {
        port_serial_close();
        snprintf(message, sizeof message, "Can't open COM1: %s", why);
        status_dirty = true;
        return false;
    }
    port_ok = true;
    return true;
}

/// Sends to COM1.
/// @param s the bytes
/// @param n their count
static void send(const char *s, int n) {
    if (port_ok)  // closed after a failed settings change: nothing to send to
        port_serial_write(s, n);
}

// ---- Screen -------------------------------------------------------------------------

/// Received or echoed text: to the screen, and to the debug UART when it has
/// room (it is slower than COM1 can be).
/// @param s the text
/// @param n its length
static void show(const char *s, int n) {
    fflush(stdout);
    console_write(s, n);
    serial_console_write_some(s, n);
}

/// Shows a string.
/// @param s it
static void show_str(const char *s) {
    show(s, strlen(s));
}

/// The status line, written straight into the bottom row so the cursor,
/// colours and the far end's output are left alone.
static void draw_status(void) {
    char text[200];  // cut to the screen width below
    if (message[0]) {
        snprintf(text, sizeof text, " %s", message);
    } else {
        char extra[96] = "";
        if (cfg.flow)
            strcat(extra, " RTS/CTS");
        if (cfg.echo)
            strcat(extra, " echo");
        if (logging)
            snprintf(extra + strlen(extra), sizeof extra - strlen(extra), " LOG %s", log_name);
        snprintf(text, sizeof text, " COM1 %u %d%c%d GP%d/GP%d%s", cfg.baud, cfg.data, cfg.parity,
                 cfg.stop, cfg.tx, cfg.tx + 1, extra);
        const char *hint = "F1 settings  Ctrl-A H help  F10 exit ";
        int pad = CONSOLE_COLS - (int)strlen(text) - (int)strlen(hint);
        if (pad > 0)
            snprintf(text + strlen(text), sizeof text - strlen(text), "%*s%s", pad, "", hint);
    }
    int len = strlen(text);
    if (len > CONSOLE_COLS)
        len = CONSOLE_COLS;
    for (int col = 0; col < CONSOLE_COLS; ++col)
        console_set_cell(CONSOLE_ROWS - 1, col, col < len ? (uint8_t)text[col] : ' ', STATUS_ATTR);
    status_dirty = false;
}

/// Sets the status line's message.
/// @param text it
static void set_message(const char *text) {
    snprintf(message, sizeof message, "%s", text);
    draw_status();
}

/// Rows 1-29 scroll; the far end may have reset that (ESC [ r), so it is put
/// back when things are quiet. ESC 7 / ESC 8 keep the cursor where it was.
static void keep_layout(void) {
    int top, bottom;
    console_get_scroll_region(&top, &bottom);
    if (top != 0 || bottom != TEXT_ROWS - 1) {
        char seq[24];
        snprintf(seq, sizeof seq, "\x1b" "7\x1b[1;%dr\x1b" "8", TEXT_ROWS);
        console_write(seq, strlen(seq));
        status_dirty = true;
    }
    if (status_dirty)
        draw_status();
}

/// A saved copy of the screen, for pages drawn over the terminal.
typedef struct {
    /// Its characters and attributes.
    ///@{
    uint8_t ch[CONSOLE_ROWS][CONSOLE_COLS], attr[CONSOLE_ROWS][CONSOLE_COLS];
    ///@}
} saved_screen_t;

/// Saves the screen, cursor and colours (NULL if out of memory: the screen
/// is then just cleared afterwards).
/// @return the copy (free with page_end), or NULL
static saved_screen_t *screen_save(void) {
    saved_screen_t *s = malloc(sizeof *s);
    if (s)
        for (int r = 0; r < CONSOLE_ROWS; ++r)
            for (int c = 0; c < CONSOLE_COLS; ++c)
                console_get_cell(r, c, &s->ch[r][c], &s->attr[r][c]);
    fflush(stdout);
    console_write("\x1b" "7", 2);  // cursor and colours
    printf("\x1b[r\x1b[0m\x1b[?25l");
    return s;
}

/// Saves the screen and clears it, for a page.
/// @return the copy, or NULL
static saved_screen_t *page_begin(void) {
    saved_screen_t *s = screen_save();
    printf("\x1b[2J\x1b[H");
    return s;
}

/// Puts the terminal's screen back.
/// @param s the copy, or NULL
static void page_end(saved_screen_t *s) {
    printf("\x1b[0m\x1b[2J\x1b[1;%dr\x1b[?25h", TEXT_ROWS);
    fflush(stdout);
    if (s) {
        for (int r = 0; r < CONSOLE_ROWS; ++r)
            for (int c = 0; c < CONSOLE_COLS; ++c)
                console_set_cell(r, c, s->ch[r][c], s->attr[r][c]);
        free(s);
    }
    console_write("\x1b" "8", 2);
    status_dirty = true;
}

// ---- Keys ---------------------------------------------------------------------------

/// Keys decoded from escape sequences.
enum { KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_ESC, KEY_F1, KEY_F10, KEY_OTHER };

/// Reads an escape sequence after ESC into seq (with the ESC); returns its key.
/// @param seq the sequence
/// @param len set to its length
/// @return the key
static int read_escape(char *seq, int *len) {
    int n = 0;
    seq[n++] = 0x1b;
    int c = getchar_timeout_us(30000);
    if (c < 0) {
        *len = n;
        return KEY_ESC;
    }
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
    seq[n] = 0;
    *len = n;
    if (!strcmp(seq, "\x1bOP") || !strcmp(seq, "\x1b[11~"))
        return KEY_F1;
    if (!strcmp(seq, "\x1b[21~"))
        return KEY_F10;
    if (!strcmp(seq, "\x1b[A") || !strcmp(seq, "\x1bOA"))
        return KEY_UP;
    if (!strcmp(seq, "\x1b[B") || !strcmp(seq, "\x1bOB"))
        return KEY_DOWN;
    if (!strcmp(seq, "\x1b[C") || !strcmp(seq, "\x1bOC"))
        return KEY_RIGHT;
    if (!strcmp(seq, "\x1b[D") || !strcmp(seq, "\x1bOD"))
        return KEY_LEFT;
    return KEY_OTHER;
}

/// Waits for a key.
/// @return it
static int wait_key(void) {
    int c;
    while ((c = getchar_timeout_us(100000)) < 0)
        ;
    if (c != 0x1b)
        return c;
    char seq[16];
    int n;
    return read_escape(seq, &n);
}

/// Reads a line of text in the status line. False if cancelled (Esc, Ctrl-C).
/// @param label the prompt
/// @param text the text, edited in place
/// @param size room in text
/// @return Enter (else cancelled)
static bool prompt(const char *label, char *text, int size) {
    int n = strlen(text);
    for (;;) {
        char line[CONSOLE_COLS + 1];
        snprintf(line, sizeof line, "%s%s_", label, text);
        set_message(line);
        int k = wait_key();
        if (k == '\r' || k == '\n') {
            message[0] = 0;
            return true;
        }
        if (k == KEY_ESC || k == CTRL('c')) {
            message[0] = 0;
            return false;
        }
        if ((k == '\b' || k == 0x7f) && n > 0)
            text[--n] = 0;
        else if (k >= 0x20 && k < 0x7f && n < size - 1 &&
                 (int)strlen(label) + n < CONSOLE_COLS - 3) {
            text[n++] = k;
            text[n] = 0;
        }
    }
}

// ---- Settings page ------------------------------------------------------------------

/// The settings page's rows.
enum { S_BAUD, S_DATA, S_PARITY, S_STOP, S_PINS, S_FLOW, S_ECHO, S_ENTER, S_CR, S_BS, S_COUNT };

/// A setting's value as text.
/// @param i the setting
/// @param out the text
/// @param size room in out
static void setting_text(int i, char *out, int size) {
    switch (i) {
    case S_BAUD: snprintf(out, size, "%u", cfg.baud); break;
    case S_DATA: snprintf(out, size, "%d", cfg.data); break;
    case S_PARITY:
        snprintf(out, size, "%s", cfg.parity == 'E' ? "Even" : cfg.parity == 'O' ? "Odd" : "None");
        break;
    case S_STOP: snprintf(out, size, "%d", cfg.stop); break;
    case S_PINS: snprintf(out, size, "TX GP%d, RX GP%d", cfg.tx, cfg.tx + 1); break;
    case S_FLOW:
        if (cfg.flow)
            snprintf(out, size, "RTS/CTS (CTS GP%d, RTS GP%d)", cfg.tx + 2, cfg.tx + 3);
        else
            snprintf(out, size, "None");
        break;
    case S_ECHO: snprintf(out, size, "%s", cfg.echo ? "On" : "Off"); break;
    case S_ENTER: snprintf(out, size, "%s", enter_names[cfg.enter]); break;
    case S_CR: snprintf(out, size, "%s", cfg.add_lf ? "CR LF (adds a line feed)" : "as it is"); break;
    case S_BS: snprintf(out, size, "%s", cfg.bs_del ? "DEL (127)" : "BS (8, Ctrl-H)"); break;
    }
}

/// Changes a setting to its next or previous value.
/// @param i the setting
/// @param dir +1 or -1
static void change_setting(int i, int dir) {
    switch (i) {
    case S_BAUD: {
        int k = 0;
        while (k < NBAUDS && bauds[k] < cfg.baud)
            ++k;
        // k: the first listed rate at or above the current one.
        if (dir > 0)
            k = k < NBAUDS && bauds[k] == cfg.baud ? k + 1 : k;
        else
            k = k - 1;
        if (k >= 0 && k < NBAUDS)
            cfg.baud = bauds[k];
        break;
    }
    case S_DATA: cfg.data = (cfg.data - 5 + 4 + dir) % 4 + 5; break;
    case S_PARITY: {
        const char *order = "NEO";
        int k = strchr(order, cfg.parity) - order;
        cfg.parity = order[(k + 3 + dir) % 3];
        break;
    }
    case S_STOP: cfg.stop = cfg.stop == 1 ? 2 : 1; break;
    case S_PINS: {
        int k = 0;
        while (k < 3 && tx_pins[k] != cfg.tx)
            ++k;
        cfg.tx = tx_pins[(k + 4 + dir) % 4];
        break;
    }
    case S_FLOW: cfg.flow = !cfg.flow; break;
    case S_ECHO: cfg.echo = !cfg.echo; break;
    case S_ENTER: cfg.enter = (cfg.enter + 3 + dir) % 3; break;
    case S_CR: cfg.add_lf = !cfg.add_lf; break;
    case S_BS: cfg.bs_del = !cfg.bs_del; break;
    }
}

/// Draws the settings page.
/// @param sel the setting selected
/// @param note a note under them, or NULL
static void draw_settings(int sel, const char *note) {
    static const char *const labels[] = {"Baud rate", "Data bits", "Parity", "Stop bits",
                                         "Pins", "Flow control", "Local echo", "Enter sends",
                                         "Received CR", "Backspace sends"};
    printf("\x1b[H\x1b[97;44m  Serial terminal settings (COM1)%*s\x1b[0m\n\n", CONSOLE_COLS - 34, "");
    for (int i = 0; i < S_COUNT; ++i) {
        char value[48];
        setting_text(i, value, sizeof value);
        printf("\x1b[2K   \x1b[90m%-18s\x1b[0m%s< %s >\x1b[0m\n", labels[i], i == sel ? "\x1b[7m" : "",
               value);
    }
    printf("\n\x1b[2K   \x1b[96mUp/Down\x1b[0m choose   \x1b[96mLeft/Right\x1b[0m change   "
           "\x1b[96mEnter\x1b[0m type a baud rate\n"
           "\x1b[2K   \x1b[96mS\x1b[0m save as the default (" CFG_FILE ")\n"
           "\x1b[2K   \x1b[96mEsc\x1b[0m use these settings   \x1b[96mQ\x1b[0m cancel changes\n\n");
    printf("\x1b[2K   \x1b[93m%s\x1b[0m", note ? note : "");
    fflush(stdout);
}

/// Returns true if the port settings changed (it needs opening again).
/// @return the port settings changed
static bool settings_page(void) {
    term_cfg_t before = cfg;
    saved_screen_t *saved = page_begin();
    int sel = 0;
    const char *note = NULL;
    char note_buf[64];
    for (;;) {
        draw_settings(sel, note);
        note = NULL;
        int k = wait_key();
        if (k == KEY_UP)
            sel = (sel + S_COUNT - 1) % S_COUNT;
        else if (k == KEY_DOWN || k == '\t')
            sel = (sel + 1) % S_COUNT;
        else if (k == KEY_LEFT || k == KEY_RIGHT || k == ' ')
            change_setting(sel, k == KEY_LEFT ? -1 : 1);
        else if ((k == '\r' || k == '\n') && sel == S_BAUD) {
            char text[12] = "";
            printf("\x1b[%d;4H\x1b[2KBaud rate: \x1b[?25h", 5 + S_COUNT + 4);
            fflush(stdout);
            int n = 0;
            for (;;) {
                int c = wait_key();
                if (c == '\r' || c == '\n' || c == KEY_ESC)
                    break;
                if ((c == '\b' || c == 0x7f) && n > 0) {
                    text[--n] = 0;
                    printf("\b \b");
                } else if (c >= '0' && c <= '9' && n < 8) {
                    text[n++] = c;
                    putchar(c);
                }
                fflush(stdout);
            }
            printf("\x1b[?25l");
            unsigned b = strtoul(text, NULL, 10);
            if (b >= 50 && b <= 4000000)
                cfg.baud = b;
            else if (n)
                note = "Baud rates go from 50 to 4000000.";
        } else if (k == 's' || k == 'S') {
            bool ok = cfg_save();
            snprintf(note_buf, sizeof note_buf, ok ? "Saved to " CFG_FILE "." : "Can't write " CFG_FILE " (is the drive in?)");
            note = note_buf;
        } else if (k == 'q' || k == 'Q' || k == CTRL('c')) {
            cfg = before;
            break;
        } else if (k == KEY_ESC || k == KEY_F1) {
            break;
        }
    }
    page_end(saved);
    return cfg.baud != before.baud || cfg.data != before.data || cfg.parity != before.parity ||
           cfg.stop != before.stop || cfg.tx != before.tx || cfg.flow != before.flow;
}

/// Ctrl-A H: the keys.
static void help_page(void) {
    saved_screen_t *saved = page_begin();
    printf("\x1b[97;44m  Serial terminal keys%*s\x1b[0m\n\n", CONSOLE_COLS - 22, "");
    static const char *const keys[][2] = {
        {"F1  or Ctrl-A S", "settings: speed, format, pins, flow control, ..."},
        {"F10 or Ctrl-A X", "leave the terminal"},
        {"Ctrl-A B", "send a BREAK"},
        {"Ctrl-A C", "clear the screen"},
        {"Ctrl-A E", "local echo on / off"},
        {"Ctrl-A L", "log what arrives to a file (again: stop)"},
        {"Ctrl-A F", "send a file"},
        {"Ctrl-A Ctrl-A", "send Ctrl-A"},
        {"Ctrl-A U", "upload: send files with XMODEM, YMODEM or ZMODEM"},
        {"Ctrl-A D", "download: receive files (sz at the far end starts it)"},
        {"Ctrl-A H", "this page"},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
        printf("   \x1b[97m%-18s\x1b[0m%s\n", keys[i][0], keys[i][1]);
    printf("\n   Other keys go to the far end; arrow and function keys as VT100\n"
           "   sequences. The screen is 80 x 29 for the far end (stty rows 29).\n\n"
           "   \x1b[90mAny key to go back.\x1b[0m");
    fflush(stdout);
    wait_key();
    page_end(saved);
}

// ---- Log and send file -----------------------------------------------------------

/// Ctrl-A L: logging on or off.
static void toggle_log(void) {
    if (logging) {
        f_close(log_file);
        logging = false;
        char m[96];
        snprintf(m, sizeof m, "Stopped logging to %s.", log_name);
        set_message(m);
        return;
    }
    char name[64] = LOG_FILE;
    if (!prompt("Log to file (adds to it): ", name, sizeof name) || !name[0]) {
        status_dirty = true;
        return;
    }
    if (!log_file && !(log_file = malloc(sizeof *log_file))) {
        set_message("Not enough memory to log.");
        return;
    }
    if (f_open(log_file, name, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        set_message("Can't open that file (is the drive in?)");
        return;
    }
    snprintf(log_name, sizeof log_name, "%s", name);
    logging = true;
    log_synced = make_timeout_time_ms(5000);
    status_dirty = true;
}

/// Writes received bytes to the log.
/// @param s the bytes
/// @param n their count
static void log_bytes(const char *s, int n) {
    if (!logging)
        return;
    UINT done;
    if (f_write(log_file, s, n, &done) != FR_OK || done != (UINT)n) {
        f_close(log_file);
        logging = false;
        set_message("Logging stopped: can't write the file (drive full or gone?)");
    }
}

static int pump_received(void);
static bool zmodem_seen;  ///< the far end started a ZMODEM send

/// Ctrl-A S: sends a text file as typed.
static void send_file(void) {
    char name[64] = "";
    if (!prompt("Send file: ", name, sizeof name) || !name[0]) {
        status_dirty = true;
        return;
    }
    FIL f;
    if (f_open(&f, name, FA_READ) != FR_OK) {
        set_message("Can't open that file.");
        return;
    }
    char buf[256];
    UINT got;
    unsigned long sent = 0, size = f_size(&f);
    bool stopped = false;
    absolute_time_t shown = nil_time;
    while (f_read(&f, buf, sizeof buf, &got) == FR_OK && got > 0) {
        for (UINT i = 0; i < got; i += 32) {
            int n = got - i < 32 ? got - i : 32;
            send(buf + i, n);
            pump_received();
        }
        sent += got;
        int c = getchar_timeout_us(0);
        if (c == CTRL('c') || c == 0x1b) {
            stopped = true;
            break;
        }
        if (time_reached(shown)) {
            char m[96];
            snprintf(m, sizeof m, "Sending %s: %lu of %lu bytes (Esc stops)", name, sent, size);
            set_message(m);
            shown = make_timeout_time_ms(200);
        }
    }
    f_close(&f);
    char m[96];
    snprintf(m, sizeof m, "%s %s: %lu bytes.", stopped ? "Stopped sending" : "Sent", name, sent);
    set_message(m);
}

// ---- XMODEM / YMODEM / ZMODEM ------------------------------------------------------

/// A file open for a transfer.
typedef struct {
    FIL f;  ///< the file
    char name[80];  ///< its name
    bool writing;  ///< open for writing
} xfile_t;

static struct {
    xfer_proto_t proto;  ///< the protocol
    bool sending;  ///< sending (else receiving)
    saved_screen_t *saved;  ///< the screen under the box
    /// When it started, and when the box is next drawn.
    ///@{
    absolute_time_t started, next_draw;
    ///@}
    uint32_t start_done;  ///< bytes already there when the file started (resume)
    const char *last_name;  ///< the file name last shown
} xf;  ///< the transfer under way

/// The transfer box's top row.
#define BOX_TOP  10
/// Its left column.
#define BOX_LEFT 10
/// Its width.
#define BOX_W    60
/// Its rows.
#define BOX_ROWS 8

/// A line of the box.
/// @param row its row in the box
/// @param text its text
static void box_line(int row, const char *text) {
    printf("\x1b[%d;%dH\x1b[97;44m %-*.*s\x1b[0m", BOX_TOP + row, BOX_LEFT, BOX_W - 1, BOX_W - 1, text);
}

/// Opens the transfer box.
static void box_open(void) {
    char title[64];
    snprintf(title, sizeof title, "%s %s", xfer_proto_name(xf.proto), xf.sending ? "upload" : "download");
    char m[96];
    snprintf(m, sizeof m, "%s: Esc or Ctrl-C cancels", title);
    set_message(m);
    xf.saved = screen_save();
    printf("\x1b[%d;%dH\x1b[30;47m %-*s\x1b[0m", BOX_TOP, BOX_LEFT, BOX_W - 1, title);
    for (int r = 1; r < BOX_ROWS; ++r)
        box_line(r, "");
    box_line(2, "Waiting for the other end...");
    box_line(BOX_ROWS - 1, "Esc or Ctrl-C cancels");
    fflush(stdout);
    xf.started = get_absolute_time();
    xf.next_draw = nil_time;
}

/// Closes it, putting the screen back.
static void box_close(void) {
    printf("\x1b[0m\x1b[1;%dr\x1b[?25h", TEXT_ROWS);
    fflush(stdout);
    if (xf.saved) {
        for (int r = 0; r < CONSOLE_ROWS; ++r)
            for (int c = 0; c < CONSOLE_COLS; ++c)
                console_set_cell(r, c, xf.saved->ch[r][c], xf.saved->attr[r][c]);
        free(xf.saved);
        xf.saved = NULL;
    }
    console_write("\x1b" "8", 2);
    status_dirty = true;
}

/// The transfer's progress, into the box.
/// @param ctx unused
/// @param p the progress
static void x_progress(void *ctx, const xfer_progress_t *p) {
    if (!time_reached(xf.next_draw))
        return;
    xf.next_draw = make_timeout_time_ms(250);
    if (p->name != xf.last_name || p->done < xf.start_done) {
        xf.last_name = p->name;
        xf.start_done = p->done;
        xf.started = get_absolute_time();
    }
    char line[80];
    snprintf(line, sizeof line, "File:   %s", p->name[0] ? p->name : "...");
    box_line(1, line);
    if (p->size)
        snprintf(line, sizeof line, "Bytes:  %lu of %lu (%lu%%)", (unsigned long)p->done,
                 (unsigned long)p->size, (unsigned long)((uint64_t)p->done * 100 / p->size));
    else
        snprintf(line, sizeof line, "Bytes:  %lu", (unsigned long)p->done);
    box_line(2, line);
    int64_t us = absolute_time_diff_us(xf.started, get_absolute_time());
    unsigned rate = us > 0 ? (unsigned)((uint64_t)(p->done - xf.start_done) * 1000000 / us) : 0;
    snprintf(line, sizeof line, "Speed:  %u.%u KB/s   Retries: %d   Files done: %d", rate / 1024,
             rate % 1024 * 10 / 1024, p->errors, p->files);
    box_line(3, line);
    char bar[BOX_W];
    int width = BOX_W - 4, fill = p->size ? (int)((uint64_t)p->done * width / p->size) : 0;
    for (int i = 0; i < width; ++i)
        bar[i] = i < fill ? '#' : '.';
    bar[width] = 0;
    box_line(5, bar);
    fflush(stdout);
}

/// A byte from COM1 for the transfer (Esc or Ctrl-C cancels).
/// @param ctx unused
/// @param timeout_ms how long to wait
/// @return the byte, XFER_TIMEOUT or XFER_ABORT
static int x_getc(void *ctx, unsigned timeout_ms) {
    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    absolute_time_t key_check = nil_time;
    for (;;) {
        int c = port_serial_getc();
        if (c >= 0)
            return c;
        if (time_reached(key_check)) {
            int k = getchar_timeout_us(0);
            if (k == 0x1b || k == CTRL('c'))
                return XFER_ABORT;
            key_check = make_timeout_time_ms(20);
        }
        if (time_reached(until))
            return XFER_TIMEOUT;
        tight_loop_contents();
    }
}

/// Sends bytes for the transfer.
/// @param ctx unused
/// @param data the bytes
/// @param len their count
static void x_write(void *ctx, const uint8_t *data, int len) {
    send((const char *)data, len);
}

/// Opens a file to send.
/// @param ctx unused
/// @param path its path
/// @param size set to its size
/// @param name set to its name for the far end
/// @param name_size room in name
/// @return the file, or NULL
static void *x_open_read(void *ctx, const char *path, uint32_t *size, char *name, int name_size) {
    xfile_t *x = malloc(sizeof *x);
    if (!x)
        return NULL;
    if (f_open(&x->f, path, FA_READ) != FR_OK) {
        free(x);
        return NULL;
    }
    x->writing = false;
    *size = f_size(&x->f);
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == ':')
            b = p + 1;
    snprintf(name, name_size, "%s", b);
    return x;
}

/// NAME.EXT, or NAME-1.EXT, NAME-2.EXT ... if that is taken.
/// @param name the name wanted
/// @param out the name free
/// @param size room in out
static void free_name(const char *name, char *out, int size) {
    FILINFO info;
    snprintf(out, size, "%s", name);
    const char *dot = strrchr(name, '.');
    int stem = dot ? dot - name : (int)strlen(name);
    for (int i = 1; f_stat(out, &info) == FR_OK && i < 1000; ++i)
        snprintf(out, size, "%.*s-%d%s", stem, name, i, dot ? dot : "");
}

/// Opens a file to receive (or resume).
/// @param ctx unused
/// @param name its name
/// @param size its size, if known
/// @param resume carry on a partial file
/// @param offset set to where to carry on
/// @param saved_as set to the name used
/// @param saved_as_size room in saved_as
/// @return the file, or NULL
static void *x_open_write(void *ctx, const char *name, uint32_t size, bool resume, uint32_t *offset,
                          char *saved_as, int saved_as_size) {
    xfile_t *x = malloc(sizeof *x);
    if (!x)
        return NULL;
    FILINFO info;
    *offset = 0;
    x->writing = true;
    if (resume && f_stat(name, &info) == FR_OK && info.fsize < size) {
        // The sender asks to carry on with a partial copy (sz -r).
        snprintf(x->name, sizeof x->name, "%s", name);
        if (f_open(&x->f, x->name, FA_WRITE | FA_OPEN_APPEND) == FR_OK) {
            *offset = f_size(&x->f);
            snprintf(saved_as, saved_as_size, "%s", x->name);
            return x;
        }
    }
    free_name(name, x->name, sizeof x->name);
    if (f_open(&x->f, x->name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        free(x);
        return NULL;
    }
    snprintf(saved_as, saved_as_size, "%s", x->name);
    return x;
}

/// Reads from a file.
/// @param ctx unused
/// @param file the file
/// @param buf where
/// @param len how many
/// @return bytes read, or -1
static int x_read(void *ctx, void *file, uint8_t *buf, int len) {
    UINT got;
    return f_read(&((xfile_t *)file)->f, buf, len, &got) == FR_OK ? (int)got : -1;
}

/// Seeks in a file.
/// @param ctx unused
/// @param file the file
/// @param offset where
/// @return done
static bool x_seek(void *ctx, void *file, uint32_t offset) {
    return f_lseek(&((xfile_t *)file)->f, offset) == FR_OK;
}

/// Writes to a file.
/// @param ctx unused
/// @param file the file
/// @param buf the bytes
/// @param len their count
/// @return written
static bool x_write_file(void *ctx, void *file, const uint8_t *buf, int len) {
    UINT done;
    return f_write(&((xfile_t *)file)->f, buf, len, &done) == FR_OK && done == (UINT)len;
}

/// Closes a file (a broken XMODEM/YMODEM download is deleted).
/// @param ctx unused
/// @param file the file
/// @param ok it completed
static void x_close(void *ctx, void *file, bool ok) {
    xfile_t *x = file;
    f_close(&x->f);
    // A broken XMODEM/YMODEM download is deleted; a ZMODEM one is kept, as
    // "sz -r" can carry on from where it stopped.
    if (x->writing && !ok && xf.proto != XFER_ZMODEM)
        f_unlink(x->name);
    free(x);
}

/// The transfers' callbacks.
static const xfer_io_t xfer_io = {NULL,   x_getc, x_write,      x_open_read, x_open_write,
                                  x_read, x_seek, x_write_file, x_close,     x_progress};

static bool glob_match(const char *pat, const char *s) {
    for (; *pat; ++pat, ++s) {
        if (*pat == '*') {
            for (const char *t = s;; ++t) {
                if (glob_match(pat + 1, t))
                    return true;
                if (!*t)
                    return false;
            }
        }
        if (!*s)
            return false;
        int a = *pat >= 'a' && *pat <= 'z' ? *pat - 32 : *pat;
        int b = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (*pat != '?' && a != b)
            return false;
    }
    return !*s;
}

/// The most files sent at once.
#define MAX_SEND 32

/// Adds `word` to paths, or the files it matches if it has * or ? in its
/// last part. Returns the new count.
/// @param word the name or pattern
/// @param paths the list
/// @param n its count
/// @return the new count
static int add_paths(const char *word, char **paths, int n) {
    const char *slash = strrchr(word, '/');
    const char *pat = slash ? slash + 1 : word;
    if (!strpbrk(pat, "*?")) {
        if (n < MAX_SEND && (paths[n] = strdup(word)))
            ++n;
        return n;
    }
    char dir[80];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - word) : 0, word);
    DIR d;
    FILINFO info;
    if (f_opendir(&d, slash ? (dir[0] ? dir : "/") : "") != FR_OK)
        return n;
    while (n < MAX_SEND && f_readdir(&d, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & (AM_DIR | AM_HID | AM_SYS) || !glob_match(pat, info.fname))
            continue;
        char path[340];
        snprintf(path, sizeof path, "%s%s%s", dir, slash ? "/" : "", info.fname);
        if ((paths[n] = strdup(path)))
            ++n;
    }
    f_closedir(&d);
    return n;
}

/// Asks for the protocol.
/// @param sending for an upload
/// @return the protocol, or -1
static int choose_protocol(bool sending) {
    set_message(sending ? "Upload with  X XMODEM  K XMODEM-1K  Y YMODEM  Z ZMODEM  (Esc: back)"
                        : "Download with  X XMODEM  Y YMODEM  Z ZMODEM  (Esc: back)");
    int k = wait_key();
    message[0] = 0;
    status_dirty = true;
    switch (k | 0x20) {
    case 'x': return XFER_XMODEM;
    case 'k': return sending ? XFER_XMODEM_1K : -1;
    case 'y': return XFER_YMODEM;
    case 'z': return XFER_ZMODEM;
    }
    return -1;
}

/// Ctrl-A U (sending) or D, or a ZMODEM download the far end started (auto).
/// @param sending an upload
static void transfer(bool sending) {
    bool autostart = !sending && zmodem_seen;
    zmodem_seen = false;
    int proto = autostart ? XFER_ZMODEM : choose_protocol(sending);
    if (proto < 0)
        return;
    xf.proto = proto;
    xf.sending = sending;
    xf.last_name = NULL;
    xfer_result_t res;
    if (sending) {
        char line[120] = "";
        if (!prompt(proto == XFER_XMODEM || proto == XFER_XMODEM_1K ? "File to send: "
                                                                    : "Files to send (* and ? work): ",
                    line, sizeof line) || !line[0]) {
            status_dirty = true;
            return;
        }
        char *paths[MAX_SEND];
        int n = 0;
        for (char *w = strtok(line, " "); w; w = strtok(NULL, " "))
            n = add_paths(w, paths, n);
        if (!n) {
            set_message("No files match that.");
            return;
        }
        box_open();
        xfer_send(&xfer_io, proto, (const char *const *)paths, n, &res);
        for (int i = 0; i < n; ++i)
            free(paths[i]);
    } else {
        char name[64] = "";
        if (proto == XFER_XMODEM &&
            (!prompt("Save as: ", name, sizeof name) || !name[0])) {
            status_dirty = true;
            return;
        }
        box_open();
        xfer_receive(&xfer_io, proto, name, &res);
    }
    box_close();
    // Drop what the other end may still send (e.g. CANs or the rest of a
    // header) so it doesn't show.
    for (absolute_time_t quiet = make_timeout_time_ms(200); !time_reached(quiet);)
        if (port_serial_getc() >= 0)
            quiet = make_timeout_time_ms(200);
    char m[160];
    snprintf(m, sizeof m, "%s %s: %s", xfer_proto_name(proto), sending ? "upload" : "download",
             res.message);
    set_message(m);
}

// ---- Main loop ----------------------------------------------------------------------

/// Moves what has arrived to the screen (and the log). Returns the count.
/// @return the count
static int pump_received(void) {
    // A ZMODEM sender starts with ZRQINIT, "**" ZDLE "B00..."; those bytes
    // are held back while they match, and a whole match starts a download.
    static const char zrqinit[] = "**\x18" "B00";
    static int matched;
    char buf[300];
    int n = 0, total = 0, c;
    while (!zmodem_seen && (c = port_serial_getc()) >= 0) {
        if (c == zrqinit[matched]) {
            if (++matched == (int)sizeof zrqinit - 1) {
                matched = 0;
                zmodem_seen = true;
            }
            continue;
        }
        if (matched) {  // not ZRQINIT after all: let the held bytes through
            memcpy(buf + n, zrqinit, matched);
            n += matched;
            matched = 0;
            if (c == zrqinit[0]) {
                matched = 1;
                continue;
            }
        }
        buf[n++] = c;
        if (c == '\r' && cfg.add_lf)
            buf[n++] = '\n';
        if (n >= (int)sizeof buf - 8) {
            show(buf, n);
            log_bytes(buf, n);
            total += n;
            n = 0;
        }
    }
    if (n) {
        show(buf, n);
        log_bytes(buf, n);
        total += n;
    }
    return total;
}

/// Sends a typed key (Enter as set, Backspace as set, local echo).
/// @param c the key
static void send_key(int c) {
    char out[2];
    int n = 0;
    if (c == '\r' || c == '\n') {
        static const char *const seqs[] = {"\r", "\n", "\r\n"};
        send(seqs[cfg.enter], strlen(seqs[cfg.enter]));
        if (cfg.echo)
            show_str("\r\n");
        return;
    }
    if (c == '\b' || c == 0x7f)
        c = cfg.bs_del ? 0x7f : '\b';
    out[n++] = c;
    send(out, n);
    if (cfg.echo)
        show(c == 0x7f ? "\b \b" : out, c == 0x7f ? 3 : n);
}

/// Ctrl-A and the key after it. False to leave the terminal.
/// @return stay
static bool command_key(void) {
    set_message("Ctrl-A: S setup X exit B break C clear E echo L log F file U upload D download H help");
    int k = wait_key();
    message[0] = 0;
    status_dirty = true;
    if (k >= 'a' && k <= 'z')
        k -= 32;
    switch (k) {
    case 'S':
        if (settings_page() && port_open())
            message[0] = 0;
        break;
    case 'X': return false;
    case 'B':
        port_serial_break(250);
        set_message("Sent a BREAK.");
        break;
    case 'C':
        show_str("\x1b[2J\x1b[H");
        break;
    case 'E':
        cfg.echo = !cfg.echo;
        set_message(cfg.echo ? "Local echo on." : "Local echo off.");
        break;
    case 'L': toggle_log(); break;
    case 'F': send_file(); break;
    case 'U': transfer(true); break;
    case 'D': transfer(false); break;
    case 'H': help_page(); break;
    case CTRL('a'): send("\x01", 1); break;
    }
    return true;
}

/// Prints how to use term.
static void usage(void) {
    printf("usage: term [baud] [format, e.g. 8N1]   (alias serial)\n"
           "A serial terminal on COM1 (UART1, GP4 TX / GP5 RX unless changed in the\n"
           "settings). F1 settings, F10 exit, Ctrl-A H for the other keys.\n");
}

void term_command(int argc, char **argv) {
    cfg_load();
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] >= '0' && a[0] <= '9' && strlen(a) == 3 && a[0] >= '5' && a[0] <= '8' &&
            strchr("NEOneo", a[1]) && (a[2] == '1' || a[2] == '2')) {
            cfg.data = a[0] - '0';
            cfg.parity = a[1] & ~0x20;
            cfg.stop = a[2] - '0';
        } else if (strspn(a, "0123456789") == strlen(a) && atol(a) >= 50 && atol(a) <= 4000000) {
            cfg.baud = atol(a);
        } else {
            usage();
            return;
        }
    }
    message[0] = 0;
    if (!port_open()) {
        printf("%s\n", message);
        return;
    }
    // Clear, set rows 1-29 to scroll, and start at the top.
    printf("\x1b[0m\x1b[2J\x1b[1;%dr\x1b[H", TEXT_ROWS);
    fflush(stdout);
    set_message("Connected. F1 settings, Ctrl-A H help, F10 exit.");
    absolute_time_t message_until = make_timeout_time_ms(3000);
    bool running = true;
    while (running) {
        int got = pump_received();
        int c = getchar_timeout_us(0);
        if (c >= 0) {
            if (message[0]) {
                message[0] = 0;
                status_dirty = true;
            }
            if (c == 0x1b) {
                char seq[16];
                int n;
                int k = read_escape(seq, &n);
                if (k == KEY_F1) {
                    if (settings_page() && port_open())
                        message[0] = 0;
                } else if (k == KEY_F10) {
                    running = false;
                } else {
                    send(seq, n);
                }
            } else if (c == CTRL('a')) {
                running = command_key();
            } else {
                send_key(c);
            }
            continue;
        }
        if (zmodem_seen) {
            transfer(false);
            continue;
        }
        if (got)
            continue;
        // Quiet: tidy up.
        if (message[0] && time_reached(message_until) && !strncmp(message, "Connected", 9)) {
            message[0] = 0;
            status_dirty = true;
        }
        keep_layout();
        if (logging && time_reached(log_synced)) {
            f_sync(log_file);
            log_synced = make_timeout_time_ms(5000);
        }
        sleep_us(200);
    }
    if (logging) {
        f_close(log_file);
        logging = false;
    }
    free(log_file);
    log_file = NULL;
    port_serial_close();
    // Whole screen scrolling again, status line gone, cursor where it was
    // (ESC [ r homes it, so it is saved round that).
    printf("\x1b[0m\x1b" "7\x1b[r\x1b[%d;1H\x1b[2K\x1b" "8\r\nLeft the terminal (COM1 closed).\n",
           CONSOLE_ROWS);
}
