/**
 * @file telnet_menu.c
 * @brief telnet's directory: `telnet` alone (or `telnet -m [file]`) opens a full-screen list of BBSes from a dialing directory (EtherTerm's dialdirectory.xml, see dialdir.h), drawn with mc's screen (mc.h).
 *
 * Enter connects, with telnet or, for SSH entries, ssh; when the session
 * ends the list comes back. Favourites and the last BBS called are kept in
 * their own small file, so the directory itself is never written to.
 *
 * Everything is in PSRAM, and only while the list is open.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a column on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "dialdir.h"
#include "ff.h"
#include "mc.h"
#include "mouse.h"
#include "net.h"
#include "psram_heap.h"

#define DIR_FILE   "/TELNET/DIALDIR.XML"                ///< on the drive
#define SHARE_FILE "/NET/telnet_list/dialdirectory.xml"  ///< or on the PC's share
#define FAV_FILE   "/TELNET/FAVS.TXT"
#define FILE_MAX   (4u << 20)

/// The screen: the title bar, a framed list (column titles, a line, the
/// rows), the key bar.
#define LIST_TOP  4
#define LIST_ROWS (SCR_H - 2 - LIST_TOP)
#define COL_NAME  4
#define COL_HOST  39
#define COL_PORT  70
#define COL_VIA   76
static const int seps[] = {COL_HOST - 1, COL_PORT - 1, COL_VIA - 1};

#define C_SSH     A(LCYAN, BLUE)
#define C_SSH_SEL A(BLUE, CYAN)

typedef struct {
    dial_entry_t *e;  ///< the directory
    ///@{
    int n, nssh;
    ///@}
    int *shown;       ///< the entries the filter lets through
    int nshown;
    /// in shown
    ///@{
    int cur, top;
    ///@}
    char filter[32];
    ///@{
    bool favs_only, quit;
    ///@}
    char source[64];
    char last[64];    ///< "host:port" of the last BBS called
    char note[64];    ///< said on the bottom line until the next key
} dirmenu_t;

static dirmenu_t *M;  ///< PSRAM, while open

// ---- The files ------------------------------------------------------------------------------------

/// A whole file into PSRAM, 0-terminated; NULL if it can't be read.
static char *read_file(const char *path, UINT *len) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    FSIZE_t size = f_size(&f);
    char *buf = size < FILE_MAX ? psram_malloc((size_t)size + 1) : NULL;
    UINT got = 0;
    if (buf && (f_read(&f, buf, (UINT)size, &got) != FR_OK || got != size)) {
        psram_free(buf);
        buf = NULL;
    }
    f_close(&f);
    if (buf) {
        buf[got] = 0;
        *len = got;
    }
    return buf;
}

/// The directory: the file given, else the drive's copy, else the share's
/// (then copied to the drive, so it's there without the share).
static bool load_directory(const char *path) {
    UINT len = 0;
    char *text = NULL;
    if (path) {
        text = read_file(path, &len);
    } else if ((text = read_file(DIR_FILE, &len)) != NULL) {
        path = DIR_FILE;
    } else if ((text = read_file(SHARE_FILE, &len)) != NULL) {
        path = SHARE_FILE;
        FIL f;
        UINT put;
        f_mkdir("/TELNET");
        if (f_open(&f, DIR_FILE, FA_CREATE_NEW | FA_WRITE) == FR_OK) {
            bool ok = f_write(&f, text, len, &put) == FR_OK && put == len;
            f_close(&f);
            if (!ok)
                f_unlink(DIR_FILE);
        }
    }
    if (!text) {
        printf("telnet: can't read %s\n", path ? path : DIR_FILE " (or " SHARE_FILE ")");
        return false;
    }
    int n = dialdir_parse(text, NULL, 0, NULL);
    M->e = n ? psram_malloc(n * sizeof *M->e) : NULL;
    M->shown = n ? psram_malloc(n * sizeof *M->shown) : NULL;
    if (!n || !M->e || !M->shown) {
        psram_free(text);
        printf(n ? "telnet: not enough memory\n" : "telnet: no BBSes in %s\n", path);
        return false;
    }
    M->n = dialdir_parse(text, M->e, n, NULL);
    psram_free(text);
    for (int i = 0; i < M->n; ++i)
        M->nssh += M->e[i].ssh;
    snprintf(M->source, sizeof M->source, "%s", path);
    return true;
}

static void key_of(const dial_entry_t *e, char *out, size_t size) {
    snprintf(out, size, "%s:%u", e->host, e->port);
}

/// FAVS.TXT: a "host:port" line for each favourite, and "last=host:port".
static void load_favs(void) {
    UINT len;
    char *text = read_file(FAV_FILE, &len);
    if (!text)
        return;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "last=", 5)) {
            snprintf(M->last, sizeof M->last, "%s", line + 5);
            continue;
        }
        char k[64];
        for (int i = 0; i < M->n; ++i) {
            key_of(&M->e[i], k, sizeof k);
            if (!strcasecmp(k, line))
                M->e[i].fav = 1;
        }
    }
    psram_free(text);
}

static void save_favs(void) {
    FIL f;
    f_mkdir("/TELNET");
    if (f_open(&f, FAV_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        snprintf(M->note, sizeof M->note, "Can't save " FAV_FILE);
        return;
    }
    char k[64];
    for (int i = 0; i < M->n; ++i)
        if (M->e[i].fav) {
            key_of(&M->e[i], k, sizeof k);
            f_printf(&f, "%s\r\n", k);
        }
    if (M->last[0])
        f_printf(&f, "last=%s\r\n", M->last);
    f_close(&f);
}

// ---- The list -------------------------------------------------------------------------------------

static bool contains(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; ++s)
        if (!strncasecmp(s, what, n))
            return true;
    return !n;
}

/// What's shown after the filter and the favourites switch, keeping the
/// cursor on the same BBS if it's still there.
static void apply_filter(void) {
    int was = M->nshown ? M->shown[M->cur] : -1;
    M->nshown = 0;
    M->cur = 0;
    for (int i = 0; i < M->n; ++i) {
        const dial_entry_t *e = &M->e[i];
        if (M->favs_only && !e->fav)
            continue;
        if (!contains(e->name, M->filter) && !contains(e->host, M->filter))
            continue;
        if (i == was)
            M->cur = M->nshown;
        M->shown[M->nshown++] = i;
    }
}

static void fix_scroll(void) {
    if (M->cur >= M->nshown)
        M->cur = M->nshown - 1;
    if (M->cur < 0)
        M->cur = 0;
    if (M->top > M->cur)
        M->top = M->cur;
    if (M->cur >= M->top + LIST_ROWS)
        M->top = M->cur - LIST_ROWS + 1;
    if (M->top > M->nshown - LIST_ROWS)
        M->top = M->nshown - LIST_ROWS;
    if (M->top < 0)
        M->top = 0;
}

// ---- Drawing --------------------------------------------------------------------------------------

static const struct {
    ///@{
    const char *key, *text;
    ///@}
    int code;
} keys[] = {{"Enter", "Connect", '\r'}, {"F2", "Favourite", KEY_F2}, {"Tab", "All/Favs", '\t'},
            {"Esc", "Clear/Quit", KEY_ESC}, {"F10", "Quit", KEY_F10}};
#define NKEYS (int)(sizeof keys / sizeof *keys)
static int key_col[NKEYS + 1];

static void draw_keybar(void) {
    int c = 0;
    scr_fill(SCR_H - 1, 0, 1, SCR_W, ' ', C_KEYTEXT);
    for (int i = 0; i < NKEYS; ++i) {
        key_col[i] = c;
        scr_text(SCR_H - 1, c, -1, C_KEYNUM, keys[i].key);
        c += strlen(keys[i].key);
        scr_text(SCR_H - 1, c, -1, C_KEYTEXT, keys[i].text);
        c += strlen(keys[i].text) + 1;
    }
    key_col[NKEYS] = c;
    scr_text(SCR_H - 1, c + 1, SCR_W - c - 1, C_KEYTEXT, "Type to filter");
}

static void draw_row(int r) {
    int row = LIST_TOP + r, i = M->top + r;
    bool sel = i == M->cur && i < M->nshown;
    uint8_t at = sel ? C_SELECT : C_PANEL;
    scr_fill(row, 1, 1, SCR_W - 2, ' ', at);
    for (unsigned s = 0; s < sizeof seps / sizeof *seps; ++s)
        scr_put(row, seps[s], S_V, at);
    if (i >= M->nshown)
        return;
    const dial_entry_t *e = &M->e[M->shown[i]];
    if (e->fav)
        scr_put(row, 2, '*', sel ? C_SELMARK : C_MARKED);
    uint8_t host_at = e->ssh ? (sel ? C_SSH_SEL : C_SSH) : at;
    scr_text(row, COL_NAME, COL_HOST - 1 - COL_NAME - 1, sel ? at : A(WHITE, BLUE), e->name);
    char host[80];
    snprintf(host, sizeof host, "%s%s%s", e->user, e->user[0] ? "@" : "", e->host);
    scr_text(row, COL_HOST, COL_PORT - 1 - COL_HOST - 1, host_at, host);
    scr_printf(row, COL_PORT, 5, at, "%5u", e->port);
    scr_text(row, COL_VIA, 3, host_at, e->ssh ? "ssh" : "tel");
}

static void draw(void) {
    scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
    scr_printf(0, 1, SCR_W - 2, C_MENU, "Telnet directory   %d BBSes (%d by SSH)   %s", M->n, M->nssh,
               M->source);
    scr_box(1, 0, SCR_H - 2, SCR_W, true, C_PANEL);
    scr_printf(1, 3, -1, C_TITLE_ON, " %s ", M->favs_only ? "Favourites" : "All BBSes");
    scr_fill(2, 1, 1, SCR_W - 2, ' ', C_PANEL);
    scr_hline(3, 0, SCR_W, DS_L, S_H, DS_R, C_PANEL);
    for (unsigned s = 0; s < sizeof seps / sizeof *seps; ++s) {
        scr_put(1, seps[s], DS_T, C_PANEL);
        scr_put(2, seps[s], S_V, C_PANEL);
        scr_put(3, seps[s], SS_X, C_PANEL);
        scr_put(SCR_H - 2, seps[s], DS_B, C_PANEL);
    }
    scr_text(2, COL_NAME, -1, C_HEADER, "Name");
    scr_text(2, COL_HOST, -1, C_HEADER, "Host");
    scr_text(2, COL_PORT, -1, C_HEADER, "Port");
    scr_text(2, COL_VIA, -1, C_HEADER, "Via");
    for (int r = 0; r < LIST_ROWS; ++r)
        draw_row(r);
    // The bottom line: the filter (or a note), and where in the list.
    if (M->note[0])
        scr_printf(SCR_H - 2, 2, 60, C_TITLE_ON, " %s ", M->note);
    else if (M->filter[0])
        scr_printf(SCR_H - 2, 2, 40, C_INPUT, " Filter: %s_ ", M->filter);
    else if (!M->nshown && M->favs_only)
        scr_text(SCR_H - 2, 2, -1, C_PANEL, " No favourites yet: F2 marks one ");
    char where[24];
    snprintf(where, sizeof where, " %d of %d ", M->nshown ? M->cur + 1 : 0, M->nshown);
    scr_text(SCR_H - 2, SCR_W - 2 - (int)strlen(where), -1, C_PANEL, where);
    draw_keybar();
    scr_cursor(-1, 0);
}

// ---- Calling ----------------------------------------------------------------------------------------

/// A session on the shell's screen; then a key brings the list back.
static void call(const dial_entry_t *e) {
    key_of(e, M->last, sizeof M->last);
    save_favs();
    mouse_pointer(MOUSE_POINTER_OFF);
    printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h%s\r\n", e->name);
    fflush(stdout);
    char port[8], who[80];
    snprintf(port, sizeof port, "%u", e->port);
    if (e->ssh) {
        snprintf(who, sizeof who, "%s%s%s", e->user, e->user[0] ? "@" : "", e->host);
        char *argv[] = {"ssh", "-p", port, who, NULL};
        net_cmd_ssh(4, argv);
    } else {
        char host[sizeof e->host];
        snprintf(host, sizeof host, "%s", e->host);
        char *argv[] = {"telnet", host, port, NULL};
        net_cmd_telnet(3, argv);
    }
    console_reset_modes();
    printf("\x1b[0m\r\nPress any key to go back to the directory ");
    fflush(stdout);
    key_get();
    printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25l");
    fflush(stdout);
    scr_grab_user();  // (so leaving shows a clear screen, not the BBS's)
}

static void toggle_fav(void) {
    if (!M->nshown)
        return;
    dial_entry_t *e = &M->e[M->shown[M->cur]];
    e->fav = !e->fav;
    snprintf(M->note, sizeof M->note, "%s favourites", e->fav ? "Added to" : "Taken off");
    save_favs();
    if (M->favs_only)
        apply_filter();
}

static void handle(int k) {
    switch (k) {
    case KEY_UP: --M->cur; break;
    case KEY_DOWN: ++M->cur; break;
    case KEY_PGUP: M->cur -= LIST_ROWS - 1; break;
    case KEY_PGDN: M->cur += LIST_ROWS - 1; break;
    case KEY_HOME: M->cur = 0; break;
    case KEY_END: M->cur = M->nshown - 1; break;
    case '\r':
        if (M->nshown)
            call(&M->e[M->shown[M->cur]]);
        break;
    case KEY_F2:
    case KEY_INS:
        toggle_fav();
        break;
    case '\t':
        M->favs_only = !M->favs_only;
        apply_filter();
        break;
    case '\b':
        if (M->filter[0]) {
            M->filter[strlen(M->filter) - 1] = 0;
            apply_filter();
        }
        break;
    case KEY_ESC:
        if (M->filter[0]) {
            M->filter[0] = 0;
            apply_filter();
        } else {
            M->quit = true;
        }
        break;
    case KEY_F10:
    case CTRL('c'):
        M->quit = true;
        break;
    default:
        if (k >= ' ' && k < 0x7f) {
            size_t n = strlen(M->filter);
            if (n + 1 < sizeof M->filter) {
                M->filter[n] = (char)k;
                M->filter[n + 1] = 0;
                apply_filter();
            }
        }
    }
    fix_scroll();
}

static void mouse_click(const mouse_event_t *e) {
    int row = e->y / 16, col = e->x / 8;
    if (e->type == MOUSE_WHEEL) {
        M->cur += e->wheel > 0 ? -3 : 3;
        fix_scroll();
        return;
    }
    if (e->type != MOUSE_DOWN)
        return;
    if (row == SCR_H - 1) {
        for (int i = 0; i < NKEYS; ++i)
            if (col >= key_col[i] && col < key_col[i + 1])
                handle(keys[i].code);
        return;
    }
    int i = M->top + row - LIST_TOP;
    if (row < LIST_TOP || row >= LIST_TOP + LIST_ROWS || i >= M->nshown)
        return;
    M->cur = i;
    if (e->button == MOUSE_RIGHT)
        toggle_fav();
    else if (e->double_click)
        handle('\r');
}

void telnet_menu(const char *path) {
    if (scr_active()) {  // (mc's screen is in use: from mc's command line)
        printf("telnet: the directory can't open inside mc; use telnet host [port]\n");
        return;
    }
    M = psram_malloc(sizeof *M);
    if (!M) {
        printf("telnet: not enough memory\n");
        return;
    }
    memset(M, 0, sizeof *M);
    printf("Reading the directory...\r");
    fflush(stdout);
    if (load_directory(path)) {
        load_favs();
        apply_filter();
        for (int i = 0; i < M->nshown && M->last[0]; ++i) {  // back where we were
            char k[64];
            key_of(&M->e[M->shown[i]], k, sizeof k);
            if (!strcasecmp(k, M->last))
                M->cur = i;
        }
        M->top = M->cur - LIST_ROWS / 2;
        fix_scroll();
        scr_begin();
        while (!M->quit) {
            mouse_pointer(MOUSE_POINTER_TEXT);
            draw();
            scr_flush();
            mouse_event_t e;
            int k = key_get_mouse(&e);
            if (k != KEY_MOUSE || e.type == MOUSE_DOWN)
                M->note[0] = 0;
            if (k == KEY_MOUSE)
                mouse_click(&e);
            else
                handle(k);
        }
        mouse_pointer(MOUSE_POINTER_OFF);
        scr_end();
    }
    psram_free(M->e);
    psram_free(M->shown);
    psram_free(M);
    M = NULL;
}
