/**
 * @file irc.c
 * @brief The `irc` command: an IRC chat client in the hex editor's style (a menu bar like the tracker's,
 *        bevelled panels, the mouse arrow).
 *
 * The screen: the menu bar; a tab for each window (the server's, each channel's and each private chat's),
 * lit when something new is said there; the channel's topic; what is said, wrapped to the width, with
 * times, coloured nicks and mIRC colours; the nick list of a channel on the right; the line being typed;
 * and the status line. Lines typed are said in the window shown, or are commands (/join, /msg, /me and
 * the like; ones it doesn't know go to the server as they are).
 *
 * Memory: nothing is taken until the command starts, and everything is given back when it quits. The
 * state, each window's last NLINES lines and the nick lists are in PSRAM (as the web browser's); the
 * connection's receive ring is net_conn's (PSRAM too). The picture is drawn off screen in PSRAM and
 * copied out at once.
 *
 * The network: one server at a time, over net_conn (and net_tls for TLS, the usual port 6697). What the
 * network code prints while connecting goes to the server's window.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "irc.h"
#include "mc.h"
#include "mouse.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "version.h"
#include "www_html.h"
#ifndef IRC_PC_TEST
#include "audio.h"
#endif

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height

#define MAXWIN 20         ///< windows open at most
#define NLINES 256        ///< lines each window keeps
#define TEXT_MAX 440      ///< a line's text at most (an IRC message is 512 bytes with its prefix)
#define NICK_MAX 31       ///< a nick's length at most (31 bytes with its end)
#define NAME_MAX 64       ///< a window's name (a channel's) with its end
#define IN_MAX 400        ///< the line being typed at most
#define NHIST 32          ///< lines typed kept for Up and Down
#define RX_MAX 1024       ///< a line received at most (longer ones are cut)
#define SAY_MAX 400       ///< bytes of text in one PRIVMSG (longer lines go as several)
#define CFG_NAME "/IRC.CFG"  ///< where the connect box's settings are kept
#define PING_IDLE_MS 240000  ///< quiet this long: we ping the server
#define DEAD_MS 360000       ///< quiet this long: the connection is taken as lost
#define QUIT_MSG "pico-os IRC " VER_IRC

/// The colours (RGB332), as in the hex editor.
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< bright text
#define C_BODY   0xdb  ///< what is said
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles, our own nick
#define C_CYAN   0x1f  ///< activity
#define C_ORANGE 0xf0  ///< notices, highlights
#define C_RED    0xe0  ///< errors
#define C_GREEN  0x3c  ///< joins, ticks, ops
#define C_PINK   0xe3  ///< actions (/me)
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the menu's choice
#define C_STRIP  0x0b  ///< the topic and status strips
#define C_HIBG   0x60  ///< behind a line that names us
#define SHADOW 6       ///< a drop-down's shadow

/// Layout.
#define BAR_H   16              ///< the menu bar
#define TAB_Y   18              ///< the tabs
#define TAB_H   18              ///< their height
#define TOPIC_Y 38              ///< the topic strip
#define MSG_Y   58              ///< the first row of what is said
#define ROWS    23              ///< rows of it
#define MSG_X   8               ///< its left
#define COLS    78              ///< its columns, without the nick list
#define NL_X    504             ///< the nick list's left
#define NL_COLS 15              ///< its columns
#define COLS_NL ((NL_X - 8 - MSG_X) / 8)  ///< the columns beside the nick list
#define IN_Y    432             ///< the line being typed
#define STAT_Y  456             ///< the status line
#define IN_COLS 76              ///< the typing field's columns

/// What a click hits.
enum {
    HIT_MENU = 1, HIT_ITEM, HIT_MENU_OFF, HIT_TAB, HIT_NICK, HIT_NICKS, HIT_MSGS, HIT_ASK, HIT_FIELD, HIT_TLS,
    HIT_OK, HIT_CANCEL, HIT_SWALLOW,
};

/// The menus' commands.
enum {
    CMD_CONNECT = 1, CMD_RECONNECT, CMD_DISCONNECT, CMD_QUIT,
    CMD_JOIN, CMD_QUERY, CMD_TOPIC, CMD_NICK, CMD_NEXT, CMD_PREV, CMD_CLOSE, CMD_CLEAR,
    CMD_NICKLIST, CMD_TIMES, CMD_JOINS, CMD_COLOURS, CMD_BEEP,
    CMD_HELP, CMD_CMDS, CMD_ABOUT,
    CMD_N_QUERY, CMD_N_WHOIS, CMD_N_OP, CMD_N_DEOP, CMD_N_VOICE, CMD_N_KICK,
};

/// A menu item.
typedef struct {
    const char *text;  ///< NULL ends; "-" is a line between items
    int id;            ///< CMD_*
    const char *key;   ///< the shortcut shown on the right
} hitem_t;

/// A menu on the bar.
typedef struct {
    const char *title;     ///< its title
    const hitem_t *items;  ///< its items
} hmenu_t;

/// The File menu.
static const hitem_t file_items[] = {
    {"Connect...", CMD_CONNECT, "Ctrl-O"}, {"Reconnect", CMD_RECONNECT, "Ctrl-R"},
    {"Disconnect", CMD_DISCONNECT, ""},    {"-", 0, NULL},
    {"Quit", CMD_QUIT, "Esc"},             {NULL, 0, NULL},
};
/// The Channel menu (and the pop-up over what is said).
static const hitem_t chan_items[] = {
    {"Join a channel...", CMD_JOIN, "/join"},  {"Private chat...", CMD_QUERY, "/query"},
    {"Topic...", CMD_TOPIC, "/topic"},         {"Change nick...", CMD_NICK, "/nick"},
    {"-", 0, NULL},                            {"Next window", CMD_NEXT, "Ctrl-N"},
    {"Previous window", CMD_PREV, "Ctrl-P"},   {"Leave and close", CMD_CLOSE, "Ctrl-W"},
    {"Clear the window", CMD_CLEAR, "Ctrl-L"}, {NULL, 0, NULL},
};
/// The View menu.
static const hitem_t view_items[] = {
    {"Nick list", CMD_NICKLIST, "F4"},    {"Times", CMD_TIMES, ""},
    {"Joins and quits", CMD_JOINS, ""},   {"Colours", CMD_COLOURS, ""},
    {"Beep when named", CMD_BEEP, ""},    {NULL, 0, NULL},
};
/// The Help menu.
static const hitem_t help_items[] = {
    {"Keys", CMD_HELP, "F1"}, {"Commands", CMD_CMDS, ""}, {"About", CMD_ABOUT, ""}, {NULL, 0, NULL},
};
/// The pop-up over a nick in the list.
static const hitem_t nick_items[] = {
    {"Private chat", CMD_N_QUERY, ""}, {"Who is it?", CMD_N_WHOIS, ""}, {"-", 0, NULL},
    {"Make operator", CMD_N_OP, ""},   {"Take operator", CMD_N_DEOP, ""},
    {"Give voice", CMD_N_VOICE, ""},   {"Kick", CMD_N_KICK, ""},    {NULL, 0, NULL},
};
/// The menus.
static const hmenu_t menus[] = {
    {"File", file_items}, {"Channel", chan_items}, {"View", view_items}, {"Help", help_items},
};
#define NMENUS 4        ///< their count
#define POP_CHAN NMENUS  ///< the pop-up Channel menu
#define POP_NICK (NMENUS + 1)  ///< the pop-up over a nick
#define MAX_ITEMS 12    ///< the most items a menu has

/// Window kinds.
enum { WIN_SERVER, WIN_CHAN, WIN_QUERY };

/// Kinds of line.
enum { K_MSG, K_SELF, K_ACTION, K_NOTICE, K_JOIN, K_PART, K_INFO, K_ERROR, K_HILITE };

/// The connection's states.
enum { ST_OFF, ST_REG, ST_ON };

/// The boxes over the chat.
enum { DLG_NONE, DLG_ASK, DLG_CONNECT, DLG_INPUT, DLG_HELP, DLG_CMDS, DLG_ABOUT };

/// The connect box's fields.
enum { CF_SERVER, CF_PORT, CF_NICK, CF_CHANS, CF_PASS, CF_N };

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A line said (or about what happened).
typedef struct {
    uint32_t t;              ///< when (seconds since 1970, 0: the clock isn't set)
    uint8_t kind;            ///< K_*
    char nick[NICK_MAX + 1]; ///< who
    char text[TEXT_MAX];     ///< what, in the font's characters, with mIRC's colour codes
} line_t;

/// A nick in a channel.
typedef struct {
    char mode;            ///< '@', '+' and the like, or 0
    char nick[NICK_MAX];  ///< the nick
} nick_t;

/// A window: the server's, a channel's or a private chat's.
typedef struct {
    char name[NAME_MAX];   ///< the channel or nick
    uint8_t type;          ///< WIN_*
    uint8_t unread;        ///< 0 nothing new, 1 comings and goings, 2 talk, 3 talk to us
    bool joined;           ///< we are in the channel
    bool names_open;       ///< a NAMES list is arriving
    char topic[TEXT_MAX];  ///< the channel's topic
    line_t *lines;         ///< a ring of NLINES (PSRAM)
    int head;              ///< the oldest
    int count;             ///< how many
    int scroll;            ///< rows scrolled up from the newest
    nick_t *nicks;         ///< the nick list (PSRAM)
    int nn;                ///< its length
    int ncap;              ///< its room
    int ntop;              ///< the first nick shown
    int nsel;              ///< the nick chosen (-1: none)
} win_t;

/// A character on the screen and its colours.
typedef struct {
    uint8_t c,  ///< the character
        fg,     ///< its colour
        bg,     ///< behind it
        ul;     ///< underlined
} cell_t;

#define CELLS_MAX (TEXT_MAX + 64)  ///< a line's cells at most

/// The client's state (PSRAM).
typedef struct {
    uint8_t *g;               ///< the off-screen picture (PSRAM)
    uint8_t *t;               ///< where drawing goes
    win_t win[MAXWIN];        ///< the windows
    int nwin;                 ///< how many
    int cur;                  ///< the one shown
    // The connection.
    net_conn_t *conn;         ///< the server, or NULL
    net_tls_t *tls;           ///< over TLS, or NULL
    int state;                ///< ST_*
    char rx[RX_MAX + 1];      ///< a line arriving
    int rxlen;                ///< its length
    uint32_t last_rx;         ///< when the server last said anything
    bool ping_sent;           ///< we asked whether it's there
    char nick[NICK_MAX + 1];  ///< our nick now
    char host[64];            ///< the server connected to
    // The settings (the connect box).
    char cf[CF_N][128];       ///< server, port, nick, channels, password
    bool use_tls;             ///< connect with TLS
    // The line being typed.
    char in[IN_MAX + 1];      ///< it
    int inlen;                ///< its length
    int incur;                ///< the cursor in it
    int inleft;               ///< its first character shown
    char hist[NHIST][IN_MAX + 1];  ///< lines typed before
    int nhist;                ///< how many
    int hpos;                 ///< Up and Down's place (nhist: the line being typed)
    // Tab completion.
    bool tab_on;              ///< Tab was the last key
    int tab_at;               ///< where the word being completed starts
    int tab_n;                ///< which match came last
    char tab_word[NICK_MAX];  ///< what was typed of it
    // What is shown.
    bool show_nicks;          ///< the nick list
    bool show_times;          ///< times on the lines
    bool show_joins;          ///< joins, parts and quits
    bool colours;             ///< mIRC's colours
    bool beep;                ///< a beep when we are named or spoken to
    // Menus and boxes.
    int menu;                 ///< the menu open (-1: none, POP_CHAN, POP_NICK)
    int msel;                 ///< its item chosen (-1: none)
    bool marmed;              ///< opened by a button still down
    int popx, popy;           ///< a pop-up's place
    int dlg;                  ///< the box open (DLG_*)
    int field;                ///< the connect box's field being typed in
    int in_cmd;               ///< what the one-line box is for (CMD_*)
    char in_title[48];        ///< its title
    char dl[128];             ///< what is typed in it
    hit_t hits[200];          ///< places to click
    int nhits;                ///< how many
    bool collect;             ///< drawing the whole picture: places to click are kept
    bool dirty;               ///< draw again
    bool quit;                ///< leaving
    int mx, my;               ///< the mouse
    char msg[100];            ///< a passing message on the status line
    uint32_t msg_until;       ///< until when
    // Printing (by the network code) goes to the server's window.
    char cap[160];            ///< a line being printed
    int caplen;               ///< its length
    bool cap_esc;             ///< in an escape sequence
    bool drawing;             ///< drawing now (printing while it doesn't draw again)
    cell_t cells[CELLS_MAX];  ///< a line laid out, for drawing
    cell_t cells2[CELLS_MAX]; ///< and for counting its rows
#ifndef IRC_PC_TEST
    stdio_driver_t cap_driver;  ///< the driver that catches printing
#endif
} irc_t;

static irc_t *I;  ///< the state, while the client runs

static void draw(void);
static void show(void);
static void command(int id);

// ---- Small things ---------------------------------------------------------------------------------------

/// The time since boot.
/// @return milliseconds
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/// Sets a message for the status line (for 4 seconds).
/// @param fmt printf's format
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(I->msg, sizeof I->msg, fmt, ap);
    va_end(ap);
    I->msg_until = now_ms() + 4000;
    I->dirty = true;
}

/// Copies a string, cut to fit.
/// @param d where to
/// @param n its size
/// @param s what
static void copy(char *d, size_t n, const char *s) {
    snprintf(d, n, "%s", s ? s : "");
}

/// Whether a name is a channel's.
/// @param s the name
/// @return it is
static bool is_chan(const char *s) {
    return *s == '#' || *s == '&' || *s == '!' || *s == '+';
}

/// The nick in a "nick!user@host" prefix.
/// @param prefix the prefix (may be NULL)
/// @param out set to the nick
/// @param n its size
static void prefix_nick(const char *prefix, char *out, size_t n) {
    size_t i = 0;
    if (prefix)
        for (; prefix[i] && prefix[i] != '!' && prefix[i] != '@' && i + 1 < n; ++i)
            out[i] = prefix[i];
    out[i] = 0;
}

/// A colour for a nick, the same each time.
/// @param nick the nick
/// @return the colour
static uint8_t nick_colour(const char *nick) {
    static const uint8_t pal[] = {0x1f, 0xfc, 0x1c, 0xf4, 0xe7, 0x9f, 0xdb, 0x7d, 0xf6, 0x3e, 0xbb, 0xfa, 0x5f, 0xd3};
    uint32_t h = 5381;
    for (; *nick; ++nick)
        h = h * 33 + (uint8_t)tolower((unsigned char)*nick);
    return pal[h % sizeof pal];
}

// ---- Windows and their lines ---------------------------------------------------------------------------

/// Finds a window by name (case doesn't matter).
/// @param name the channel or nick
/// @return its number, or -1
static int win_find(const char *name) {
    for (int i = 0; i < I->nwin; ++i)
        if (!strcasecmp(I->win[i].name, name))
            return i;
    return -1;
}

/// Opens a window (or finds the one open).
/// @param name the channel or nick
/// @param type WIN_*
/// @return its number (the server's, 0, if there is no room)
static int win_open(const char *name, int type) {
    int i = win_find(name);
    if (i >= 0)
        return i;
    if (I->nwin >= MAXWIN) {
        message("Too many windows: close some (Ctrl-W)");
        return 0;
    }
    win_t *w = &I->win[I->nwin];
    memset(w, 0, sizeof *w);
    copy(w->name, sizeof w->name, name);
    w->type = (uint8_t)type;
    w->nsel = -1;
    I->dirty = true;
    return I->nwin++;
}

/// Closes a window (not the server's) and frees its lines and nicks.
/// @param i its number
static void win_close(int i) {
    if (i <= 0 || i >= I->nwin)
        return;
    psram_free(I->win[i].lines);
    psram_free(I->win[i].nicks);
    memmove(&I->win[i], &I->win[i + 1], (I->nwin - i - 1) * sizeof *I->win);
    --I->nwin;
    if (I->cur >= i && I->cur > 0)
        --I->cur;
    I->dirty = true;
}

/// Shows a window.
/// @param i its number
static void win_show(int i) {
    if (i < 0 || i >= I->nwin)
        return;
    I->cur = i;
    I->win[i].unread = 0;
    I->tab_on = false;
    I->dirty = true;
}

/// A line of a window.
/// @param w the window
/// @param i which (0: the oldest)
/// @return it
static line_t *line_at(win_t *w, int i) {
    return &w->lines[(w->head + i) % NLINES];
}

/// Whether a line is hidden (joins and quits, when they are).
/// @param l the line
/// @return it is
static bool hidden(const line_t *l) {
    return !I->show_joins && (l->kind == K_JOIN || l->kind == K_PART);
}

static int line_rows(const win_t *w, const line_t *l);

/// Copies text received (UTF-8, or Latin-1 where it isn't) into the font's characters, keeping mIRC's
/// colour codes.
/// @param out where to
/// @param n its size
/// @param s the text
static void to_cp437(char *out, size_t n, const char *s) {
    size_t o = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && o + 1 < n) {
        uint32_t u = *p;
        int len = 1;
        if (u >= 0xc0 && u < 0xf8) {
            int more = u >= 0xf0 ? 3 : u >= 0xe0 ? 2 : 1;
            uint32_t v = u & (0x3f >> more);
            int k;
            for (k = 1; k <= more && (p[k] & 0xc0) == 0x80; ++k)
                v = v << 6 | (p[k] & 0x3f);
            if (k > more)
                u = v, len = more + 1;
        }
        p += len;
        if (u < 0x20) {
            if (u == 2 || u == 3 || u == 4 || u == 0x0f || u == 0x16 || u == 0x1d || u == 0x1e || u == 0x1f)
                out[o++] = (char)u;
            else if (u == '\t')
                out[o++] = ' ';
            continue;
        }
        uint8_t c = u < 0x80 ? (uint8_t)u : www_cp437(u);
        if (c)
            out[o++] = (char)c;
    }
    out[o] = 0;
}

/// Adds a line to a window.
/// @param wi the window's number
/// @param kind K_*
/// @param nick who (may be NULL)
/// @param fmt printf's format for the text (UTF-8)
static void add(int wi, int kind, const char *nick, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add(int wi, int kind, const char *nick, const char *fmt, ...) {
    if (wi < 0 || wi >= I->nwin)
        wi = 0;
    win_t *w = &I->win[wi];
    if (!w->lines && !(w->lines = psram_malloc(NLINES * sizeof *w->lines)))
        return;
    line_t *l;
    if (w->count < NLINES) {
        l = line_at(w, w->count++);
    } else {
        l = line_at(w, 0);
        w->head = (w->head + 1) % NLINES;
    }
    char raw[TEXT_MAX * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    l->t = now > 1600000000 ? (uint32_t)now : 0;
    l->kind = (uint8_t)kind;
    copy(l->nick, sizeof l->nick, nick);
    to_cp437(l->text, sizeof l->text, raw);
    if (w->scroll && !hidden(l))
        w->scroll += line_rows(w, l);  // what is shown stays put
    if (wi != I->cur && !hidden(l)) {
        int level = kind == K_HILITE ? 3 : (kind == K_MSG || kind == K_ACTION || kind == K_NOTICE) ? 2 : 1;
        if (w->type == WIN_QUERY && level == 2)
            level = 3;
        if (level > w->unread)
            w->unread = (uint8_t)level;
    }
    I->dirty = true;
}

/// Adds a line to the window shown.
/// @param kind K_*
/// @param fmt printf's format
#define say_here(kind, ...) add(I->cur, kind, NULL, __VA_ARGS__)

// ---- Nick lists ------------------------------------------------------------------------------------------

/// A mode prefix's rank (owners first).
/// @param m the prefix
/// @return its rank
static int mode_rank(char m) {
    switch (m) {
    case '~': return 0;
    case '&': return 1;
    case '@': return 2;
    case '%': return 3;
    case '+': return 4;
    }
    return 5;
}

/// The order of the nick list: by rank, then by name.
/// @param a a nick
/// @param b another
/// @return <0, 0 or >0
static int nick_cmp(const void *a, const void *b) {
    const nick_t *x = a, *y = b;
    int d = mode_rank(x->mode) - mode_rank(y->mode);
    return d ? d : strcasecmp(x->nick, y->nick);
}

/// Finds a nick in a channel.
/// @param w the channel
/// @param nick the nick
/// @return its place, or -1
static int nick_find(const win_t *w, const char *nick) {
    for (int i = 0; i < w->nn; ++i)
        if (!strcasecmp(w->nicks[i].nick, nick))
            return i;
    return -1;
}

/// Adds a nick to a channel (with its mode prefixes, as NAMES gives them).
/// @param w the channel
/// @param name the nick
/// @param sorted put it in its place (else at the end, sorted later)
static void nick_add(win_t *w, const char *name, bool sorted) {
    char mode = 0;
    while (*name && strchr("~&@%+", *name)) {
        if (!mode || mode_rank(*name) < mode_rank(mode))
            mode = *name;
        ++name;
    }
    if (!*name)
        return;
    int at = nick_find(w, name);
    if (at >= 0) {
        w->nicks[at].mode = mode;
        return;
    }
    if (w->nn == w->ncap) {
        int cap = w->ncap ? w->ncap * 2 : 64;
        nick_t *n = psram_realloc(w->nicks, cap * sizeof *n);
        if (!n)
            return;
        w->nicks = n, w->ncap = cap;
    }
    nick_t e = {mode, {0}};
    copy(e.nick, sizeof e.nick, name);
    int i = w->nn;
    if (sorted)
        while (i > 0 && nick_cmp(&w->nicks[i - 1], &e) > 0)
            --i;
    memmove(&w->nicks[i + 1], &w->nicks[i], (w->nn - i) * sizeof e);
    w->nicks[i] = e;
    ++w->nn;
    I->dirty = true;
}

/// Takes a nick out of a channel.
/// @param w the channel
/// @param nick the nick
/// @return it was there
static bool nick_remove(win_t *w, const char *nick) {
    int i = nick_find(w, nick);
    if (i < 0)
        return false;
    memmove(&w->nicks[i], &w->nicks[i + 1], (w->nn - i - 1) * sizeof *w->nicks);
    --w->nn;
    if (w->nsel >= w->nn)
        w->nsel = w->nn - 1;
    I->dirty = true;
    return true;
}

/// Sets a nick's mode in a channel (and keeps the list in order).
/// @param w the channel
/// @param nick the nick
/// @param mode its prefix, or 0
static void nick_mode(win_t *w, const char *nick, char mode) {
    int i = nick_find(w, nick);
    if (i < 0)
        return;
    char name[NICK_MAX];
    copy(name, sizeof name, w->nicks[i].nick);
    nick_remove(w, name);
    char full[NICK_MAX + 1] = {mode};
    snprintf(full + (mode ? 1 : 0), sizeof full - 1, "%s", name);
    nick_add(w, full, true);
}

// ---- Settings --------------------------------------------------------------------------------------------

/// Reads /IRC.CFG (name=value lines) into the connect box.
static void cfg_load(void) {
    copy(I->cf[CF_SERVER], sizeof I->cf[0], "irc.libera.chat");
    copy(I->cf[CF_PORT], sizeof I->cf[0], "6697");
    copy(I->cf[CF_NICK], sizeof I->cf[0], "pico");
    I->use_tls = true;
    FIL f;
    if (f_open(&f, CFG_NAME, FA_READ) != FR_OK)
        return;
    char buf[1024];
    UINT got = 0;
    f_read(&f, buf, sizeof buf - 1, &got);
    f_close(&f);
    buf[got] = 0;
    static const char *const keys[CF_N] = {"server", "port", "nick", "channels", "password"};
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        for (int i = 0; i < CF_N; ++i)
            if (!strcmp(line, keys[i]))
                copy(I->cf[i], sizeof I->cf[i], eq + 1);
        if (!strcmp(line, "tls"))
            I->use_tls = eq[1] == '1' || eq[1] == 'y';
        if (!strcmp(line, "nicklist"))
            I->show_nicks = eq[1] == '1';
        if (!strcmp(line, "times"))
            I->show_times = eq[1] == '1';
        if (!strcmp(line, "joins"))
            I->show_joins = eq[1] == '1';
        if (!strcmp(line, "colours"))
            I->colours = eq[1] == '1';
        if (!strcmp(line, "beep"))
            I->beep = eq[1] == '1';
    }
}

/// Writes the connect box and the View menu's choices to /IRC.CFG.
static void cfg_save(void) {
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "server=%s\nport=%s\ntls=%d\nnick=%s\nchannels=%s\npassword=%s\n"
                     "nicklist=%d\ntimes=%d\njoins=%d\ncolours=%d\nbeep=%d\n",
                     I->cf[CF_SERVER], I->cf[CF_PORT], I->use_tls, I->cf[CF_NICK], I->cf[CF_CHANS], I->cf[CF_PASS],
                     I->show_nicks, I->show_times, I->show_joins, I->colours, I->beep);
    FIL f;
    UINT put;
    if (f_open(&f, CFG_NAME, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    f_write(&f, buf, (UINT)n, &put);
    f_close(&f);
}

// ---- Talking to the server -------------------------------------------------------------------------------

/// Sends a line to the server (CR LF added).
/// @param fmt printf's format
/// @return it went
static bool send_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool send_line(const char *fmt, ...) {
    if (!I->conn)
        return false;
    char buf[520];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 3)
        n = sizeof buf - 3;
    for (int i = 0; i < n; ++i)  // a line never holds another
        if (buf[i] == '\r' || buf[i] == '\n')
            buf[i] = ' ';
    buf[n++] = '\r';
    buf[n++] = '\n';
    return I->tls ? net_tls_write(I->tls, buf, n) : net_conn_write(I->conn, buf, n);
}

/// Closes the connection (without a QUIT).
static void hang_up(void) {
    if (I->tls)
        net_tls_end(I->tls);
    if (I->conn)
        net_conn_close(I->conn);
    I->tls = NULL;
    I->conn = NULL;
    I->state = ST_OFF;
    I->rxlen = 0;
    for (int i = 0; i < I->nwin; ++i)
        if (I->win[i].type == WIN_CHAN) {
            I->win[i].joined = false;
            I->win[i].nn = 0;
        }
    I->dirty = true;
}

/// Says goodbye to the server and closes the connection.
/// @param why the quit message
static void disconnect(const char *why) {
    if (!I->conn)
        return;
    send_line("QUIT :%s", why && *why ? why : QUIT_MSG);
    sleep_ms(200);  // for it to go out
    hang_up();
    add(0, K_INFO, NULL, "Disconnected");
}

/// Connects to the server in the connect box, and logs in (the rest when the server welcomes us).
static void connect_now(void) {
    if (I->conn)
        disconnect(NULL);
    const char *host = I->cf[CF_SERVER];
    int port = atoi(I->cf[CF_PORT]);
    if (!*host) {
        message("No server: File > Connect");
        return;
    }
    if (port <= 0 || port > 65535)
        port = I->use_tls ? 6697 : 6667;
    copy(I->host, sizeof I->host, host);
    copy(I->win[0].name, sizeof I->win[0].name, host);
    win_show(0);
    add(0, K_INFO, NULL, "Connecting to %s port %d%s...", host, port, I->use_tls ? " (TLS)" : "");
    draw();
    show();
    if (!net_need_link())
        return;
    bool verify = I->use_tls && net_tls_prepare();
    if (I->use_tls && !verify)
        add(0, K_ERROR, NULL, "No CACERT.PEM: the server's certificate isn't checked");
    if (!(I->conn = net_conn_open(host, (uint16_t)port))) {
        add(0, K_ERROR, NULL, "Not connected");
        return;
    }
    if (I->use_tls && !(I->tls = net_tls_start(I->conn, host, verify))) {
        net_conn_close(I->conn);
        I->conn = NULL;
        add(0, K_ERROR, NULL, "The TLS handshake failed");
        return;
    }
    I->state = ST_REG;
    I->rxlen = 0;
    I->last_rx = now_ms();
    I->ping_sent = false;
    copy(I->nick, sizeof I->nick, *I->cf[CF_NICK] ? I->cf[CF_NICK] : "pico");
    if (*I->cf[CF_PASS])
        send_line("PASS %s", I->cf[CF_PASS]);
    send_line("NICK %s", I->nick);
    send_line("USER %s 0 * :pico-os IRC user", I->nick);
    add(0, K_INFO, NULL, "Connected; logging in as %s", I->nick);
}

/// The server has welcomed us: joins the channels open before (after a reconnect) or those in the
/// connect box.
static void join_channels(void) {
    bool any = false;
    for (int i = 0; i < I->nwin; ++i)
        if (I->win[i].type == WIN_CHAN) {
            send_line("JOIN %s", I->win[i].name);
            any = true;
        }
    if (!any && *I->cf[CF_CHANS]) {
        char list[128];
        copy(list, sizeof list, I->cf[CF_CHANS]);
        for (char *c = strtok(list, ", "); c; c = strtok(NULL, ", "))
            send_line("JOIN %s%s", is_chan(c) ? "" : "#", c);
    }
}

/// Whether text names us (our nick as a word).
/// @param text the text
/// @return it does
static bool names_us(const char *text) {
    size_t n = strlen(I->nick);
    if (!n)
        return false;
    for (const char *p = text; *p; ++p) {
        if (strncasecmp(p, I->nick, n))
            continue;
        bool before = p == text || !(isalnum((unsigned char)p[-1]) || strchr("_-[]\\`^{}|", p[-1]));
        char a = p[n];
        bool after = !(isalnum((unsigned char)a) || strchr("_-[]\\`^{}|", a)) || !a;
        if (before && after)
            return true;
    }
    return false;
}

/// A beep, when they are wanted.
static void ding(void) {
#ifndef IRC_PC_TEST
    if (I->beep)
        audio_beep(1200, 80);
#endif
}

/// A message (PRIVMSG or NOTICE) received.
/// @param from who
/// @param to the channel or us
/// @param text what
/// @param notice a NOTICE
static void got_message(const char *from, const char *to, char *text, bool notice) {
    bool to_chan = is_chan(to);
    size_t tl = strlen(text);
    // CTCP: \1WORD args\1
    if (text[0] == 1) {
        if (tl > 1 && text[tl - 1] == 1)
            text[--tl] = 0;
        char *word = text + 1, *args = strchr(word, ' ');
        if (args)
            *args++ = 0;
        if (!strcasecmp(word, "ACTION")) {
            int wi = to_chan ? win_find(to) : win_open(from, WIN_QUERY);
            bool hi = names_us(args ? args : "");
            add(wi < 0 ? 0 : wi, hi ? K_HILITE : K_ACTION, from, "* %s %s", from, args ? args : "");
            if (hi || !to_chan)
                ding();
        } else if (!notice) {
            if (!strcasecmp(word, "VERSION"))
                send_line("NOTICE %s :\1VERSION pico-os IRC " VER_IRC " (Raspberry Pi Pico 2)\1", from);
            else if (!strcasecmp(word, "PING"))
                send_line("NOTICE %s :\1PING %s\1", from, args ? args : "");
            else if (!strcasecmp(word, "TIME")) {
                time_t now = time(NULL);
                char tbuf[40];
                strftime(tbuf, sizeof tbuf, "%a %b %d %H:%M:%S %Y", localtime(&now));
                send_line("NOTICE %s :\1TIME %s\1", from, tbuf);
            }
            add(0, K_INFO, NULL, "%s asked for CTCP %s", from, word);
        } else {
            add(I->cur, K_NOTICE, from, "CTCP %s reply: %s", word, args ? args : "");
        }
        return;
    }
    if (notice) {
        int wi = to_chan ? win_find(to) : win_find(from);
        if (wi < 0)
            wi = I->state == ST_ON && strchr(from, '.') == NULL ? I->cur : 0;  // servers' names have dots
        add(wi, K_NOTICE, from, "%s", text);
        return;
    }
    if (to_chan) {
        int wi = win_find(to);
        bool hi = names_us(text);
        add(wi < 0 ? 0 : wi, hi ? K_HILITE : K_MSG, from, "%s", text);
        if (hi)
            ding();
    } else {
        add(win_open(from, WIN_QUERY), K_MSG, from, "%s", text);
        ding();
    }
}

/// Shows a numeric reply's words after our nick in the server's window.
/// @param a the parameters
/// @param n how many
/// @param wi the window
static void show_params(char **a, int n, int wi) {
    char buf[TEXT_MAX];
    int o = 0;
    buf[0] = 0;
    for (int i = 1; i < n && o < (int)sizeof buf - 1; ++i)
        o += snprintf(buf + o, sizeof buf - o, "%s%s", i > 1 ? " " : "", a[i]);
    add(wi, K_INFO, NULL, "%s", buf);
}

/// A line from the server: [@tags] [:prefix] COMMAND params [:trailing].
/// @param line it (changed)
static void handle_line(char *line) {
    char *p = line;
    if (*p == '@') {  // IRCv3 tags: not asked for, skipped
        p = strchr(p, ' ');
        if (!p)
            return;
        while (*p == ' ')
            ++p;
    }
    char *prefix = NULL;
    if (*p == ':') {
        prefix = p + 1;
        p = strchr(p, ' ');
        if (!p)
            return;
        *p++ = 0;
        while (*p == ' ')
            ++p;
    }
    char *cmd = p, *a[16];
    int n = 0;
    p = strchr(p, ' ');
    if (p) {
        *p++ = 0;
        while (*p && n < 16) {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            if (*p == ':') {
                a[n++] = p + 1;
                break;
            }
            a[n++] = p;
            p = strchr(p, ' ');
            if (!p)
                break;
            *p++ = 0;
        }
    }
    for (int i = n; i < 16; ++i)
        a[i] = "";
    char from[NICK_MAX + 1];
    prefix_nick(prefix, from, sizeof from);
    const char *host = prefix && strchr(prefix, '!') ? strchr(prefix, '!') + 1 : "";
    bool me = !strcasecmp(from, I->nick);

    if (!strcmp(cmd, "PING")) {
        send_line("PONG :%s", a[0]);
    } else if (!strcmp(cmd, "PONG")) {
        // the answer to our ping: last_rx is enough
    } else if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
        got_message(*from ? from : I->host, a[0], a[1], cmd[0] == 'N');
    } else if (!strcmp(cmd, "JOIN")) {
        int wi = win_open(a[0], WIN_CHAN);
        win_t *w = &I->win[wi];
        if (me) {
            w->joined = true;
            w->nn = 0;
            w->names_open = false;
            win_show(wi);
            add(wi, K_JOIN, from, "You joined %s", a[0]);
        } else {
            nick_add(w, from, true);
            add(wi, K_JOIN, from, "%s (%s) joined", from, host);
        }
    } else if (!strcmp(cmd, "PART")) {
        int wi = win_find(a[0]);
        if (wi > 0) {
            if (me) {
                I->win[wi].joined = false;
                I->win[wi].nn = 0;
                add(wi, K_PART, from, "You left %s", a[0]);
            } else {
                nick_remove(&I->win[wi], from);
                add(wi, K_PART, from, "%s left%s%s", from, *a[1] ? ": " : "", a[1]);
            }
        }
    } else if (!strcmp(cmd, "KICK")) {
        int wi = win_find(a[0]);
        if (wi > 0) {
            bool us = !strcasecmp(a[1], I->nick);
            if (us) {
                I->win[wi].joined = false;
                I->win[wi].nn = 0;
            } else {
                nick_remove(&I->win[wi], a[1]);
            }
            add(wi, us ? K_ERROR : K_PART, from, "%s was kicked by %s (%s)", us ? "You" : a[1], from, a[2]);
        }
    } else if (!strcmp(cmd, "QUIT")) {
        for (int i = 1; i < I->nwin; ++i)
            if ((I->win[i].type == WIN_CHAN && nick_remove(&I->win[i], from)) ||
                (I->win[i].type == WIN_QUERY && !strcasecmp(I->win[i].name, from)))
                add(i, K_PART, from, "%s quit%s%s", from, *a[0] ? ": " : "", a[0]);
    } else if (!strcmp(cmd, "NICK")) {
        if (me) {
            copy(I->nick, sizeof I->nick, a[0]);
            add(0, K_INFO, NULL, "You are now %s", a[0]);
        }
        for (int i = 1; i < I->nwin; ++i) {
            win_t *w = &I->win[i];
            int at = w->type == WIN_CHAN ? nick_find(w, from) : -1;
            if (at >= 0) {
                char full[NICK_MAX + 1] = {w->nicks[at].mode};
                snprintf(full + (full[0] ? 1 : 0), sizeof full - 1, "%s", a[0]);
                nick_remove(w, from);
                nick_add(w, full, true);
                add(i, K_INFO, NULL, "%s is now %s", from, a[0]);
            } else if (w->type == WIN_QUERY && !strcasecmp(w->name, from)) {
                copy(w->name, sizeof w->name, a[0]);
                add(i, K_INFO, NULL, "%s is now %s", from, a[0]);
            }
        }
    } else if (!strcmp(cmd, "TOPIC")) {
        int wi = win_find(a[0]);
        if (wi > 0) {
            to_cp437(I->win[wi].topic, sizeof I->win[wi].topic, a[1]);
            add(wi, K_INFO, NULL, "%s set the topic: %s", from, a[1]);
        }
    } else if (!strcmp(cmd, "MODE")) {
        int wi = is_chan(a[0]) ? win_find(a[0]) : 0;
        if (wi > 0) {
            // +o nick, -v nick and the like change the nick list.
            const char *m = a[1];
            bool plus = true;
            int arg = 2;
            for (; *m; ++m) {
                if (*m == '+' || *m == '-') {
                    plus = *m == '+';
                    continue;
                }
                const char *sym = strchr("qaohv", *m) ? "~&@%+" + (strchr("qaohv", *m) - "qaohv") : NULL;
                if (sym && arg < n) {
                    nick_mode(&I->win[wi], a[arg], plus ? *sym : 0);
                    ++arg;
                } else if (strchr("bkleIfjL", *m) && (plus || *m != 'l') && arg < n) {
                    ++arg;  // a mode that takes a word
                }
            }
        }
        char rest[TEXT_MAX];
        int o = 0;
        rest[0] = 0;
        for (int i = 1; i < n; ++i)
            o += snprintf(rest + o, sizeof rest - o, " %s", a[i]);
        add(wi < 0 ? 0 : wi, K_INFO, NULL, "%s sets mode%s", *from ? from : I->host, rest);
    } else if (!strcmp(cmd, "ERROR")) {
        add(0, K_ERROR, NULL, "%s", a[0]);
    } else if (!strcmp(cmd, "001")) {
        I->state = ST_ON;
        if (*a[0])
            copy(I->nick, sizeof I->nick, a[0]);
        show_params(a, n, 0);
        join_channels();
    } else if (!strcmp(cmd, "433") || !strcmp(cmd, "432")) {  // nick taken or not allowed
        add(I->state == ST_ON ? I->cur : 0, K_ERROR, NULL, "%s: %s", a[1], a[2]);
        if (I->state == ST_REG) {
            size_t l = strlen(I->nick);
            if (l < 15)
                I->nick[l] = '_', I->nick[l + 1] = 0;
            else
                I->nick[l - 1] = (char)('0' + rand() % 10);
            send_line("NICK %s", I->nick);
        }
    } else if (!strcmp(cmd, "332")) {  // the topic, on joining
        int wi = win_find(a[1]);
        if (wi > 0) {
            to_cp437(I->win[wi].topic, sizeof I->win[wi].topic, a[2]);
            add(wi, K_INFO, NULL, "Topic: %s", a[2]);
        }
    } else if (!strcmp(cmd, "333")) {  // who set it
        int wi = win_find(a[1]);
        if (wi > 0) {
            char who[NICK_MAX + 1];
            prefix_nick(a[2], who, sizeof who);
            time_t t = atol(a[3]);
            char when[40] = "";
            if (t)
                strftime(when, sizeof when, " on %d %b %Y", localtime(&t));
            add(wi, K_INFO, NULL, "Set by %s%s", who, when);
        }
    } else if (!strcmp(cmd, "331")) {
        int wi = win_find(a[1]);
        add(wi < 0 ? 0 : wi, K_INFO, NULL, "No topic is set");
    } else if (!strcmp(cmd, "353")) {  // NAMES: me = channel :nicks
        int wi = win_find(a[2]);
        if (wi > 0) {
            win_t *w = &I->win[wi];
            if (!w->names_open)
                w->nn = 0, w->names_open = true;
            for (char *s = strtok(a[3], " "); s; s = strtok(NULL, " "))
                nick_add(w, s, false);
        } else {
            show_params(a, n, I->cur);
        }
    } else if (!strcmp(cmd, "366")) {  // the end of NAMES
        int wi = win_find(a[1]);
        if (wi > 0) {
            win_t *w = &I->win[wi];
            w->names_open = false;
            qsort(w->nicks, w->nn, sizeof *w->nicks, nick_cmp);
            add(wi, K_INFO, NULL, "%d people here", w->nn);
        }
    } else if (!strcmp(cmd, "372") || !strcmp(cmd, "375") || !strcmp(cmd, "376")) {  // the MOTD
        if (cmd[2] == '2')
            add(0, K_INFO, NULL, "%s", a[1]);
    } else if (cmd[0] >= '0' && cmd[0] <= '9') {
        int num = atoi(cmd);
        // Errors and whois answers go where we are; the rest to the server's window.
        bool here = (num >= 400 && num < 600) || (num >= 311 && num <= 319) || num == 301 || num == 330 ||
                    num == 338 || num == 671 || num == 378;
        int wi = here ? I->cur : 0;
        if (num >= 400 && num < 600)
            add(wi, K_ERROR, NULL, "%s%s%s", n > 2 ? a[1] : "", n > 2 ? ": " : "", a[n > 1 ? n - 1 : 0]);
        else
            show_params(a, n, wi);
    } else {
        add(0, K_INFO, NULL, "%s %s %s %s", cmd, a[0], a[1], a[2]);
    }
}

/// Reads what the server has sent and handles each whole line.
static void poll_server(void) {
    if (!I->conn)
        return;
    for (int rounds = 0; rounds < 8; ++rounds) {
        char buf[512];
        int got = I->tls ? net_tls_read(I->tls, buf, sizeof buf) : net_conn_read(I->conn, buf, sizeof buf);
        if (got < 0) {
            hang_up();
            add(0, K_ERROR, NULL, "The server closed the connection (Ctrl-R reconnects)");
            for (int i = 1; i < I->nwin; ++i)
                if (I->win[i].type == WIN_CHAN)
                    add(i, K_ERROR, NULL, "Disconnected");
            return;
        }
        if (got == 0)
            break;
        I->last_rx = now_ms();
        I->ping_sent = false;
        for (int i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (I->rxlen) {
                    I->rx[I->rxlen] = 0;
                    I->rxlen = 0;
                    handle_line(I->rx);
                    if (!I->conn)
                        return;
                }
            } else if (I->rxlen < RX_MAX) {
                I->rx[I->rxlen++] = c ? c : ' ';
            }
        }
    }
    uint32_t quiet = now_ms() - I->last_rx;
    if (quiet > DEAD_MS) {
        hang_up();
        add(0, K_ERROR, NULL, "The server stopped answering (Ctrl-R reconnects)");
    } else if (quiet > PING_IDLE_MS && !I->ping_sent) {
        send_line("PING :%s", I->host);
        I->ping_sent = true;
    }
}

// ---- Lines typed -----------------------------------------------------------------------------------------

/// Says text in the window shown (split into lines of SAY_MAX bytes).
/// @param text what
/// @param action as /me
static void say(const char *text, bool action) {
    win_t *w = &I->win[I->cur];
    if (I->cur == 0) {
        message("This is the server's window: /join a channel, or /msg someone");
        return;
    }
    if (I->state != ST_ON) {
        message("Not connected (Ctrl-R reconnects)");
        return;
    }
    size_t len = strlen(text);
    do {
        size_t n = len > SAY_MAX ? SAY_MAX : len;
        if (n < len)  // a break between words
            for (size_t k = n; k > SAY_MAX / 2; --k)
                if (text[k] == ' ') {
                    n = k;
                    break;
                }
        if (action) {
            send_line("PRIVMSG %s :\1ACTION %.*s\1", w->name, (int)n, text);
            add(I->cur, K_ACTION, I->nick, "* %s %.*s", I->nick, (int)n, text);
        } else {
            send_line("PRIVMSG %s :%.*s", w->name, (int)n, text);
            add(I->cur, K_SELF, I->nick, "%.*s", (int)n, text);
        }
        text += n;
        len -= n;
        while (*text == ' ')
            ++text, --len;
    } while (len);
}

/// True (and a message) if not connected.
/// @return not connected
static bool offline(void) {
    if (I->state == ST_ON)
        return false;
    message(I->state == ST_REG ? "Still logging in" : "Not connected (Ctrl-R reconnects, Ctrl-O connects)");
    return true;
}

/// Opens a private chat.
/// @param nick with whom
static void query(const char *nick) {
    if (*nick)
        win_show(win_open(nick, WIN_QUERY));
}

/// A line typed: a /command, or something said.
/// @param line it
static void typed(char *line) {
    if (line[0] != '/' || line[1] == '/') {
        if (*line)
            say(line[0] == '/' ? line + 1 : line, false);
        return;
    }
    char *cmd = line + 1, *rest = strchr(cmd, ' ');
    if (rest)
        for (*rest++ = 0; *rest == ' '; ++rest) {
        }
    else
        rest = "";
    char *arg1 = rest, *arg2 = strchr(rest, ' ');
    char first[128];
    copy(first, sizeof first, arg1);
    if (strchr(first, ' '))
        *strchr(first, ' ') = 0;
    arg2 = arg2 ? arg2 + 1 : "";
    while (*arg2 == ' ')
        ++arg2;
    win_t *w = &I->win[I->cur];
    if (!strcasecmp(cmd, "join") || !strcasecmp(cmd, "j")) {
        if (!*first)
            message("/join #channel");
        else if (!offline())
            send_line("JOIN %s%s%s%s", is_chan(first) ? "" : "#", first, *arg2 ? " " : "", arg2);
    } else if (!strcasecmp(cmd, "part") || !strcasecmp(cmd, "leave")) {
        const char *chan = is_chan(first) ? first : w->name;
        const char *why = is_chan(first) ? arg2 : rest;
        if (!offline() && is_chan(chan))
            send_line("PART %s :%s", chan, *why ? why : QUIT_MSG);
    } else if (!strcasecmp(cmd, "msg") || !strcasecmp(cmd, "m")) {
        if (!*first || !*arg2) {
            message("/msg nick text");
        } else if (!offline()) {
            send_line("PRIVMSG %s :%s", first, arg2);
            int wi = win_find(first);
            add(wi >= 0 ? wi : I->cur, K_SELF, I->nick, "%s%s", wi >= 0 ? "" : "-> ", arg2);
            if (wi < 0)
                add(I->cur, K_INFO, NULL, "(to %s)", first);
        }
    } else if (!strcasecmp(cmd, "query") || !strcasecmp(cmd, "q")) {
        query(first);
        if (*first && *arg2 && !offline())
            say(arg2, false);
    } else if (!strcasecmp(cmd, "me")) {
        say(rest, true);
    } else if (!strcasecmp(cmd, "notice")) {
        if (*first && *arg2 && !offline()) {
            send_line("NOTICE %s :%s", first, arg2);
            add(I->cur, K_NOTICE, I->nick, "-> %s: %s", first, arg2);
        }
    } else if (!strcasecmp(cmd, "nick")) {
        if (!*first) {
            message("/nick newnick");
        } else if (I->state == ST_OFF) {
            copy(I->cf[CF_NICK], sizeof I->cf[0], first);
            message("Your nick will be %s", first);
        } else {
            send_line("NICK %s", first);
        }
    } else if (!strcasecmp(cmd, "topic")) {
        if (w->type != WIN_CHAN)
            message("/topic is for a channel");
        else if (!offline())
            *rest ? send_line("TOPIC %s :%s", w->name, rest) : send_line("TOPIC %s", w->name);
    } else if (!strcasecmp(cmd, "names")) {
        const char *chan = *first ? first : w->name;
        if (!offline())
            send_line("NAMES %s", chan);
    } else if (!strcasecmp(cmd, "whois") || !strcasecmp(cmd, "wi")) {
        if (!offline() && *first)
            send_line("WHOIS %s", first);
    } else if (!strcasecmp(cmd, "away")) {
        if (!offline())
            *rest ? send_line("AWAY :%s", rest) : send_line("AWAY");
    } else if (!strcasecmp(cmd, "ctcp")) {
        if (!offline() && *first && *arg2)
            send_line("PRIVMSG %s :\1%s\1", first, arg2);
    } else if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit")) {
        disconnect(rest);
        I->quit = true;
    } else if (!strcasecmp(cmd, "disconnect")) {
        disconnect(rest);
    } else if (!strcasecmp(cmd, "close") || !strcasecmp(cmd, "wc")) {
        command(CMD_CLOSE);
    } else if (!strcasecmp(cmd, "clear")) {
        command(CMD_CLEAR);
    } else if (!strcasecmp(cmd, "server") || !strcasecmp(cmd, "connect")) {
        if (*first) {
            copy(I->cf[CF_SERVER], sizeof I->cf[0], first);
            const char *port = *arg2 ? arg2 : "";
            I->use_tls = *port == '+' || (!*port && I->use_tls) || atoi(port) == 6697;
            copy(I->cf[CF_PORT], sizeof I->cf[0], *port ? port + (*port == '+') : I->use_tls ? "6697" : "6667");
        }
        connect_now();
    } else if (!strcasecmp(cmd, "raw") || !strcasecmp(cmd, "quote")) {
        if (!*rest)
            message("/raw IRC LINE");
        else if (I->conn)
            send_line("%s", rest), add(0, K_INFO, NULL, "-> %s", rest);
    } else if (!strcasecmp(cmd, "op") || !strcasecmp(cmd, "deop") || !strcasecmp(cmd, "voice") ||
               !strcasecmp(cmd, "devoice")) {
        bool plus = cmd[0] != 'd' && cmd[0] != 'D';
        char c = tolower((unsigned char)cmd[plus ? 0 : 2]) == 'o' ? 'o' : 'v';
        if (w->type == WIN_CHAN && *first && !offline())
            send_line("MODE %s %c%c %s", w->name, plus ? '+' : '-', c, first);
    } else if (!strcasecmp(cmd, "kick")) {
        if (w->type == WIN_CHAN && *first && !offline())
            send_line("KICK %s %s :%s", w->name, first, *arg2 ? arg2 : "");
    } else if (!strcasecmp(cmd, "help")) {
        command(CMD_CMDS);
    } else if (I->conn) {  // the server's own commands (/list, /mode, /invite, /who ...) go as they are
        for (char *c = cmd; *c; ++c)
            *c = (char)toupper((unsigned char)*c);
        *rest ? send_line("%s %s", cmd, rest) : send_line("%s", cmd);
    } else {
        message("Not connected: /%s isn't one of mine", cmd);
    }
}

// ---- Drawing -------------------------------------------------------------------------------------------

/// Fills a rectangle (clipped to the screen).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; ++r)
        memset(I->t + (y + r) * W + x, c, w);
}

/// Draws a character of the 8x16 font.
/// @param x left
/// @param y top
/// @param c the character
/// @param fg its colour
/// @param bg the background
static void ch(int x, int y, uint8_t c, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x > W - 8 || y > H - 16)
        return;
    const uint8_t *gl = font8x16[c];
    for (int r = 0; r < 16; ++r) {
        uint8_t *p = I->t + (y + r) * W + x;
        for (int i = 0; i < 8; ++i)
            p[i] = gl[r] & (0x80 >> i) ? fg : bg;
    }
}

/// Draws text, up to `max` characters.
/// @param x left
/// @param y top
/// @param s the text
/// @param max the most characters
/// @param fg its colour
/// @param bg the background
/// @return the x after it
static int textn(int x, int y, const char *s, int max, uint8_t fg, uint8_t bg) {
    for (; *s && max > 0; ++s, x += 8, --max)
        ch(x, y, (uint8_t)*s, fg, bg);
    return x;
}

/// Draws text.
/// @param x left
/// @param y top
/// @param s the text
/// @param fg its colour
/// @param bg the background
/// @return the x after it
static int text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    return textn(x, y, s, 1000, fg, bg);
}

/// Draws formatted text.
/// @param x left
/// @param y top
/// @param fg its colour
/// @param bg the background
/// @param fmt printf's format
/// @return the x after it
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int textf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(x, y, buf, fg, bg);
}

/// A bevelled rectangle.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param raised raised (else sunken)
/// @param face its colour
static void bevel(int x, int y, int w, int h, bool raised, uint8_t face) {
    fill(x, y, w, h, face);
    uint8_t a = raised ? C_LIGHT : C_DARK, b = raised ? C_DARK : C_LIGHT;
    fill(x, y, w, 1, a);
    fill(x, y, 1, h, a);
    fill(x, y + h - 1, w, 1, b);
    fill(x + w - 1, y, 1, h, b);
}

/// Remembers a place to click (only while the whole picture is drawn).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param id HIT_*
/// @param arg which
static void hit(int x, int y, int w, int h, int id, int arg) {
    if (I->collect && I->nhits < (int)(sizeof I->hits / sizeof *I->hits))
        I->hits[I->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = I->nhits - 1; i >= 0; --i) {
        const hit_t *h = &I->hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
            return h;
    }
    return NULL;
}

/// A button: pressed-in and yellow when lit.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param label its text
/// @param lit pressed
/// @param id what a click on it does (0: nothing)
/// @param arg which
static void button(int x, int y, int w, int h, const char *label, bool lit, int id, int arg) {
    uint8_t face = lit ? C_SEL : C_FACE;
    bevel(x, y, w, h, !lit, face);
    int n = strlen(label);
    text(x + (w - n * 8) / 2, y + (h - 16) / 2, label, lit ? C_YELLOW : C_TEXT, face);
    if (id)
        hit(x, y, w, h, id, arg);
}

/// mIRC's 16 colours in RGB332 (black is grey here, on the black background).
static const uint8_t mirc[16] = {0xff, 0x49, 0x03, 0x10, 0xe0, 0x84, 0x82, 0xf0,
                                 0xfc, 0x1c, 0x12, 0x1f, 0x4b, 0xe3, 0x92, 0xb6};

/// The width that what is said is wrapped to in a window.
/// @param w the window
/// @return columns
static int msg_cols(const win_t *w) {
    return I->show_nicks && w->type == WIN_CHAN ? COLS_NL : COLS;
}

/// Lays out a line as cells: the time, the nick (as the kind of line has it) and the text in its colours.
/// @param l the line
/// @param out the cells
/// @param max room in out
/// @param indent set to where the text starts (wrapped rows start there)
/// @return the cells
static int line_cells(const line_t *l, cell_t *out, int max, int *indent) {
    int n = 0;
    uint8_t base = C_BODY, bg0 = l->kind == K_HILITE ? C_HIBG : C_BG;
#define PUT(ch_, fg_) \
    do { if (n < max) out[n++] = (cell_t){(uint8_t)(ch_), (fg_), bg0, 0}; } while (0)
#define PUTS(s_, fg_) \
    do { for (const char *q_ = (s_); *q_; ++q_) PUT(*q_, fg_); } while (0)
    if (I->show_times) {
        char tb[8] = "--:-- ";
        if (l->t) {
            time_t t = l->t;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(tb, sizeof tb, "%02d:%02d ", tm.tm_hour, tm.tm_min);
        }
        PUTS(tb, C_DIM);
    }
    switch (l->kind) {
    case K_MSG: case K_SELF: case K_HILITE:
        PUT('<', C_DIM);
        PUTS(l->nick, l->kind == K_SELF ? C_YELLOW : nick_colour(l->nick));
        PUTS("> ", C_DIM);
        if (l->kind == K_SELF)
            base = C_TEXT;
        break;
    case K_ACTION: base = C_PINK; break;
    case K_NOTICE:
        PUT('-', C_ORANGE);
        PUTS(l->nick, C_ORANGE);
        PUTS("- ", C_ORANGE);
        break;
    case K_JOIN: PUTS("--> ", C_GREEN); base = C_DIM; break;
    case K_PART: PUTS("<-- ", C_RED); base = C_DIM; break;
    case K_INFO: PUTS("-- ", C_CYAN); base = C_LABEL; break;
    case K_ERROR: PUTS("!! ", C_RED); base = C_ORANGE; break;
    }
    *indent = n;
    // The text, with mIRC's codes: ^B bold, ^C colours, ^O plain, ^V reversed, ^_ underlined.
    uint8_t fg = base, bg = bg0;
    bool bold = false, rev = false, ul = false;
    for (const uint8_t *p = (const uint8_t *)l->text; *p && n < max; ++p) {
        uint8_t c = *p;
        if (c == 2) {
            bold = !bold;
        } else if (c == 3) {
            int f = -1, b = -1;
            if (isdigit(p[1])) {
                f = *++p - '0';
                if (isdigit(p[1]))
                    f = f * 10 + *++p - '0';
                if (p[1] == ',' && isdigit(p[2])) {
                    ++p;
                    b = *++p - '0';
                    if (isdigit(p[1]))
                        b = b * 10 + *++p - '0';
                }
            }
            if (!I->colours)
                continue;
            if (f < 0)
                fg = base, bg = bg0;
            if (f >= 0 && f < 99)
                fg = mirc[f % 16];
            if (b >= 0 && b < 99)
                bg = b % 16 == 1 ? C_BG : mirc[b % 16];
        } else if (c == 4) {  // ^D: a colour in hex (shown plain)
            for (int k = 0; k < 6 && isxdigit(p[1]); ++k)
                ++p;
            if (p[1] == ',')
                for (++p; isxdigit(p[1]); ++p) {
                }
        } else if (c == 0x0f) {
            bold = rev = ul = false;
            fg = base, bg = bg0;
        } else if (c == 0x16) {
            rev = !rev;
        } else if (c == 0x1f) {
            ul = !ul;
        } else if (c >= 0x20 || c == 0) {
            uint8_t f2 = fg, b2 = bg;
            if (bold && f2 == base)
                f2 = C_TEXT;
            if (rev && I->colours)
                f2 = bg == C_BG ? C_BG : bg, b2 = fg;
            if (rev && I->colours && f2 == b2)
                f2 = C_BG, b2 = C_BODY;
            out[n++] = (cell_t){c, f2, b2, (uint8_t)(ul && I->colours)};
        } else if (c != 0x1d && c != 0x1e) {
            PUT(c, fg);
        }
    }
#undef PUT
#undef PUTS
    return n;
}

/// Wraps cells into rows: between words where it can, the rows after the first starting at `indent`.
/// @param c the cells
/// @param n how many
/// @param cols the width
/// @param indent where rows after the first start
/// @param start set to each row's first cell
/// @param len set to each row's length
/// @param max room in start and len
/// @return the rows
static int wrap(const cell_t *c, int n, int cols, int indent, int16_t *start, int16_t *len, int max) {
    if (indent > cols / 3)
        indent = 2;
    int rows = 0, i = 0;
    do {
        int room = rows ? cols - indent : cols;
        if (rows)
            while (i < n && c[i].c == ' ')
                ++i;
        int end = i + room;
        if (end >= n) {
            end = n;
        } else {
            int k = end;
            while (k > i + room / 2 && c[k].c != ' ')
                --k;
            if (k > i + room / 2)
                end = k;
        }
        if (rows < max)
            start[rows] = (int16_t)i, len[rows] = (int16_t)(end - i);
        ++rows;
        i = end;
    } while (i < n && rows < max);
    return rows;
}

/// The rows a line takes in a window.
/// @param w the window
/// @param l the line
/// @return them
static int line_rows(const win_t *w, const line_t *l) {
    int16_t st[64], ln[64];
    int ind, n = line_cells(l, I->cells2, CELLS_MAX, &ind);
    return wrap(I->cells2, n, msg_cols(w), ind, st, ln, 64);
}

/// All the rows a window's lines take.
/// @param w the window
/// @return them
static int total_rows(const win_t *w) {
    int t = 0;
    for (int i = 0; i < w->count; ++i)
        if (!hidden(line_at((win_t *)w, i)))
            t += line_rows(w, line_at((win_t *)w, i));
    return t;
}

/// Scrolls the window shown.
/// @param rows up (+) or down (-)
static void scroll(int rows) {
    win_t *w = &I->win[I->cur];
    int max = total_rows(w) - ROWS;
    w->scroll += rows;
    if (w->scroll > max)
        w->scroll = max;
    if (w->scroll < 0)
        w->scroll = 0;
    I->dirty = true;
}

/// A tab's width.
/// @param i the window
/// @return pixels
static int tab_w(int i) {
    int n = (int)strlen(I->win[i].name);
    if (n > 14)
        n = 14;
    return (n + 3) * 8 + 4;
}

/// Draws the menu bar, as the tracker's.
static void draw_bar(void);

/// Draws the tabs: a window each, lit when something new is said there.
static void draw_tabs(void) {
    fill(0, TAB_Y - 2, W, TAB_H + 2, C_BG);
    // From the first tab such that the one shown fits.
    int first = 0, x = 4;
    for (int i = 0; i <= I->cur; ++i)
        x += tab_w(i) + 2;
    while (x > W - 4 && first < I->cur)
        x -= tab_w(first++) + 2;
    x = 4;
    if (first)
        ch(x, TAB_Y + 1, 0x11, C_LABEL, C_BG), x += 10;
    for (int i = first; i < I->nwin; ++i) {
        win_t *w = &I->win[i];
        int tw = tab_w(i);
        if (x + tw > W - 4) {
            ch(W - 12, TAB_Y + 1, 0x10, C_LABEL, C_BG);
            break;
        }
        bool on = i == I->cur;
        uint8_t face = on ? C_SEL : C_FACE;
        uint8_t fg = on ? C_YELLOW : w->unread == 3 ? C_ORANGE : w->unread == 2 ? C_TEXT : w->unread == 1 ? C_CYAN : C_DIM;
        bevel(x, TAB_Y, tw, TAB_H, !on, face);
        char label[24];
        snprintf(label, sizeof label, "%d %.14s", (i + 1) % 100, w->name);
        int lx = textn(x + 6, TAB_Y + 1, label, 2, on ? C_YELLOW : C_DIM, face);
        textn(lx, TAB_Y + 1, label + 2, 14, fg, face);
        if (w->type == WIN_CHAN && !w->joined)
            fill(x + 6, TAB_Y + 9, tw - 12, 1, C_DIM);  // struck through: not in it
        hit(x, TAB_Y, tw, TAB_H, HIT_TAB, i);
        x += tw + 2;
    }
}

/// Draws the topic strip.
static void draw_topic(void) {
    win_t *w = &I->win[I->cur];
    fill(0, TOPIC_Y, W, 18, C_STRIP);
    char buf[TEXT_MAX + 80];
    if (w->type == WIN_CHAN)
        snprintf(buf, sizeof buf, "%s", w->topic[0] ? w->topic : "(no topic)");
    else if (w->type == WIN_QUERY)
        snprintf(buf, sizeof buf, "Private chat with %s", w->name);
    else if (I->state != ST_OFF)
        snprintf(buf, sizeof buf, "%s, port %s%s, as %s", I->host, I->cf[CF_PORT], I->tls ? " (TLS)" : "", I->nick);
    else
        snprintf(buf, sizeof buf, "Not connected: Ctrl-O connects, F1 lists the keys");
    // The topic without its colour codes.
    char plain[100];
    int o = 0;
    for (const char *p = buf; *p && o < 78; ++p) {
        if (*p == 3) {
            while (isdigit((unsigned char)p[1]) || p[1] == ',')
                ++p;
        } else if ((uint8_t)*p >= 0x20) {
            plain[o++] = *p;
        }
    }
    plain[o] = 0;
    text(8, TOPIC_Y + 1, plain, C_TEXT, C_STRIP);
}

/// Draws what is said in the window shown, the newest at the bottom.
static void draw_lines(void) {
    win_t *w = &I->win[I->cur];
    int cols = msg_cols(w);
    fill(4, MSG_Y - 2, cols * 8 + 8, ROWS * 16 + 4, C_BG);
    hit(4, MSG_Y - 2, cols * 8 + 8, ROWS * 16 + 4, HIT_MSGS, 0);
    int left = ROWS, skip = w->scroll;
    cell_t *cells = I->cells;
    for (int i = w->count - 1; i >= 0 && left > 0; --i) {
        const line_t *l = line_at(w, i);
        if (hidden(l))
            continue;
        int16_t st[64], ln[64];
        int ind, n = line_cells(l, cells, CELLS_MAX, &ind);
        int r = wrap(cells, n, cols, ind, st, ln, 64);
        int in = ind > cols / 3 ? 2 : ind;
        for (int j = r - 1; j >= 0 && left > 0; --j) {
            if (skip > 0) {
                --skip;
                continue;
            }
            int y = MSG_Y + (--left) * 16, x = MSG_X + (j ? in * 8 : 0);
            if (l->kind == K_HILITE)
                fill(MSG_X, y, cols * 8, 16, C_HIBG);
            for (int k = 0; k < ln[j]; ++k, x += 8) {
                const cell_t *c = &cells[st[j] + k];
                ch(x, y, c->c, c->fg, c->bg);
                if (c->ul)
                    fill(x, y + 14, 8, 1, c->fg);
            }
        }
    }
    if (w->scroll) {
        const char *more = " More below: PgDn ";
        int mx = MSG_X + cols * 8 - (int)strlen(more) * 8;
        text(mx, MSG_Y + (ROWS - 1) * 16, more, C_BG, C_YELLOW);
    }
}

/// Draws a channel's nick list.
static void draw_nicks(void) {
    win_t *w = &I->win[I->cur];
    int h = ROWS * 16 + 4;
    bevel(NL_X, MSG_Y - 2, W - NL_X - 4, h, false, C_FIELD);
    hit(NL_X, MSG_Y - 2, W - NL_X - 4, h, HIT_NICKS, 0);
    textf(NL_X + 4, MSG_Y, C_LABEL, C_FIELD, "%d people", w->nn);
    int rows = ROWS - 1;
    if (w->ntop > w->nn - rows)
        w->ntop = w->nn - rows;
    if (w->ntop < 0)
        w->ntop = 0;
    for (int r = 0; r < rows && w->ntop + r < w->nn; ++r) {
        int i = w->ntop + r, y = MSG_Y + 16 * (r + 1);
        const nick_t *e = &w->nicks[i];
        bool on = i == w->nsel;
        uint8_t bg = on ? C_SEL : C_FIELD;
        if (on)
            fill(NL_X + 2, y, W - NL_X - 8, 16, bg);
        ch(NL_X + 4, y, e->mode ? (uint8_t)e->mode : ' ', e->mode == '+' ? C_YELLOW : C_GREEN, bg);
        textn(NL_X + 12, y, e->nick, NL_COLS - 1, !strcasecmp(e->nick, I->nick) ? C_YELLOW : C_BODY, bg);
        hit(NL_X + 2, y, W - NL_X - 8, 16, HIT_NICK, i);
    }
    if (w->nn > rows) {  // where the list is
        int sh = rows * 16, th = sh * rows / w->nn, ty = sh * w->ntop / w->nn;
        fill(W - 9, MSG_Y + 16 + ty, 2, th < 8 ? 8 : th, C_LABEL);
    }
}

/// Draws the line being typed.
static void draw_input(void) {
    win_t *w = &I->win[I->cur];
    bevel(4, IN_Y - 2, W - 8, 20, false, C_FIELD);
    char prompt[24];
    snprintf(prompt, sizeof prompt, "[%.12s] ", I->cur ? w->name : "server");
    int px = text(8, IN_Y, prompt, C_LABEL, C_FIELD);
    int cols = (W - 12 - px) / 8;
    if (I->incur < I->inleft)
        I->inleft = I->incur;
    if (I->incur >= I->inleft + cols)
        I->inleft = I->incur - cols + 1;
    for (int i = 0; i < cols && I->inleft + i <= I->inlen; ++i) {
        int at = I->inleft + i;
        uint8_t c = at < I->inlen ? (uint8_t)I->in[at] : ' ';
        bool cur = at == I->incur && !I->dlg && I->menu < 0;
        uint8_t fg = C_TEXT, bg = C_FIELD;
        if (c < 0x20)  // a colour code: its letter, reversed
            c = (uint8_t)(c + '@'), fg = C_BG, bg = C_LABEL;
        if (cur)
            fg = C_BG, bg = C_YELLOW;
        ch(px + i * 8, IN_Y, c, fg, bg);
    }
}

/// Draws the status line: a message, or who we are, where, and the keys.
static void draw_status(void) {
    fill(0, STAT_Y, W, H - STAT_Y, C_STRIP);
    if (I->msg[0] && now_ms() < I->msg_until) {
        textf(8, STAT_Y + 4, C_YELLOW, C_STRIP, "%.78s", I->msg);
        return;
    }
    const char *st = I->state == ST_ON ? "on" : I->state == ST_REG ? "logging in to" : "not connected";
    int x = textf(8, STAT_Y + 4, C_YELLOW, C_STRIP, "%s", I->state == ST_OFF ? I->cf[CF_NICK] : I->nick);
    x = textf(x, STAT_Y + 4, C_LABEL, C_STRIP, " %s %s", st, I->state == ST_OFF ? "" : I->host);
    // Windows with something new.
    for (int i = 0; i < I->nwin; ++i)
        if (I->win[i].unread >= 2 && x < W - 200)
            x = textf(x + 8, STAT_Y + 4, I->win[i].unread == 3 ? C_ORANGE : C_TEXT, C_STRIP, "%d", i + 1);
    text(W - 8 - 23 * 8, STAT_Y + 4, "F1 keys  F10 menus", C_DIM, C_STRIP);
}

/// A menu title's x on the bar.
/// @param i the menu
/// @return its left
static int bar_x(int i) {
    int x = 16;
    for (int j = 0; j < i; ++j)
        x += ((int)strlen(menus[j].title) + 4) * 8;
    return x;
}

static void draw_bar(void) {
    fill(0, 0, W, BAR_H, C_FACE);
    for (int i = 0; i < NMENUS; ++i) {
        int bx = bar_x(i), bw = ((int)strlen(menus[i].title) + 4) * 8;
        bool open = i == I->menu;
        if (open)
            fill(bx, 1, bw, 14, C_SEL);
        text(bx + 16, 0, menus[i].title, open ? C_YELLOW : C_TEXT, open ? C_SEL : C_FACE);
        fill(bx + 16, 14, 8, 1, open ? C_YELLOW : C_LABEL);  // its key letter (Alt and it)
        hit(bx, 0, bw, BAR_H, HIT_MENU, i);
    }
    const char *title = "IRC " VER_IRC;
    text(W - 8 - (int)strlen(title) * 8, 0, title, C_YELLOW, C_FACE);
    fill(0, 0, W, 1, C_LIGHT);
    fill(0, BAR_H - 1, W, 1, C_DARK);
}

// ---- The menus -------------------------------------------------------------------------------------------

/// Whether a menu item can be chosen now.
/// @param it the item
/// @return it can
static bool usable(const hitem_t *it) {
    win_t *w = &I->win[I->cur];
    switch (it->id) {
    case 0: return false;
    case CMD_RECONNECT: return I->cf[CF_SERVER][0] != 0;
    case CMD_DISCONNECT: return I->conn != NULL;
    case CMD_TOPIC: return w->type == WIN_CHAN && I->state == ST_ON;
    case CMD_JOIN: case CMD_QUERY: return I->state == ST_ON;
    case CMD_CLOSE: return I->cur > 0;
    case CMD_NEXT: case CMD_PREV: return I->nwin > 1;
    case CMD_N_OP: case CMD_N_DEOP: case CMD_N_VOICE: case CMD_N_KICK: case CMD_N_WHOIS:
        return I->state == ST_ON;
    }
    return true;
}

/// Whether a menu item shows a tick.
/// @param it the item
/// @return it does
static bool ticked(const hitem_t *it) {
    switch (it->id) {
    case CMD_NICKLIST: return I->show_nicks;
    case CMD_TIMES: return I->show_times;
    case CMD_JOINS: return I->show_joins;
    case CMD_COLOURS: return I->colours;
    case CMD_BEEP: return I->beep;
    }
    return false;
}

/// The items of the menu open.
/// @return them
static const hitem_t *menu_items(void) {
    return I->menu == POP_CHAN ? chan_items : I->menu == POP_NICK ? nick_items : menus[I->menu].items;
}

/// Lays out the menu open.
/// @param x set to its left
/// @param y set to its top
/// @param w set to its width
/// @param h set to its height
/// @param iy set to each item's y (MAX_ITEMS)
/// @param acc set to each item's key letter (its place in the text, -1: none)
/// @return its items
static int menu_layout(int *x, int *y, int *w, int *h, int *iy, int8_t *acc) {
    const hitem_t *items = menu_items();
    int n, tw = 0, kw = 0;
    for (n = 0; items[n].text; ++n) {
        if ((int)strlen(items[n].text) > tw)
            tw = strlen(items[n].text);
        if (items[n].key && (int)strlen(items[n].key) > kw)
            kw = strlen(items[n].key);
    }
    *w = (tw + kw + 6) * 8;
    bool pop = I->menu >= NMENUS;
    *x = pop ? I->popx : bar_x(I->menu);
    *y = pop ? I->popy : BAR_H;
    *h = 6;
    for (int i = 0; i < n; ++i) {
        iy[i] = *y + 3 + *h - 6;
        *h += items[i].text[0] == '-' ? 8 : 18;
    }
    if (*x + *w + SHADOW > W)
        *x = W - *w - SHADOW;
    if (*y + *h + SHADOW > H) {
        int up = H - *h - SHADOW - *y;
        *y += up;
        for (int i = 0; i < n; ++i)
            iy[i] += up;
    }
    // Key letters: the first letter of a word not taken by an item above, else any letter not taken.
    bool used[26] = {false};
    for (int i = 0; i < n; ++i) {
        const char *t = items[i].text;
        acc[i] = -1;
        if (t[0] == '-')
            continue;
        for (int pass = 0; pass < 2; ++pass)
            for (int c = 0; t[c] && acc[i] < 0; ++c) {
                int l = (t[c] | 0x20) - 'a';
                if (l >= 0 && l < 26 && !used[l] && (pass || c == 0 || t[c - 1] == ' '))
                    used[l] = true, acc[i] = (int8_t)c;
            }
    }
    return n;
}

/// Draws the menu open, its shadow and its items (and the places to click on them).
static void draw_menu(void) {
    int x, y, w, h, iy[MAX_ITEMS];
    int8_t acc[MAX_ITEMS];
    int n = menu_layout(&x, &y, &w, &h, iy, acc);
    const hitem_t *items = menu_items();
    hit(0, 0, W, H, HIT_MENU_OFF, 0);  // a click elsewhere closes it
    if (I->menu < NMENUS)
        for (int i = 0; i < NMENUS; ++i)  // moving along the bar opens the others
            hit(bar_x(i), 0, ((int)strlen(menus[i].title) + 4) * 8, BAR_H, HIT_MENU, i);
    fill(x + SHADOW, y + SHADOW, w, h, C_BG);
    bevel(x, y, w, h, true, C_FACE);
    for (int i = 0; i < n; ++i) {
        const hitem_t *it = &items[i];
        if (it->text[0] == '-') {
            fill(x + 6, iy[i] + 3, w - 12, 1, C_DARK);
            fill(x + 6, iy[i] + 4, w - 12, 1, C_LIGHT);
            continue;
        }
        bool on = i == I->msel, off = !usable(it);
        uint8_t bg = on ? C_SEL : C_FACE, fg = off ? C_DIM : on ? C_TEXT : C_LABEL;
        int ty = iy[i] + 1;
        fill(x + 3, iy[i], w - 6, 18, bg);
        if (ticked(it))
            ch(x + 6, ty, 0xfb, on ? C_YELLOW : C_GREEN, bg);
        text(x + 22, ty, it->text, fg, bg);
        if (acc[i] >= 0 && !off)
            fill(x + 22 + acc[i] * 8, ty + 14, 8, 1, on ? C_YELLOW : C_DIM);
        if (it->key && it->key[0])
            text(x + w - 8 - (int)strlen(it->key) * 8, ty, it->key, off ? C_DIM : on ? C_YELLOW : C_DIM, bg);
        hit(x + 3, iy[i], w - 6, 18, HIT_ITEM, i);
    }
}

/// Opens a menu.
/// @param m the menu (or POP_CHAN, POP_NICK at popx, popy)
/// @param armed opened by a button that is still down
static void menu_open(int m, bool armed) {
    I->menu = m;
    I->msel = -1;
    I->marmed = armed;
    I->dirty = true;
}

/// Closes the menu.
static void menu_close(void) {
    I->menu = -1;
    I->dirty = true;
}

/// Runs a menu item.
/// @param i the item
static void item_run(int i) {
    const hitem_t *it = &menu_items()[i];
    if (!usable(it))
        return;
    menu_close();
    command(it->id);
}

/// A key while a menu is open: Up and Down choose, Left and Right go along the bar, Enter or an item's
/// letter runs it, Esc closes it.
/// @param k the key
static void menu_key(int k) {
    int x, y, w, h, iy[MAX_ITEMS];
    int8_t acc[MAX_ITEMS];
    int n = menu_layout(&x, &y, &w, &h, iy, acc);
    const hitem_t *items = menu_items();
    I->dirty = true;
    if (k == KEY_ESC || k == KEY_F9 || k == KEY_F10 || k == CTRL('c')) {
        menu_close();
    } else if (k == KEY_UP || k == KEY_DOWN) {
        int d = k == KEY_UP ? -1 : 1, s = I->msel < 0 ? (d > 0 ? -1 : 0) : I->msel;
        for (int t = 0; t < n; ++t) {
            s = (s + d + n) % n;
            if (usable(&items[s]))
                break;
        }
        I->msel = s;
    } else if ((k == KEY_LEFT || k == KEY_RIGHT) && I->menu < NMENUS) {
        menu_open((I->menu + (k == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS, false);
    } else if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
        for (int i = 0; i < NMENUS; ++i)
            if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                menu_open(i, false);
    } else if (k == '\r' || k == '\n') {
        if (I->msel >= 0)
            item_run(I->msel);
    } else if (k >= 0x20 && k < 0x7f) {
        for (int i = 0; i < n; ++i)
            if (acc[i] >= 0 && (items[i].text[acc[i]] | 0x20) == (k | 0x20) && usable(&items[i])) {
                item_run(i);
                break;
            }
    }
}

// ---- The boxes ---------------------------------------------------------------------------------------

/// A box's background, which swallows clicks around its buttons.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param title its title
static void box(int x, int y, int w, int h, const char *title) {
    bevel(x - 2, y - 2, w + 4, h + 4, true, C_LIGHT);
    bevel(x, y, w, h, true, C_FACE);
    hit(0, 0, W, H, HIT_SWALLOW, 0);
    text(x + 16, y + 10, title, C_YELLOW, C_FACE);
}

/// A field to type in, with a cursor at its end when it is the one being typed in.
/// @param x left
/// @param y top
/// @param cols its width in characters
/// @param s its text
/// @param on being typed in
/// @param hidden shown as stars (a password)
static void field(int x, int y, int cols, const char *s, bool on, bool hidden) {
    bevel(x, y, cols * 8 + 8, 20, false, C_FIELD);
    int n = (int)strlen(s), from = n >= cols ? n - cols + 1 : 0;
    int cx = x + 4;
    for (int i = from; i < n; ++i, cx += 8)
        ch(cx, y + 2, hidden ? '*' : (uint8_t)s[i], C_TEXT, C_FIELD);
    if (on)
        ch(cx, y + 2, ' ', C_BG, C_YELLOW);
}

/// Draws the question about quitting.
static void draw_ask(void) {
    int w = 400, h = 96, x = (W - w) / 2, y = 170;
    box(x, y, w, h, "Leave IRC? (you will be disconnected)");
    button(x + 16, y + 52, 120, 26, "Quit", true, HIT_ASK, 1);
    button(x + 144, y + 52, 120, 26, "Stay", false, HIT_ASK, 0);
}

/// Draws the connect box.
static void draw_connect(void) {
    static const char *const labels[CF_N] = {"Server", "Port", "Nick", "Channels", "Password"};
    int w = 500, h = 236, x = (W - w) / 2, y = 100;
    box(x, y, w, h, "Connect to an IRC server");
    for (int i = 0; i < CF_N; ++i) {
        int fy = y + 36 + i * 26;
        text(x + 16, fy + 2, labels[i], I->field == i ? C_YELLOW : C_LABEL, C_FACE);
        field(x + 112, fy, 44, I->cf[i], I->field == i, i == CF_PASS);
        hit(x + 112, fy, 44 * 8 + 8, 20, HIT_FIELD, i);
    }
    int ty = y + 36 + CF_N * 26;
    bevel(x + 112, ty + 2, 14, 14, false, C_FIELD);
    if (I->use_tls)
        ch(x + 115, ty, 0xfb, C_GREEN, C_FIELD);
    text(x + 134, ty, "TLS (encrypted; port 6697)", I->field == CF_N ? C_YELLOW : C_LABEL, C_FACE);
    hit(x + 112, ty, 260, 18, HIT_TLS, 0);
    text(x + 16, ty + 18, "Channels: #a,#b   Password: the server's (often none)", C_DIM, C_FACE);
    button(x + w - 272, y + h - 36, 120, 26, "Connect", true, HIT_OK, 0);
    button(x + w - 136, y + h - 36, 120, 26, "Cancel", false, HIT_CANCEL, 0);
}

/// Draws the one-line box (join, private chat, topic, nick).
static void draw_input_box(void) {
    int w = 500, h = 110, x = (W - w) / 2, y = 160;
    box(x, y, w, h, I->in_title);
    field(x + 16, y + 38, 57, I->dl, true, false);
    button(x + w - 272, y + h - 36, 120, 26, "OK", true, HIT_OK, 0);
    button(x + w - 136, y + h - 36, 120, 26, "Cancel", false, HIT_CANCEL, 0);
}

/// Draws a box of text lines.
/// @param title its title
/// @param lines the lines
/// @param n how many
static void draw_text_box(const char *title, const char *const *lines, int n) {
    int w = 600, h = 56 + n * 16, x = (W - w) / 2, y = (H - h) / 2;
    box(x, y, w, h, title);
    for (int i = 0; i < n; ++i)
        text(x + 16, y + 32 + i * 16, lines[i], lines[i][0] == ' ' ? C_TEXT : C_LABEL, C_FACE);
    text(x + 16, y + h - 20, "(any key or click closes this)", C_DIM, C_FACE);
}

/// The keys.
static const char *const help_lines[] = {
    "Typing",
    "  Enter says the line (or runs a /command: Help > Commands)",
    "  Up and Down bring back lines typed; Tab finishes a nick",
    "  Ctrl-B bold, Ctrl-K colour (alone: plain), Ctrl-U underline",
    "Windows",
    "  Ctrl-N or F3 the next, Ctrl-P or F2 the one before, or click a tab",
    "  Ctrl-W leaves and closes one, Ctrl-L clears it, F4 the nick list",
    "  PgUp and PgDn (or the wheel) scroll back through what was said",
    "  Tabs: white for talk, orange when you are named, cyan for",
    "  comings and goings",
    "  Nicks: click one, double-click to chat, right-click for more",
    "The rest",
    "  Ctrl-O connect, Ctrl-R reconnect, Esc quits",
    "  F10 or Alt and a menu's letter open the menus",
    "  The settings are kept in /IRC.CFG",
};

/// The commands.
static const char *const cmd_lines[] = {
    "  /join #channel [key]     /part [#channel] [why]    /close",
    "  /msg nick text           /query nick [text]        /me does something",
    "  /nick newnick            /topic [new topic]        /names [#channel]",
    "  /whois nick              /away [why]               /notice nick text",
    "  /op /deop /voice /devoice nick     /kick nick [why]",
    "  /server host [+port]     /disconnect [why]         /quit [why]",
    "  /ctcp nick VERSION       /raw LINE (as it is)      /clear",
    "Others (/list, /mode, /invite, /who ...) go to the server as they are.",
    "Start a line with // to say something that starts with /.",
    "NickServ: /msg NickServ IDENTIFY password",
};

/// Draws the About box.
static void draw_about(void) {
    char l0[80], l1[80], l2[80];
    snprintf(l0, sizeof l0, "  pico-os IRC " VER_IRC ", for the Raspberry Pi Pico 2 W");
    int lines = 0, nicks = 0;
    for (int i = 0; i < I->nwin; ++i)
        lines += I->win[i].count, nicks += I->win[i].nn;
    snprintf(l1, sizeof l1, "  %d windows, %d lines kept, %d nicks", I->nwin, lines, nicks);
    snprintf(l2, sizeof l2, "  PSRAM free: %lu KB", (unsigned long)(psram_free_total() / 1024));
    const char *const about[] = {l0, "  Copyright (c) 2026 Ken Segler, MIT licence", l1, l2,
                                 "  Each window keeps its last 256 lines."};
    draw_text_box("About", about, 5);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    I->drawing = true;
    I->t = I->g ? I->g : dvi_framebuf;
    I->collect = true;
    I->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_bar();
    draw_tabs();
    draw_topic();
    draw_lines();
    if (I->show_nicks && I->win[I->cur].type == WIN_CHAN)
        draw_nicks();
    draw_input();
    draw_status();
    switch (I->dlg) {
    case DLG_ASK: draw_ask(); break;
    case DLG_CONNECT: draw_connect(); break;
    case DLG_INPUT: draw_input_box(); break;
    case DLG_HELP: draw_text_box("Keys", help_lines, sizeof help_lines / sizeof *help_lines); break;
    case DLG_CMDS: draw_text_box("Commands", cmd_lines, sizeof cmd_lines / sizeof *cmd_lines); break;
    case DLG_ABOUT: draw_about(); break;
    }
    if (I->menu >= 0)
        draw_menu();
    I->collect = false;
    I->t = dvi_framebuf;
    I->drawing = false;
}

/// Copies the picture onto the screen.
static void show(void) {
    if (I->g)
        memcpy(dvi_framebuf, I->g, W * H);
}

// ---- Commands, keys and the mouse ------------------------------------------------------------------------

/// Opens the one-line box.
/// @param cmd what it is for (CMD_*)
/// @param title its title
/// @param start what is in it at first
static void input_open(int cmd, const char *title, const char *start) {
    I->dlg = DLG_INPUT;
    I->in_cmd = cmd;
    copy(I->in_title, sizeof I->in_title, title);
    copy(I->dl, sizeof I->dl, start);
    I->dirty = true;
}

/// The one-line box's OK.
static void input_ok(void) {
    I->dlg = DLG_NONE;
    I->dirty = true;
    char line[200];
    const char *v = I->dl;
    while (*v == ' ')
        ++v;
    if (!*v && I->in_cmd != CMD_TOPIC)
        return;
    switch (I->in_cmd) {
    case CMD_JOIN: snprintf(line, sizeof line, "/join %s", v); break;
    case CMD_QUERY: snprintf(line, sizeof line, "/query %s", v); break;
    case CMD_TOPIC: snprintf(line, sizeof line, "/topic %s", v); break;
    case CMD_NICK: snprintf(line, sizeof line, "/nick %s", v); break;
    case CMD_N_KICK: snprintf(line, sizeof line, "/kick %s", v); break;
    default: return;
    }
    typed(line);
}

/// The connect box's Connect.
static void connect_ok(void) {
    I->dlg = DLG_NONE;
    I->dirty = true;
    cfg_save();
    connect_now();
}

/// The nick chosen in the list.
/// @return it, or NULL
static const char *chosen_nick(void) {
    win_t *w = &I->win[I->cur];
    return w->type == WIN_CHAN && w->nsel >= 0 && w->nsel < w->nn ? w->nicks[w->nsel].nick : NULL;
}

static void command(int id) {
    win_t *w = &I->win[I->cur];
    const char *nick = chosen_nick();
    char line[100];
    I->dirty = true;
    switch (id) {
    case CMD_CONNECT: I->dlg = DLG_CONNECT; I->field = CF_SERVER; break;
    case CMD_RECONNECT: connect_now(); break;
    case CMD_DISCONNECT: disconnect(NULL); break;
    case CMD_QUIT:
        if (I->conn)
            I->dlg = DLG_ASK;
        else
            I->quit = true;
        break;
    case CMD_JOIN: input_open(CMD_JOIN, "Join a channel (#name)", "#"); break;
    case CMD_QUERY: input_open(CMD_QUERY, "Private chat with (nick)", nick ? nick : ""); break;
    case CMD_TOPIC: input_open(CMD_TOPIC, "The channel's topic", w->topic); break;
    case CMD_NICK: input_open(CMD_NICK, "Your new nick", I->state == ST_OFF ? I->cf[CF_NICK] : I->nick); break;
    case CMD_NEXT: win_show((I->cur + 1) % I->nwin); break;
    case CMD_PREV: win_show((I->cur + I->nwin - 1) % I->nwin); break;
    case CMD_CLOSE:
        if (I->cur == 0) {
            message("The server's window stays open");
        } else {
            if (w->type == WIN_CHAN && w->joined)
                send_line("PART %s :%s", w->name, QUIT_MSG);
            win_close(I->cur);
            win_show(I->cur);
        }
        break;
    case CMD_CLEAR: w->count = w->head = w->scroll = 0; break;
    case CMD_NICKLIST: I->show_nicks = !I->show_nicks; w->scroll = 0; cfg_save(); break;
    case CMD_TIMES: I->show_times = !I->show_times; w->scroll = 0; cfg_save(); break;
    case CMD_JOINS: I->show_joins = !I->show_joins; w->scroll = 0; cfg_save(); break;
    case CMD_COLOURS: I->colours = !I->colours; cfg_save(); break;
    case CMD_BEEP: I->beep = !I->beep; cfg_save(); break;
    case CMD_HELP: I->dlg = DLG_HELP; break;
    case CMD_CMDS: I->dlg = DLG_CMDS; break;
    case CMD_ABOUT: I->dlg = DLG_ABOUT; break;
    case CMD_N_QUERY: if (nick) query(nick); break;
    case CMD_N_WHOIS: if (nick) send_line("WHOIS %s", nick); break;
    case CMD_N_OP: case CMD_N_DEOP: case CMD_N_VOICE:
        if (nick) {
            snprintf(line, sizeof line, "/%s %s", id == CMD_N_OP ? "op" : id == CMD_N_DEOP ? "deop" : "voice", nick);
            typed(line);
        }
        break;
    case CMD_N_KICK:
        if (nick) {
            snprintf(line, sizeof line, "%s ", nick);
            input_open(CMD_N_KICK, "Kick (nick and why)", line);
        }
        break;
    }
}

/// Typing in a box's field: letters added, Backspace takes one off.
/// @param s the field
/// @param size its size
/// @param k the key
/// @return the key was taken
static bool field_key(char *s, size_t size, int k) {
    size_t n = strlen(s);
    if ((k == 8 || k == 0x7f) && n) {
        s[n - 1] = 0;
        return true;
    }
    if (k == CTRL('u')) {
        s[0] = 0;
        return true;
    }
    if (k >= 0x20 && k < 0x7f && n + 1 < size) {
        s[n] = (char)k, s[n + 1] = 0;
        return true;
    }
    return false;
}

/// Puts a character into the line being typed.
/// @param c it
static void in_put(char c) {
    if (I->inlen >= IN_MAX)
        return;
    memmove(I->in + I->incur + 1, I->in + I->incur, I->inlen - I->incur);
    I->in[I->incur++] = c;
    I->in[++I->inlen] = 0;
}

/// Sets the line being typed.
/// @param s it
static void in_set(const char *s) {
    copy(I->in, sizeof I->in, s);
    I->inlen = I->incur = (int)strlen(I->in);
}

/// Tab: finishes the nick being typed from the channel's list (again: the next that fits).
static void complete(void) {
    win_t *w = &I->win[I->cur];
    if (!I->tab_on) {
        int s = I->incur;
        while (s > 0 && I->in[s - 1] != ' ')
            --s;
        int n = I->incur - s;
        if (n <= 0 || n >= NICK_MAX)
            return;
        memcpy(I->tab_word, I->in + s, n);
        I->tab_word[n] = 0;
        I->tab_at = s;
        I->tab_n = -1;
    }
    // The candidates: the channel's nicks, or the chat's other person.
    int count = w->type == WIN_CHAN ? w->nn : w->type == WIN_QUERY ? 1 : 0;
    size_t wl = strlen(I->tab_word);
    for (int t = 1; t <= count; ++t) {
        int i = (I->tab_n + t) % count;
        const char *cand = w->type == WIN_CHAN ? w->nicks[i].nick : w->name;
        if (!strncasecmp(cand, I->tab_word, wl) && strcasecmp(cand, I->nick)) {
            // Replace from tab_at to the cursor.
            char rest[IN_MAX + 1];
            copy(rest, sizeof rest, I->in + I->incur);
            char done[IN_MAX + 1];
            snprintf(done, sizeof done, "%.*s%s%s", I->tab_at, I->in, cand, I->tab_at == 0 ? ": " : " ");
            int cur = (int)strlen(done);
            char all[IN_MAX * 2 + 2];
            snprintf(all, sizeof all, "%s%s", done, rest);
            in_set(all);
            I->incur = cur > I->inlen ? I->inlen : cur;
            I->tab_n = i;
            I->tab_on = true;
            return;
        }
    }
}

/// Enter: the line typed is said (or run), and kept for Up.
static void enter(void) {
    if (!I->inlen)
        return;
    char line[IN_MAX + 1];
    copy(line, sizeof line, I->in);
    if (!I->nhist || strcmp(I->hist[I->nhist - 1], line)) {
        if (I->nhist == NHIST)
            memmove(I->hist, I->hist + 1, (NHIST - 1) * sizeof *I->hist), --I->nhist;
        copy(I->hist[I->nhist++], sizeof *I->hist, line);
    }
    I->hpos = I->nhist;
    in_set("");
    I->win[I->cur].scroll = 0;
    typed(line);
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    I->dirty = true;
    if (I->menu >= 0) {
        menu_key(k);
        return;
    }
    switch (I->dlg) {
    case DLG_HELP: case DLG_CMDS: case DLG_ABOUT: I->dlg = DLG_NONE; return;
    case DLG_ASK:
        if (k == '\r' || k == '\n' || k == 'q' || k == 'Q') {
            I->dlg = DLG_NONE;
            disconnect(NULL);
            I->quit = true;
        } else if (k == KEY_ESC || k == 's' || k == 'S') {
            I->dlg = DLG_NONE;
        }
        return;
    case DLG_INPUT:
        if (k == '\r' || k == '\n')
            input_ok();
        else if (k == KEY_ESC)
            I->dlg = DLG_NONE;
        else
            field_key(I->dl, sizeof I->dl, k);
        return;
    case DLG_CONNECT:
        if (k == '\r' || k == '\n') {
            connect_ok();
        } else if (k == KEY_ESC) {
            I->dlg = DLG_NONE;
        } else if (k == '\t' || k == KEY_DOWN) {
            I->field = (I->field + 1) % (CF_N + 1);
        } else if (k == KEY_UP) {
            I->field = (I->field + CF_N) % (CF_N + 1);
        } else if (I->field == CF_N) {
            if (k == ' ')
                I->use_tls = !I->use_tls;
        } else {
            field_key(I->cf[I->field], sizeof I->cf[0], k);
            if (I->field == CF_PORT && !strcmp(I->cf[CF_PORT], "6697"))
                I->use_tls = true;
        }
        return;
    }
    if (k != '\t')
        I->tab_on = false;
    switch (k) {
    case '\r': case '\n': enter(); break;
    case '\t': complete(); break;
    case 8: case 0x7f:
        if (I->incur > 0) {
            memmove(I->in + I->incur - 1, I->in + I->incur, I->inlen - I->incur + 1);
            --I->incur, --I->inlen;
        }
        break;
    case KEY_DEL:
        if (I->incur < I->inlen) {
            memmove(I->in + I->incur, I->in + I->incur + 1, I->inlen - I->incur);
            --I->inlen;
        }
        break;
    case KEY_LEFT: if (I->incur > 0) --I->incur; break;
    case KEY_RIGHT: if (I->incur < I->inlen) ++I->incur; break;
    case KEY_HOME: I->incur = 0; break;
    case KEY_END: I->incur = I->inlen; break;
    case KEY_UP:
        if (I->hpos > 0)
            in_set(I->hist[--I->hpos]);
        break;
    case KEY_DOWN:
        if (I->hpos < I->nhist)
            ++I->hpos, in_set(I->hpos < I->nhist ? I->hist[I->hpos] : "");
        break;
    case KEY_PGUP: scroll(ROWS - 2); break;
    case KEY_PGDN: scroll(-(ROWS - 2)); break;
    case CTRL('n'): case KEY_F3: command(CMD_NEXT); break;
    case CTRL('p'): case KEY_F2: command(CMD_PREV); break;
    case CTRL('w'): command(CMD_CLOSE); break;
    case CTRL('l'): command(CMD_CLEAR); break;
    case CTRL('o'): command(CMD_CONNECT); break;
    case CTRL('r'): command(CMD_RECONNECT); break;
    case CTRL('b'): in_put(2); break;
    case CTRL('k'): in_put(3); break;
    case CTRL('u'): in_put(0x1f); break;
    case CTRL('v'): in_put(0x16); break;
    case KEY_F1: command(CMD_HELP); break;
    case KEY_F4: command(CMD_NICKLIST); break;
    case KEY_F9: case KEY_F10: menu_open(0, false); break;
    case KEY_ESC: command(CMD_QUIT); break;
    default:
        if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
            for (int i = 0; i < NMENUS; ++i)
                if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                    menu_open(i, false);
        } else if (k >= KEY_ALT + '1' && k <= KEY_ALT + '9') {
            win_show(k - KEY_ALT - '1');
        } else if (k >= 0x20 && k < 0x7f) {
            in_put((char)k);
        }
        break;
    }
}

/// The mouse while a menu is open: moving chooses (and along the bar opens the others), a click on an item
/// (or letting go on one, of the button that opened it) runs it, a click elsewhere closes it.
/// @param e the event
static void menu_mouse(const mouse_event_t *e) {
    const hit_t *h = hit_at(e->x, e->y);
    int id = h ? h->id : 0;
    if (e->type == MOUSE_WHEEL) {
        menu_key(e->wheel > 0 ? KEY_UP : KEY_DOWN);
        return;
    }
    if (id == HIT_MENU && h->arg != I->menu && e->type != MOUSE_UP) {
        menu_open(h->arg, I->marmed || e->type == MOUSE_DOWN);
        return;
    }
    if (id == HIT_ITEM) {
        if (I->msel != h->arg)
            I->msel = h->arg, I->dirty = true;
        if (e->type == MOUSE_DOWN || (e->type == MOUSE_UP && I->marmed)) {
            item_run(h->arg);
            return;
        }
    } else if (I->msel >= 0 && e->type == MOUSE_MOVE) {
        I->msel = -1;
        I->dirty = true;
    }
    if (e->type == MOUSE_DOWN && id != HIT_ITEM)
        menu_close();
    if (e->type == MOUSE_UP)
        I->marmed = false;
}

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    I->mx = e->x, I->my = e->y;
    if (I->menu >= 0) {
        menu_mouse(e);
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    win_t *w = &I->win[I->cur];
    if (e->type == MOUSE_WHEEL) {
        if (I->dlg)
            return;
        if (h && (h->id == HIT_NICKS || h->id == HIT_NICK))
            w->ntop -= e->wheel * 3;
        else
            scroll(e->wheel * 3);
        I->dirty = true;
        return;
    }
    if (e->type != MOUSE_DOWN || !h)
        return;
    I->dirty = true;
    if (e->button == MOUSE_RIGHT && !I->dlg) {
        I->popx = e->x, I->popy = e->y;
        if (h->id == HIT_NICK) {
            w->nsel = h->arg;
            menu_open(POP_NICK, true);
        } else if (h->id == HIT_MSGS || h->id == HIT_TAB) {
            if (h->id == HIT_TAB)
                win_show(h->arg);
            menu_open(POP_CHAN, true);
        }
        return;
    }
    switch (h->id) {
    case HIT_MENU: menu_open(h->arg, true); break;
    case HIT_TAB: win_show(h->arg); break;
    case HIT_NICK:
        w->nsel = h->arg;
        if (e->double_click)
            command(CMD_N_QUERY);
        break;
    case HIT_ASK:
        I->dlg = DLG_NONE;
        if (h->arg) {
            disconnect(NULL);
            I->quit = true;
        }
        break;
    case HIT_FIELD: I->field = h->arg; break;
    case HIT_TLS: I->use_tls = !I->use_tls; I->field = CF_N; break;
    case HIT_OK:
        if (I->dlg == DLG_CONNECT)
            connect_ok();
        else
            input_ok();
        break;
    case HIT_CANCEL: I->dlg = DLG_NONE; break;
    case HIT_SWALLOW:
        if (I->dlg == DLG_HELP || I->dlg == DLG_CMDS || I->dlg == DLG_ABOUT)
            I->dlg = DLG_NONE;
        break;
    }
}

// ---- Printing goes to the server's window ----------------------------------------------------------------

#ifndef IRC_PC_TEST
/// stdio's output while the client runs: each line printed (by the network code) goes to the server's
/// window, drawn at once (connecting waits, so the loop wouldn't draw it).
/// @param buf the characters
/// @param len how many
static void cap_out(const char *buf, int len) {
    for (int i = 0; i < len; ++i) {
        char c = buf[i];
        if (I->cap_esc) {  // escape sequences are dropped
            I->cap_esc = !isalpha((unsigned char)c);
            continue;
        }
        if (c == 0x1b) {
            I->cap_esc = true;
        } else if (c == '\n' || c == '\r') {
            if (I->caplen) {
                I->cap[I->caplen] = 0;
                I->caplen = 0;
                add(0, K_INFO, NULL, "%s", I->cap);
                if (!I->drawing) {
                    draw();
                    show();
                    I->dirty = false;
                }
            }
        } else if ((uint8_t)c >= ' ' && I->caplen < (int)sizeof I->cap - 1) {
            I->cap[I->caplen++] = c;
        }
    }
}
#endif

// ---- The client ------------------------------------------------------------------------------------------

/// Starts the state (PSRAM).
/// @return false if there is no memory for it
static bool start(void) {
    I = psram_malloc(sizeof *I);
    if (!I)
        return false;
    memset(I, 0, sizeof *I);
    I->menu = -1;
    I->t = dvi_framebuf;
    I->show_nicks = I->show_times = I->show_joins = I->colours = I->beep = true;
    I->nwin = 1;
    I->win[0].type = WIN_SERVER;
    I->win[0].nsel = -1;
    copy(I->win[0].name, sizeof I->win[0].name, "server");
    cfg_load();
    return true;
}

/// Frees the state.
static void stop(void) {
    hang_up();
    for (int i = 0; i < I->nwin; ++i) {
        psram_free(I->win[i].lines);
        psram_free(I->win[i].nicks);
    }
    psram_free(I->g);
    psram_free(I);
    I = NULL;
}

/// The client's loop.
static void run(void) {
    I->g = psram_malloc(W * H);  // drawn into and copied out at once
    scr_begin();
#ifndef IRC_PC_TEST
    I->cap_driver.out_chars = cap_out;
    stdio_set_driver_enabled(&console_stdio_driver, false);
    stdio_set_driver_enabled(&I->cap_driver, true);
#endif
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    draw();
    show();
    if (I->dlg == DLG_NONE && I->cf[CF_SERVER][0] && I->state == ST_OFF && I->win[0].count == 0)
        connect_now();
    bool msg_shown = false;
    uint32_t drawn = 0;
    while (!I->quit) {
        int k;
        while (!I->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!I->quit && mouse_event(&e))
            mouse(&e);
        poll_server();
        mouse_pointer_update();
        bool msg_on = I->msg[0] && now_ms() < I->msg_until;
        if (msg_on != msg_shown)
            msg_shown = msg_on, I->dirty = true;
        if (I->dirty && !I->quit && now_ms() - drawn >= 20) {
            I->dirty = false;
            draw();
            show();
            drawn = now_ms();
        }
        sleep_ms(5);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
#ifndef IRC_PC_TEST
    stdio_set_driver_enabled(&I->cap_driver, false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
#endif
    scr_end();
}

/// The command line: server [port|+port] [nick] [#channel...], kept in /IRC.CFG.
/// @param argc the argument count
/// @param argv the arguments
static void args(int argc, char **argv) {
    copy(I->cf[CF_SERVER], sizeof I->cf[0], argv[1]);
    int a = 2;
    if (a < argc && (isdigit((unsigned char)argv[a][0]) || argv[a][0] == '+')) {
        I->use_tls = argv[a][0] == '+' || !strcmp(argv[a], "6697");
        copy(I->cf[CF_PORT], sizeof I->cf[0], argv[a] + (argv[a][0] == '+'));
        ++a;
    }
    if (a < argc && !is_chan(argv[a]))
        copy(I->cf[CF_NICK], sizeof I->cf[0], argv[a++]);
    if (a < argc) {
        I->cf[CF_CHANS][0] = 0;
        for (; a < argc; ++a) {
            size_t n = strlen(I->cf[CF_CHANS]);
            snprintf(I->cf[CF_CHANS] + n, sizeof I->cf[0] - n, "%s%s", n ? "," : "", argv[a]);
        }
    }
    cfg_save();
}

void irc_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        printf("usage: irc [server [port|+port] [nick] [#channel...]]   F1 in it lists its keys\n");
        return;
    }
    if (!start()) {
        printf("irc: not enough memory\n");
        return;
    }
    if (argc > 1)
        args(argc, argv);
    else
        I->dlg = DLG_CONNECT;  // filled in from /IRC.CFG: Enter connects
    run();
    stop();
}
