/**
 * @file term.c
 * @brief term: a serial terminal on tty1 (UART1, the port BASIC calls COM1; never the debug UART).
 *
 * What arrives goes to the screen through the VT100/ANSI console, so the far
 * end's colours and cursor moves work; keys go out. As in the editor, the top
 * row is a menu bar (File, Transfer, Settings, Help: a click or F9), the
 * bottom row a status line, and the far end has the 28 rows between (the
 * console keeps it there, console_set_view). The right mouse button opens a
 * menu of the commands too.
 *
 * Ctrl-A starts a command (S settings, X exit, B break, C clear, L log,
 * F send a file, U upload and D download with XMODEM/YMODEM/ZMODEM, E echo,
 * M menus, H help, Ctrl-A sends Ctrl-A); F1 opens the settings and F10 exits too.
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
#include "mouse.h"
#include "pico/stdlib.h"
#include "serial_console.h"
#include "xfer.h"
#include "version.h"

/// The settings file.
#define CFG_FILE   "/TERM.CFG"
/// The log file's name.
#define LOG_FILE   "TERM.LOG"
#define TEXT_TOP   1                    ///< the far end's first row (row 0 is the menu bar)
#define TEXT_ROWS  (CONSOLE_ROWS - 2)   ///< its rows (the last row is the status line)
#define TEXT_BOTTOM (TEXT_TOP + TEXT_ROWS - 1)  ///< its last row
#define BAR_ATTR   0x70  ///< the menu bar and status line: black on light grey, as in the editor
#define MENU_ATTR  0x70  ///< a menu
#define SEL_ATTR   0x1f  ///< its item under the pointer: bright white on blue
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
        snprintf(message, sizeof message, "Can't open tty1: %s", why);
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

/// The menu bar's titles.
static const char *const menu_titles[] = {"File", "Transfer", "Settings", "Help"};
/// Their count.
#define NMENUS 4

/// A menu title's column on the menu bar.
/// @param m the menu
/// @return its first letter's column
static int menu_col(int m) {
    int c = 1;
    for (int i = 0; i < m; ++i)
        c += (int)strlen(menu_titles[i]) + 2;
    return c;
}

/// The menu title at a column of the menu bar, or -1.
/// @param col the column
/// @return the menu, or -1
static int menu_at(int col) {
    for (int i = 0; i < NMENUS; ++i)
        if (col >= menu_col(i) - 1 && col <= menu_col(i) + (int)strlen(menu_titles[i]))
            return i;
    return -1;
}

/// Writes a bar row, changing only the cells that differ (no flicker).
/// @param row the row
/// @param text its text (cut or padded to the width)
/// @param attr its colours
/// @param hl_from the first column in SEL_ATTR (a menu's title while it is open)
/// @param hl_to after the last (the same as hl_from: none)
static void put_bar(int row, const char *text, uint8_t attr, int hl_from, int hl_to) {
    int len = strlen(text);
    for (int col = 0; col < CONSOLE_COLS; ++col) {
        uint8_t ch = col < len ? (uint8_t)text[col] : ' ', a = col >= hl_from && col < hl_to ? SEL_ATTR : attr;
        uint8_t was_ch, was_a;
        console_get_cell(row, col, &was_ch, &was_a);
        if (was_ch != ch || was_a != a)
            console_set_cell(row, col, ch, a);
    }
}

/// The menu bar, with menu `open`'s title highlighted (-1: none).
/// @param open the menu open, or -1
static void draw_menu_bar(int open) {
    char text[CONSOLE_COLS + 1];
    snprintf(text, sizeof text, " File  Transfer  Settings  Help");
    char right[40];
    snprintf(right, sizeof right, "Serial terminal %s ", VER_TERM);
    int n = strlen(text);
    snprintf(text + n, sizeof text - n, "%*s", CONSOLE_COLS - n, right);
    int from = open >= 0 ? menu_col(open) - 1 : 0;
    put_bar(0, text, BAR_ATTR, from, open >= 0 ? from + (int)strlen(menu_titles[open]) + 2 : 0);
}

/// The status line and menu bar, written straight into their rows so the
/// cursor, colours and the far end's output are left alone.
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
        snprintf(text, sizeof text, " tty1 %u %d%c%d GP%d/GP%d%s", cfg.baud, cfg.data, cfg.parity,
                 cfg.stop, cfg.tx, cfg.tx + 1, extra);
        const char *hint = "F9 menus  F1 settings  F10 exit ";
        int pad = CONSOLE_COLS - (int)strlen(text) - (int)strlen(hint);
        if (pad > 0)
            snprintf(text + strlen(text), sizeof text - strlen(text), "%*s%s", pad, "", hint);
    }
    text[CONSOLE_COLS] = 0;
    put_bar(CONSOLE_ROWS - 1, text, BAR_ATTR, 0, 0);
    draw_menu_bar(-1);
    status_dirty = false;
}

/// Sets the status line's message.
/// @param text it
static void set_message(const char *text) {
    snprintf(message, sizeof message, "%s", text);
    draw_status();
}

/// When things are quiet: the bars drawn again (only cells that changed,
/// e.g. after a full-screen program at the far end gave back the screen it
/// had saved).
static void keep_layout(void) {
    static absolute_time_t next;
    if (status_dirty || time_reached(next)) {
        draw_status();
        next = make_timeout_time_ms(200);
    }
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
    console_set_view(0, CONSOLE_ROWS - 1);  // the whole screen for the page
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
    printf("\x1b[0m\x1b[2J\x1b[?25h");
    fflush(stdout);
    console_set_view(TEXT_TOP, TEXT_BOTTOM);
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
enum { KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_ESC, KEY_F1, KEY_F9, KEY_F10, KEY_OTHER };
/// A mouse event instead of a key (menu_key).
#define KEY_MOUSE 0x1ff

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
    if (!strcmp(seq, "\x1b[20~"))
        return KEY_F9;
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

static void put_text(int row, int col, const char *text, uint8_t attr);
static int menu_key(mouse_event_t *e);

/// The settings box's size and place (in the middle of the screen).
enum { SET_W = 60, SET_H = S_COUNT + 9, SET_ROW = (CONSOLE_ROWS - SET_H) / 2, SET_COL = (CONSOLE_COLS - SET_W) / 2 };
/// The settings box's buttons: OK (use these), Save (as the default too), Cancel.
static const char *const set_buttons[] = {"OK", "Save as default", "Cancel"};
/// Each button's column (worked out as they are drawn).
static int set_button_col[3];
/// Where the setting values start in the box.
#define SET_VALUE_COL (SET_COL + 22)

/// Fills part of a box row with spaces.
/// @param row the row
/// @param col the first column
/// @param w how many
/// @param attr the colours
static void fill_cells(int row, int col, int w, uint8_t attr) {
    for (int c = 0; c < w; ++c)
        console_set_cell(row, col + c, ' ', attr);
}

/// Draws the settings box: a frame with its title, a row for each setting
/// (the one chosen highlighted, with < > to change it), the keys, a note
/// and the buttons.
/// @param sel the setting chosen
/// @param note a note under them, or NULL
static void draw_settings(int sel, const char *note) {
    static const char *const labels[] = {"Baud rate", "Data bits", "Parity", "Stop bits",
                                         "Pins", "Flow control", "Local echo", "Enter sends",
                                         "Received CR", "Backspace sends"};
    int r0 = SET_ROW, c0 = SET_COL;
    for (int r = 0; r < SET_H; ++r) {
        bool edge = r == 0 || r == SET_H - 1;
        console_set_cell(r0 + r, c0, r == 0 ? 0xda : r == SET_H - 1 ? 0xc0 : 0xb3, MENU_ATTR);
        for (int c = 1; c < SET_W - 1; ++c)
            console_set_cell(r0 + r, c0 + c, edge ? 0xc4 : ' ', MENU_ATTR);
        console_set_cell(r0 + r, c0 + SET_W - 1, r == 0 ? 0xbf : r == SET_H - 1 ? 0xd9 : 0xb3, MENU_ATTR);
    }
    put_text(r0, c0 + 2, " Port settings (tty1) ", MENU_ATTR);
    for (int i = 0; i < S_COUNT; ++i) {
        char value[64];
        char v[48];
        setting_text(i, v, sizeof v);
        snprintf(value, sizeof value, "< %s >", v);
        put_text(r0 + 2 + i, c0 + 3, labels[i], MENU_ATTR);
        if (i == sel)
            fill_cells(r0 + 2 + i, SET_VALUE_COL - 1, (int)strlen(value) + 2, SEL_ATTR);
        put_text(r0 + 2 + i, SET_VALUE_COL, value, i == sel ? SEL_ATTR : MENU_ATTR);
    }
    put_text(r0 + S_COUNT + 3, c0 + 3, "Up/Down choose, Left/Right or a click change,", 0x78);
    put_text(r0 + S_COUNT + 4, c0 + 3, "Enter on the baud rate types one", 0x78);
    if (note)
        put_text(r0 + S_COUNT + 5, c0 + 3, note, 0x74);  // red on grey
    int total = 0;
    for (int i = 0; i < 3; ++i)
        total += (int)strlen(set_buttons[i]) + 4 + (i ? 2 : 0);
    int x = c0 + (SET_W - total) / 2;
    for (int i = 0; i < 3; ++i) {
        char b[32];
        snprintf(b, sizeof b, "[ %s ]", set_buttons[i]);
        set_button_col[i] = x;
        put_text(r0 + SET_H - 2, x, b, MENU_ATTR);
        console_set_cell(r0 + SET_H - 2, x + 2, (uint8_t)set_buttons[i][0], 0x71);  // its key, in blue
        x += (int)strlen(b) + 2;
    }
}

/// Types a baud rate on the note line of the box.
/// @return the rate, or 0 if none (cancelled, or out of range: *bad set)
/// @param bad set if one was typed but is out of range
static unsigned type_baud(bool *bad) {
    char text[12] = "";
    int n = 0, row = SET_ROW + S_COUNT + 5;
    *bad = false;
    for (;;) {
        char line[40];
        snprintf(line, sizeof line, "Baud rate: %s_", text);
        fill_cells(row, SET_COL + 1, SET_W - 2, MENU_ATTR);
        put_text(row, SET_COL + 3, line, MENU_ATTR);
        int c = wait_key();
        if (c == '\r' || c == '\n')
            break;
        if (c == KEY_ESC)
            return 0;
        if ((c == '\b' || c == 0x7f) && n > 0)
            text[--n] = 0;
        else if (c >= '0' && c <= '9' && n < 8)
            text[n++] = c, text[n] = 0;
    }
    unsigned b = strtoul(text, NULL, 10);
    if (b >= 50 && b <= 4000000)
        return b;
    *bad = n > 0;
    return 0;
}

/// The settings box (F1, Ctrl-A S, or the menus), over the terminal. Keys:
/// Up/Down choose, Left/Right/Space change, Enter types a baud rate (on that
/// row), S saves as the default, Esc uses them, Q cancels. The mouse: a
/// click on a setting chooses it, on its < or > changes it, on a button
/// presses it. Returns true if the port settings changed (it needs opening
/// again).
/// @return the port settings changed
static bool settings_page(void) {
    term_cfg_t before = cfg;
    console_write("\x1b[?25l", 6);  // the cursor's blink kept off the box
    // The cells under the box, put back afterwards.
    uint8_t *under = malloc(2 * SET_W * SET_H);
    if (under)
        for (int r = 0; r < SET_H; ++r)
            for (int c = 0; c < SET_W; ++c)
                console_get_cell(SET_ROW + r, SET_COL + c, &under[2 * (r * SET_W + c)],
                                 &under[2 * (r * SET_W + c) + 1]);
    int sel = 0;
    const char *note = NULL;
    char note_buf[64];
    bool redraw = true;
    for (;;) {
        if (redraw)
            draw_settings(sel, note);
        redraw = true;
        note = NULL;
        mouse_event_t e;
        int k = menu_key(&e);
        if (k == KEY_MOUSE) {
            redraw = false;
            if (e.type != MOUSE_DOWN && e.type != MOUSE_WHEEL)
                continue;  // the pointer moving: nothing to draw
            int row = e.y / 16, col = e.x / 8, i = row - SET_ROW - 2;
            if (e.type == MOUSE_WHEEL) {
                if (i >= 0 && i < S_COUNT) {
                    sel = i;
                    change_setting(i, e.wheel > 0 ? 1 : -1);
                    redraw = true;
                }
                continue;
            }
            if (e.button == MOUSE_RIGHT) {
                k = KEY_ESC;  // as Esc
            } else if (i >= 0 && i < S_COUNT && col >= SET_COL && col < SET_COL + SET_W) {
                sel = i;
                if (col >= SET_VALUE_COL - 1)  // on the < : back; on the value or > : on
                    change_setting(i, col < SET_VALUE_COL + 2 ? -1 : 1);
                redraw = true;
                continue;
            } else if (row == SET_ROW + SET_H - 2) {
                int b = -1;
                for (int j = 0; j < 3; ++j)
                    if (col >= set_button_col[j] && col < set_button_col[j] + (int)strlen(set_buttons[j]) + 4)
                        b = j;
                if (b < 0)
                    continue;
                k = b == 0 ? KEY_ESC : b == 1 ? 'S' : 'Q';
            } else {
                continue;
            }
        }
        if (k == KEY_UP)
            sel = (sel + S_COUNT - 1) % S_COUNT;
        else if (k == KEY_DOWN || k == '\t')
            sel = (sel + 1) % S_COUNT;
        else if (k == KEY_LEFT || k == KEY_RIGHT || k == ' ')
            change_setting(sel, k == KEY_LEFT ? -1 : 1);
        else if ((k == '\r' || k == '\n') && sel == S_BAUD) {
            bool bad;
            unsigned b = type_baud(&bad);
            if (b)
                cfg.baud = b;
            else if (bad)
                note = "Baud rates go from 50 to 4000000.";
        } else if (k == 's' || k == 'S') {
            bool ok = cfg_save();
            snprintf(note_buf, sizeof note_buf, ok ? "Saved to " CFG_FILE "." : "Can't write " CFG_FILE " (is the drive in?)");
            note = note_buf;
            if (ok)
                break;
        } else if (k == 'q' || k == 'Q' || k == CTRL('c')) {
            cfg = before;
            break;
        } else if (k == KEY_ESC || k == KEY_F1 || k == 'o' || k == 'O' || k == '\r') {
            break;
        }
    }
    if (under) {
        for (int r = 0; r < SET_H; ++r)
            for (int c = 0; c < SET_W; ++c)
                console_set_cell(SET_ROW + r, SET_COL + c, under[2 * (r * SET_W + c)],
                                 under[2 * (r * SET_W + c) + 1]);
        free(under);
    } else {
        status_dirty = true;
    }
    console_write("\x1b[?25h", 6);
    if (note && note == note_buf)
        set_message(note_buf);  // saved: say so on the status line
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
        {"F9  or Ctrl-A M", "the menus (or click the menu bar; right button: a menu)"},
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
           "   sequences. The screen is 80 x 28 for the far end (stty rows 28).\n\n"
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
    printf("\x1b[0m\x1b[?25h");
    fflush(stdout);
    console_set_view(TEXT_TOP, TEXT_BOTTOM);
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

/// Whether a name matches a pattern with * and ?.
/// @param pat the pattern
/// @param s the name
/// @return it matches
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

static int menu_bar(int m);

/// Runs a command (Ctrl-A's letter, upper case). False to leave the terminal.
/// @param k the letter
/// @return stay
static bool run_command(int k) {
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
    case 'M': return run_command(menu_bar(0));
    case CTRL('a'): send("\x01", 1); break;
    }
    return true;
}

/// Ctrl-A and the key after it. False to leave the terminal.
/// @return stay
static bool command_key(void) {
    set_message("Ctrl-A: S setup X exit B break C clear E echo L log F file U upload D download M menus H help");
    int k = wait_key();
    message[0] = 0;
    status_dirty = true;
    if (k >= 'a' && k <= 'z')
        k -= 32;
    return run_command(k);
}

// ---- Menus --------------------------------------------------------------------------

/// A menu item.
typedef struct {
    const char *text;  ///< its name ("-": a line between items)
    const char *keys;  ///< the keys that do the same
    int cmd;           ///< the command it runs (run_command's letter)
} menu_item_t;

/// A line between items.
#define SEPARATOR {"-", NULL, 0}
/// The File menu.
static const menu_item_t file_items[] = {
    {"Send text file...", "Ctrl-A F", 'F'}, {"Log to file...", "Ctrl-A L", 'L'},
    SEPARATOR,                              {"Exit", "F10", 'X'},
};
/// The Transfer menu.
static const menu_item_t transfer_items[] = {
    {"Upload files...", "Ctrl-A U", 'U'}, {"Download files...", "Ctrl-A D", 'D'},
};
/// The Settings menu.
static const menu_item_t settings_items[] = {
    {"Port settings...", "F1", 'S'},     {"Local echo", "Ctrl-A E", 'E'}, SEPARATOR,
    {"Send BREAK", "Ctrl-A B", 'B'},     {"Clear screen", "Ctrl-A C", 'C'},
};
/// The Help menu.
static const menu_item_t help_items[] = {
    {"Keys...", "Ctrl-A H", 'H'},
};
/// The right button's menu.
static const menu_item_t popup_items[] = {
    {"Port settings...", "F1", 'S'},       {"Local echo", "Ctrl-A E", 'E'},
    {"Send BREAK", "Ctrl-A B", 'B'},       {"Clear screen", "Ctrl-A C", 'C'},
    SEPARATOR,                             {"Send text file...", "Ctrl-A F", 'F'},
    {"Upload files...", "Ctrl-A U", 'U'},  {"Download files...", "Ctrl-A D", 'D'},
    {"Log to file...", "Ctrl-A L", 'L'},   SEPARATOR,
    {"Exit", "F10", 'X'},
};
/// The items in an array of them, and their count.
#define ITEMS(a) a, (int)(sizeof a / sizeof *a)
/// The menu bar's menus, in menu_titles' order.
static const struct {
    const menu_item_t *items;  ///< its items
    int n;                     ///< their count
} menus[NMENUS] = {{ITEMS(file_items)}, {ITEMS(transfer_items)}, {ITEMS(settings_items)}, {ITEMS(help_items)}};

/// What run_menu did besides choosing: stay (else the menu-bar menu to go to).
#define MENU_STAY (-1)
/// popup_draw's "nothing drawn yet".
#define MENU_UNDRAWN (-2)

/// An item's name as shown: what it does now (Stop logging, a tick on Local echo).
/// @param it the item
/// @return its name
static const char *item_text(const menu_item_t *it) {
    if (it->cmd == 'L' && logging)
        return "Stop logging";
    if (it->cmd == 'E')
        return cfg.echo ? "\xfb Local echo" : "  Local echo";
    return it->text;
}

/// Writes text into a row of cells.
/// @param row the row
/// @param col the first column
/// @param text the text
/// @param attr its colours
static void put_text(int row, int col, const char *text, uint8_t attr) {
    for (; *text && col < CONSOLE_COLS; ++text, ++col)
        console_set_cell(row, col, (uint8_t)*text, attr);
}

/// Draws a menu straight into the cells (CP437 box lines): all of it, or
/// only the items whose highlight changed (was and sel), so that moving the
/// pointer doesn't flicker.
/// @param items its items
/// @param n their count
/// @param row its top row
/// @param col its left column
/// @param w its width
/// @param sel the item highlighted, or -1
/// @param was the one highlighted on the screen now, or MENU_UNDRAWN
static void menu_draw(const menu_item_t *items, int n, int row, int col, int w, int sel, int was) {
    bool all = was == MENU_UNDRAWN;
    if (all) {
        console_set_cell(row, col, 0xda, MENU_ATTR);
        console_set_cell(row + n + 1, col, 0xc0, MENU_ATTR);
        for (int x = 1; x < w - 1; ++x) {
            console_set_cell(row, col + x, 0xc4, MENU_ATTR);
            console_set_cell(row + n + 1, col + x, 0xc4, MENU_ATTR);
        }
        console_set_cell(row, col + w - 1, 0xbf, MENU_ATTR);
        console_set_cell(row + n + 1, col + w - 1, 0xd9, MENU_ATTR);
    }
    for (int i = 0; i < n; ++i) {
        if (!all && i != sel && i != was)
            continue;
        int r = row + 1 + i;
        if (items[i].text[0] == '-') {
            console_set_cell(r, col, 0xc3, MENU_ATTR);
            for (int x = 1; x < w - 1; ++x)
                console_set_cell(r, col + x, 0xc4, MENU_ATTR);
            console_set_cell(r, col + w - 1, 0xb4, MENU_ATTR);
            continue;
        }
        uint8_t a = i == sel ? SEL_ATTR : MENU_ATTR;
        console_set_cell(r, col, 0xb3, MENU_ATTR);
        for (int x = 1; x < w - 1; ++x)
            console_set_cell(r, col + x, ' ', a);
        console_set_cell(r, col + w - 1, 0xb3, MENU_ATTR);
        put_text(r, col + 2, item_text(&items[i]), a);
        put_text(r, col + w - 2 - (int)strlen(items[i].keys), items[i].keys, a);
    }
}


/// Waits for a key or a mouse event.
/// @param e set to the mouse event
/// @return the key, or KEY_MOUSE
static int menu_key(mouse_event_t *e) {
    for (;;) {
        mouse_pointer_update();
        if (mouse_event(e))
            return KEY_MOUSE;
        int c = getchar_timeout_us(10000);
        if (c < 0)
            continue;
        if (c != 0x1b)
            return c;
        char seq[16];
        int n;
        return read_escape(seq, &n);
    }
}

/// Runs a menu with its top-left corner at a cell; returns the command of
/// the item chosen (by the keys, a click, or letting go of the button held
/// over it), or 0. A menu-bar menu (bar >= 0) also goes to another one with
/// Left / Right or the pointer on its title: *next is set to it.
/// What arrives from COM1 waits while a menu is open.
/// @param items its items
/// @param n their count
/// @param row its top row
/// @param col its left column
/// @param bar its number on the menu bar, or -1 for the pop-up
/// @param next set to the menu to go to next, or MENU_STAY
/// @return the item's command, or 0
static int run_menu(const menu_item_t *items, int n, int row, int col, int bar, int *next) {
    int w = 0;
    for (int i = 0; i < n; ++i)
        if (items[i].keys) {
            int len = (int)strlen(items[i].text) + (int)strlen(items[i].keys) + 4;
            if (len > w)
                w = len;
        }
    w += 4;
    int h = n + 2;
    if (row + h > CONSOLE_ROWS - 1)
        row = CONSOLE_ROWS - 1 - h;
    if (row < 1)
        row = 1;
    if (col + w > CONSOLE_COLS)
        col = CONSOLE_COLS - w;
    if (col < 0)
        col = 0;
    // The cells under it, put back when it closes.
    uint8_t *under = malloc(2 * w * h);
    if (under)
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c)
                console_get_cell(row + r, col + c, &under[2 * (r * w + c)], &under[2 * (r * w + c) + 1]);
    int sel = -1, cmd = 0, drawn = MENU_UNDRAWN;
    bool moved = false;  // the pointer has moved since the menu opened
    *next = MENU_STAY;
    console_write("\x1b[?25l", 6);  // the cursor's blink kept off the menu
    for (;;) {
        if (sel != drawn)
            menu_draw(items, n, row, col, w, sel, drawn);
        drawn = sel;
        mouse_event_t e;
        int k = menu_key(&e);
        if (k == KEY_MOUSE) {
            int i = e.y / 16 - row - 1, c = e.x / 8;
            bool inside = c > col && c < col + w - 1 && i >= 0 && i < n;
            bool ok = inside && items[i].cmd;
            int title = bar >= 0 && e.y / 16 == 0 ? menu_at(c) : -1;
            if (e.type == MOUSE_MOVE) {
                moved = true;
                sel = ok ? i : -1;
                if (title >= 0 && title != bar) {
                    *next = title;
                    break;
                }
            } else if (ok && (e.type == MOUSE_DOWN || (e.type == MOUSE_UP && moved))) {
                cmd = items[i].cmd;
                break;
            } else if (e.type == MOUSE_DOWN && title >= 0 && title != bar) {
                *next = title;
                break;
            } else if (e.type == MOUSE_DOWN && !inside) {
                break;  // a click outside (or on its own title) closes it
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c') || k == KEY_F9 || k == KEY_F10)
            break;
        if (k == '\r' && sel >= 0) {
            cmd = items[sel].cmd;
            break;
        }
        if (bar >= 0 && (k == KEY_LEFT || k == KEY_RIGHT)) {
            *next = (bar + (k == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS;
            break;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int d = k == KEY_UP ? n - 1 : 1;
            if (sel < 0)
                sel = k == KEY_UP ? 0 : n - 1;
            do
                sel = (sel + d) % n;
            while (!items[sel].cmd);
        }
    }
    if (under) {
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c)
                console_set_cell(row + r, col + c, under[2 * (r * w + c)], under[2 * (r * w + c) + 1]);
        free(under);
    }
    console_write("\x1b[?25h", 6);
    return cmd;
}

/// Opens the menu bar at menu m, going from menu to menu until an item is
/// chosen or the menus close; returns the item's command, or 0.
/// @param m the menu
/// @return the command, or 0
static int menu_bar(int m) {
    for (;;) {
        draw_menu_bar(m);
        int next, cmd = run_menu(menus[m].items, menus[m].n, 1, menu_col(m) - 1, m, &next);
        draw_menu_bar(-1);
        if (next == MENU_STAY)
            return cmd;
        m = next;
    }
}

/// The mouse in the terminal: a click on the menu bar opens a menu, the
/// right button the pop-up. False to leave the terminal.
/// @param e the event
/// @return stay
static bool mouse_click(const mouse_event_t *e) {
    if (e->type != MOUSE_DOWN)
        return true;
    int row = e->y / 16, col = e->x / 8;
    int cmd = 0;
    if (row == 0 && menu_at(col) >= 0) {
        cmd = menu_bar(menu_at(col));
    } else if (e->button == MOUSE_RIGHT) {
        int next;
        cmd = run_menu(ITEMS(popup_items), row + 1, col, -1, &next);
    }
    if (message[0]) {
        message[0] = 0;
        status_dirty = true;
    }
    return cmd ? run_command(cmd) : true;
}

/// Prints how to use term.
static void usage(void) {
    printf("usage: term [baud] [format, e.g. 8N1]   (alias serial)\n"
           "A serial terminal on tty1 (UART1, BASIC's COM1; GP4 TX / GP5 RX unless\n"
           "changed in the settings). F1 settings, F9 menus, F10 exit, Ctrl-A H keys.\n");
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
    // Clear, keep the far end to rows 1-28 (the bars above and below), and
    // start at the top.
    printf("\x1b[0m\x1b[2J");
    fflush(stdout);
    console_set_view(TEXT_TOP, TEXT_BOTTOM);
    printf("\x1b[H");
    fflush(stdout);
    int pointer = mouse_pointer_mode();
    mouse_pointer(MOUSE_POINTER_TEXT);
    mouse_flush();
    set_message("Connected. F9 menus, F1 settings, Ctrl-A H help, F10 exit.");
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
                } else if (k == KEY_F9) {
                    running = run_command(menu_bar(0));
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
        mouse_event_t e;
        mouse_pointer_update();
        if (mouse_event(&e)) {
            running = mouse_click(&e);
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
    mouse_pointer(pointer);
    // The whole screen again, cleared (the bars gone with it).
    fflush(stdout);
    console_set_view(0, CONSOLE_ROWS - 1);
    printf("\x1b[0m\x1b[r\x1b[2J\x1b[HLeft the terminal (tty1 closed).\n");
}
