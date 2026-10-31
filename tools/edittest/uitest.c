/**
 * @file uitest.c
 * @brief The text editor on a PC: its VT100 output read back by a small terminal, keys and mouse events fed in, and the files it saves checked. Clicking, dragging (also past the edges, which scrolls), double-clicking a word, the wheel, the right button's menu, Ctrl-X / C / V / A, typing over a selection, and the old Ctrl-K / Ctrl-U; screenshots.
 *
 *     ./uitest.sh  (builds this, writes edit_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vt_printf(const char *fmt, ...);
int vt_putchar(int c);
#define printf vt_printf    ///< the editor's output goes to the terminal below
#define putchar vt_putchar  ///< and its single characters
#define PSRAM_UNCACHED_OFFSET 0  ///< no second window on a PC
#include "../../src/editor.c"
#undef printf
#undef putchar
#include "../../src/font8x16.h"

// ---- A small terminal --------------------------------------------------------

/// A screen: each cell's character, colours (reverse applied) and the cursor.
typedef struct {
    unsigned char ch[ROWS][COLS];  ///< characters
    uint8_t fg[ROWS][COLS];        ///< foreground colour (0-15)
    uint8_t bg[ROWS][COLS];        ///< background colour
    int row, col;                  ///< the cursor
    bool cursor_on;                ///< it is shown
} screen_t;

static screen_t scr;  ///< the terminal's screen
/// The terminal's state: colours, reverse, line drawing, scroll region, escape parsing.
///@{
static int t_fg = 7, t_bg = 0, t_top = 0, t_bot = ROWS - 1, esc_state, nparams, params[8];
static bool t_rev, t_lines, t_private;
///@}

/// Scrolls rows [t_top, t_bot] up (n > 0) or down by one.
/// @param up up (else down)
static void t_scroll(bool up) {
    int a = up ? t_top : t_bot, b = up ? t_bot : t_top, d = up ? 1 : -1;
    for (int r = a; r != b; r += d) {
        memcpy(scr.ch[r], scr.ch[r + d], COLS);
        memcpy(scr.fg[r], scr.fg[r + d], COLS);
        memcpy(scr.bg[r], scr.bg[r + d], COLS);
    }
    memset(scr.ch[b], ' ', COLS);
    memset(scr.fg[b], 7, COLS);
    memset(scr.bg[b], 0, COLS);
}

/// The terminal takes a byte.
/// @param c the byte
static void t_put(unsigned char c) {
    if (esc_state == 1) {
        esc_state = c == '[' ? 2 : c == '(' ? 3 : 0;
        nparams = 0;
        t_private = false;
        memset(params, 0, sizeof params);
        return;
    }
    if (esc_state == 3) {
        t_lines = c == '0';
        esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (!nparams)
                nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + c - '0';
        } else if (c == ';') {
            if (!nparams)
                nparams = 1;
            ++nparams;
        } else if (c == '?') {
            t_private = true;
        } else {
            esc_state = 0;
            int p0 = nparams ? params[0] : 0;
            switch (c) {
            case 'H':
                scr.row = (p0 ? p0 : 1) - 1;
                scr.col = (nparams > 1 && params[1] ? params[1] : 1) - 1;
                break;
            case 'J':
                memset(scr.ch, ' ', sizeof scr.ch);
                memset(scr.fg, 7, sizeof scr.fg);
                memset(scr.bg, 0, sizeof scr.bg);
                break;
            case 'r':
                t_top = nparams ? params[0] - 1 : 0;
                t_bot = nparams > 1 ? params[1] - 1 : ROWS - 1;
                break;
            case 'S': case 'T':
                for (int i = 0; i < (p0 ? p0 : 1); ++i)
                    t_scroll(c == 'S');
                break;
            case 'h': case 'l':
                if (t_private && p0 == 25)
                    scr.cursor_on = c == 'h';
                break;
            case 'm':
                for (int i = 0; i < (nparams ? nparams : 1); ++i) {
                    int p = params[i];
                    if (p == 0)
                        t_fg = 7, t_bg = 0, t_rev = false;
                    else if (p == 7)
                        t_rev = true;
                    else if (p >= 30 && p <= 37)
                        t_fg = p - 30;
                    else if (p >= 40 && p <= 47)
                        t_bg = p - 40;
                    else if (p >= 90 && p <= 97)
                        t_fg = p - 90 + 8;
                }
                break;
            }
        }
        return;
    }
    if (c == 0x1b) {
        esc_state = 1;
        return;
    }
    if (scr.col >= COLS) {  // the wrap waiting after the last column
        scr.col = 0;
        if (scr.row == t_bot)
            t_scroll(true);
        else if (scr.row < ROWS - 1)
            ++scr.row;
    }
    if (t_lines && c >= 'j' && c <= 'x')  // DEC line drawing, as CP437 boxes
        c = strchr("jklmn", c) ? "\xd9\xbf\xda\xc0\xc5"[c - 'j']
            : c == 'q' ? 0xc4 : c == 't' ? 0xc3 : c == 'u' ? 0xb4 : c == 'x' ? 0xb3 : c;
    scr.ch[scr.row][scr.col] = c;
    scr.fg[scr.row][scr.col] = t_rev ? t_bg : t_fg;
    scr.bg[scr.row][scr.col] = t_rev ? t_fg : t_bg;
    ++scr.col;
}

int vt_printf(const char *fmt, ...) {
    char out[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof out, fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && i < (int)sizeof out - 1; ++i)
        t_put(out[i]);
    return n;
}

int vt_putchar(int c) {
    t_put(c);
    return c;
}

// ---- The keys, the mouse and the clock ---------------------------------------

/// What the test feeds in, in order.
enum { EV_KEYS, EV_MOUSE, EV_WAIT, EV_SNAP };
/// One thing fed in.
typedef struct {
    int kind;            ///< EV_
    const char *keys;    ///< EV_KEYS: the bytes typed
    mouse_event_t m;     ///< EV_MOUSE: the event
    int ms;              ///< EV_WAIT: how long nothing happens
    int snap;            ///< EV_SNAP: the screen kept in snaps[snap]
} ev_t;

static ev_t q[256];               ///< the queue
static int qn, qi, key_i;         ///< its length, the next one, the next byte of EV_KEYS
static uint64_t now_us = 1000000; ///< the clock
static uint64_t wait_until;       ///< EV_WAIT's end (0: not started)
static int mx, my, mbuttons;      ///< the mouse now
static int pointer_mode;          ///< mouse_pointer's mode
static screen_t snaps[8];         ///< screens kept by EV_SNAP
static int fails;                 ///< checks that failed

void *psram_malloc(size_t n) { return malloc(n); }
void psram_free(void *p) { free(p); }
const char *fresult_name(FRESULT r) { return r == FR_DENIED ? "denied" : r ? "error" : "ok"; }
absolute_time_t make_timeout_time_ms(uint32_t ms) { return now_us + ms * 1000ull; }
bool time_reached(absolute_time_t t) { return now_us >= t; }
void mouse_get(int *x, int *y, int *b) { *x = mx, *y = my, *b = mbuttons; }
void mouse_flush(void) {}
void mouse_pointer(int mode) { pointer_mode = mode; }
int mouse_pointer_mode(void) { return pointer_mode; }
void mouse_pointer_update(void) {}

int getchar_timeout_us(uint32_t us) {
    for (;;) {
        if (qi >= qn) {
            printf("FAIL: the editor wants more input than the test gave\n");
            exit(1);
        }
        ev_t *e = &q[qi];
        if (e->kind == EV_KEYS) {
            int c = (unsigned char)e->keys[key_i++];
            if (!e->keys[key_i])
                ++qi, key_i = 0;
            return c;
        }
        if (e->kind == EV_SNAP && us >= 10000) {  // only when the editor waits for a key (it has drawn)
            snaps[e->snap] = scr;
            ++qi;
            continue;
        }
        if (e->kind == EV_WAIT && us >= 10000) {
            if (!wait_until)
                wait_until = now_us + e->ms * 1000ull;
            if (now_us >= wait_until) {
                wait_until = 0;
                ++qi;
                continue;
            }
        }
        now_us += us;
        return PICO_ERROR_TIMEOUT;
    }
}

bool mouse_event(mouse_event_t *e) {
    if (qi >= qn || q[qi].kind != EV_MOUSE)
        return false;
    *e = q[qi++].m;
    mx = e->x, my = e->y, mbuttons = e->buttons;
    return true;
}

/// Adds keys to the queue.
/// @param s the bytes
static void keys(const char *s) { q[qn++] = (ev_t){.kind = EV_KEYS, .keys = s}; }
/// Adds a mouse event at a text cell (its middle).
/// @param type MOUSE_
/// @param button the button that changed (DOWN, UP)
/// @param buttons those held after it
/// @param row the screen row
/// @param col the column
/// @param dbl a double-click
static void mouse(int type, int button, int buttons, int row, int col, bool dbl) {
    q[qn++] = (ev_t){.kind = EV_MOUSE,
                     .m = {(uint8_t)type, (uint8_t)button, (uint8_t)buttons, 0, (int16_t)(col * 8 + 4),
                           (int16_t)(row * 16 + 8), dbl}};
}
/// A click (down and up) at a cell.
/// @param button the button
/// @param row the row
/// @param col the column
static void click(int button, int row, int col) {
    mouse(MOUSE_DOWN, button, button, row, col, false);
    mouse(MOUSE_UP, button, 0, row, col, false);
}
/// The wheel turned at a cell.
/// @param steps + up
static void wheel(int steps) {
    q[qn++] = (ev_t){.kind = EV_MOUSE, .m = {MOUSE_WHEEL, 0, 0, (int8_t)steps, 300, 200, false}};
}
/// Nothing happens for a while (the editor waits for keys).
/// @param ms how long
static void wait_ms(int ms) { q[qn++] = (ev_t){.kind = EV_WAIT, .ms = ms}; }
/// The screen kept, once the editor has drawn it.
/// @param n which snaps[] slot
static void snap(int n) { q[qn++] = (ev_t){.kind = EV_SNAP, .snap = n}; }

// ---- Files and checks ---------------------------------------------------------

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param p the path
/// @return the local one
static const char *local(const char *p) {
    static char b[2][300];
    static int k;
    k ^= 1;
    snprintf(b[k], sizeof b[k], ".%s%s", p[0] == '/' ? "" : "/", p);
    return b[k];
}
FRESULT f_open(FIL *f, const char *p, int mode) {
    f->f = fopen(local(p), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : FR_NO_FILE;
}
FRESULT f_read(FIL *f, void *b, UINT n, UINT *got) { *got = (UINT)fread(b, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *b, UINT n, UINT *put) { *put = (UINT)fwrite(b, 1, n, f->f); return FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? FR_DENIED : FR_OK; }
unsigned long f_size(FIL *f) {
    long at = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long n = ftell(f->f);
    fseek(f->f, at, SEEK_SET);
    return (unsigned long)n;
}
FRESULT f_unlink(const char *p) { return remove(local(p)) ? FR_NO_FILE : FR_OK; }
FRESULT f_rename(const char *a, const char *b) { return rename(local(a), local(b)) ? FR_DENIED : FR_OK; }

/// Writes a file.
/// @param name its name
/// @param text what's in it
static void put_file(const char *name, const char *text) {
    FILE *f = fopen(name, "wb");
    fputs(text, f);
    fclose(f);
}

/// Checks a file holds some text.
/// @param what the check's name
/// @param name the file
/// @param want what it should hold
static void check_file(const char *what, const char *name, const char *want) {
    static char got[1 << 16];
    FILE *f = fopen(name, "rb");
    size_t n = f ? fread(got, 1, sizeof got - 1, f) : 0;
    if (f)
        fclose(f);
    got[n] = 0;
    if (strcmp(got, want)) {
        printf("FAIL %s:\n  want \"%s\"\n  got  \"%s\"\n", what, want, got);
        ++fails;
    } else {
        printf("ok   %s\n", what);
    }
}

/// Checks a condition.
/// @param what the check's name
/// @param ok it held
static void check(const char *what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

/// A screen row's text (trailing spaces dropped).
/// @param s the screen
/// @param row the row
/// @return its text
static const char *row_text(const screen_t *s, int row) {
    static char t[COLS + 1];
    memcpy(t, s->ch[row], COLS);
    int n = COLS;
    while (n > 0 && t[n - 1] == ' ')
        --n;
    t[n] = 0;
    return t;
}

/// Whether a cell shows reversed (dark on light: the selection).
/// @param s the screen
/// @param row the row
/// @param col the column
/// @return it does
static bool rev(const screen_t *s, int row, int col) { return s->bg[row][col] == 7 && s->fg[row][col] == 0; }

/// The columns of a row shown reversed, as "from-to" ("" if none).
/// @param s the screen
/// @param row the row
/// @return the span
static const char *rev_span(const screen_t *s, int row) {
    static char t[16];
    int a = -1, b = -1;
    for (int c = 0; c < COLS; ++c)
        if (rev(s, row, c)) {
            if (a < 0)
                a = c;
            b = c + 1;
        }
    snprintf(t, sizeof t, a < 0 ? "" : "%d-%d", a, b);
    return t;
}

/// Runs the editor on a file with what's queued, which must all be used.
/// @param name the file
static void run(const char *name) {
    qi = key_i = 0;
    editor_run(name);
    if (qi != qn) {
        printf("FAIL: %d of the test's inputs were left over\n", qn - qi);
        ++fails;
    }
    qn = 0;
}

/// Saves a screen as raw RGB (640 x 480), for uitest.sh to make a PNG of.
/// @param s the screen
/// @param name the file
static void shot(const screen_t *s, const char *name) {
    static const uint8_t pal[16][3] = {
        {0, 0, 0},       {170, 0, 0},     {0, 170, 0},    {170, 85, 0},   {0, 0, 170},    {170, 0, 170},
        {0, 170, 170},   {170, 170, 170}, {85, 85, 85},   {255, 85, 85},  {85, 255, 85},  {255, 255, 85},
        {85, 85, 255},   {255, 85, 255},  {85, 255, 255}, {255, 255, 255},
    };
    FILE *f = fopen(name, "wb");
    for (int y = 0; y < ROWS * 16; ++y)
        for (int x = 0; x < COLS * 8; ++x) {
            int r = y / 16, c = x / 8;
            bool on = font8x16[s->ch[r][c]][y % 16] & (0x80 >> (x % 8));
            if (s->cursor_on && r == s->row && c == s->col && y % 16 >= 14)
                on = true;
            fwrite(pal[on ? s->fg[r][c] : s->bg[r][c]], 1, 3, f);
        }
    fclose(f);
}

int main(void) {
    const char *three = "hello world\nsecond line\nthird\n";

    // A click places the cursor; typing goes there.
    put_file("T1.TXT", three);
    click(MOUSE_LEFT, 1, 6);
    keys("X\x13");
    keys("\x11");
    run("/T1.TXT");
    check_file("click places the cursor", "T1.TXT", "hello Xworld\nsecond line\nthird\n");

    // Dragging selects (a selected line end shows as a reversed space), Ctrl-X cuts, Ctrl-V pastes.
    put_file("T2.TXT", three);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 1, 0, false);
    mouse(MOUSE_MOVE, 0, MOUSE_LEFT, 1, 30, false);
    mouse(MOUSE_MOVE, 0, MOUSE_LEFT, 2, 6, false);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 2, 6, false);
    snap(0);
    keys("\x18");
    snap(1);
    click(MOUSE_LEFT, 3, 20);  // past the end of the text: its last place
    keys("\x16\x13\x11");
    run("/T2.TXT");
    check("drag: \"hello world\" and its line end reversed", !strcmp(rev_span(&snaps[0], 1), "0-12"));
    check("drag: \"second\" reversed", !strcmp(rev_span(&snaps[0], 2), "0-6"));
    check("drag: the status row counts 18 selected", strstr(row_text(&snaps[0], ROWS - 1), "18 selected") != NULL);
    check("Ctrl-X: the first row is \" line\"", !strcmp(row_text(&snaps[1], 1), " line"));
    check("Ctrl-X: nothing reversed now", !rev_span(&snaps[1], 1)[0]);
    check_file("Ctrl-X then Ctrl-V at the end", "T2.TXT", " line\nthird\nhello world\nsecond");

    // A double-click selects a word; typing replaces it.
    put_file("T3.TXT", three);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 1, 8, false);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 1, 8, false);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 1, 8, true);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 1, 8, false);
    snap(0);
    keys("there\x13\x11");
    run("/T3.TXT");
    check("double-click: \"world\" reversed", !strcmp(rev_span(&snaps[0], 1), "6-11"));
    check_file("double-click and type", "T3.TXT", "hello there\nsecond line\nthird\n");

    // The right button's menu: Copy on a selection (click), then Paste
    // elsewhere (press, move onto it, let go).
    put_file("T4.TXT", three);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 2, 0, true);  // "second"
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 2, 0, false);
    mouse(MOUSE_DOWN, MOUSE_RIGHT, MOUSE_RIGHT, 2, 3, false);
    mouse(MOUSE_UP, MOUSE_RIGHT, 0, 2, 3, false);
    snap(0);
    click(MOUSE_LEFT, 5, 6);  // Copy: the second item, on row 2 + 1 + 1 + 1
    snap(1);
    mouse(MOUSE_DOWN, MOUSE_RIGHT, MOUSE_RIGHT, 3, 5, false);  // the end of "third"
    mouse(MOUSE_MOVE, 0, MOUSE_RIGHT, 7, 8, false);           // Paste: row 3 + 1 + 1 + 2
    mouse(MOUSE_UP, MOUSE_RIGHT, 0, 7, 8, false);
    keys("\x13\x11");
    run("/T4.TXT");
    check("menu: drawn below the click", strstr(row_text(&snaps[0], 4), "Cut") && strstr(row_text(&snaps[0], 4), "Ctrl-X"));
    check("menu: Select all and Quit in it", strstr(row_text(&snaps[0], 9), "Select all") && strstr(row_text(&snaps[0], 16), "Quit"));
    check("menu: the selection kept under it", !strcmp(rev_span(&snaps[0], 2), "0-6"));
    check("menu: gone after Copy", !strstr(row_text(&snaps[1], 4), "Cut"));
    check("menu: Copy leaves the selection", !strcmp(rev_span(&snaps[1], 2), "0-6"));
    check_file("menu: Copy, then Paste by letting go on it", "T4.TXT", "hello world\nsecond line\nthirdsecond\n");
    shot(&snaps[0], "shot_menu.raw");

    // Greyed items do nothing; Esc closes the menu. A right-click outside the
    // selection moves the cursor there first.
    put_file("T5.TXT", three);
    click(MOUSE_RIGHT, 1, 2);
    click(MOUSE_LEFT, 3, 4);  // Cut, greyed: nothing selected
    snap(0);
    keys("\x1b");
    wait_ms(100);  // a lone Esc
    keys("Z\x13\x11");
    run("/T5.TXT");
    check("menu: a greyed item leaves it open", strstr(row_text(&snaps[0], 3), "Cut") != NULL);
    check_file("menu: Esc closes it; the right-click placed the cursor", "T5.TXT", "heZllo world\nsecond line\nthird\n");

    // Ctrl-A selects all; Delete removes it.
    put_file("T6.TXT", three);
    keys("\x01");
    keys("\x1b[3~\x13\x11");
    run("/T6.TXT");
    check_file("Ctrl-A, Delete", "T6.TXT", "");

    // The wheel scrolls the view, not the cursor; a key brings the cursor back.
    static char hundred[100 * 12];
    hundred[0] = 0;
    for (int i = 1; i <= 100; ++i)
        sprintf(hundred + strlen(hundred), "line %d\n", i);
    put_file("T7.TXT", hundred);
    wheel(-2);
    snap(0);
    keys("A");
    snap(1);
    keys("\x13\x11");
    run("/T7.TXT");
    check("wheel: 6 lines down", !strcmp(row_text(&snaps[0], 1), "line 7"));
    check("wheel: the cursor off the screen is hidden", !snaps[0].cursor_on);
    check("a key: back to the cursor", !strcmp(row_text(&snaps[1], 1), "Aline 1"));

    // Dragging below the text scrolls on while the pointer stays there.
    put_file("T8.TXT", hundred);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 27, 0, false);  // "line 27"
    mouse(MOUSE_MOVE, 0, MOUSE_LEFT, 29, 3, false);          // onto the status row
    wait_ms(520);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 29, 3, false);
    snap(0);
    keys("\x18\x13\x11");
    run("/T8.TXT");
    int last = atoi(row_text(&snaps[0], TEXT_ROWS) + 5);
    printf("     (dragging below: the last row shows line %d)\n", last);
    check("drag past the bottom scrolls", last >= 33 && last <= 45);
    shot(&snaps[0], "shot_drag.raw");
    {
        FILE *f = fopen("T8.TXT", "rb");
        static char got[sizeof hundred];
        size_t n = fread(got, 1, sizeof got - 1, f);
        fclose(f);
        got[n] = 0;
        char want[sizeof hundred];
        snprintf(want, sizeof want, "line 1\n");
        for (int i = 2; i <= 26; ++i)
            sprintf(want + strlen(want), "line %d\n", i);
        // From line 27 to column 3 of the last line shown (the cursor's) is gone.
        char rest[32];
        snprintf(rest, sizeof rest, "e %d\n", last);
        check("drag past the bottom: the cut is line 27 to under the pointer",
              !strncmp(got, want, strlen(want)) && !strncmp(got + strlen(want), rest, strlen(rest)));
    }

    // The keys from before still work: Ctrl-K twice, Ctrl-U.
    put_file("T9.TXT", three);
    keys("\x0b\x0b\x1b[B\x15\x13\x11");
    run("/T9.TXT");
    check_file("Ctrl-K, Ctrl-K, Down, Ctrl-U", "T9.TXT", "third\nhello world\nsecond line\n");

    // The menu bar: a click on Edit opens it, Select all; Search by moving
    // the pointer onto its title while Edit is open; F10 opens File, Right
    // goes to Edit; Alt-S to Search; Esc; then File > Save by pressing on
    // File, moving onto Save and letting go.
    put_file("T11.TXT", three);
    click(MOUSE_LEFT, 0, 8);  // Edit
    snap(0);
    mouse(MOUSE_MOVE, 0, 0, 0, 14, false);  // Search
    snap(1);
    keys("\x1b[A");  // Up: its last item, Go to line...
    keys("\r2\r");   // line 2
    keys("\x1b[21~");  // F10: File
    snap(2);
    keys("\x1b[C");  // Right: Edit
    snap(3);
    keys("\x1bs");    // Alt-S: Search
    snap(4);
    keys("\x1b");
    wait_ms(100);
    keys("Q");  // at the start of line 2
    click(MOUSE_LEFT, 0, 8);  // Edit > Select all
    click(MOUSE_LEFT, 7, 8);
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 0, 2, false);  // File, held
    mouse(MOUSE_MOVE, 0, MOUSE_LEFT, 2, 4, false);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 2, 4, false);  // Save
    snap(5);
    keys("\x11");
    run("/T11.TXT");
    check("menu bar: File Edit Search on the title row", !strncmp(row_text(&snaps[0], 0), " File  Edit  Search", 19));
    check("menu bar: Edit open under its title", strstr(row_text(&snaps[0], 2), "Cut") && snaps[0].ch[1][6] == 0xda);
    check("menu bar: Edit's title highlighted", snaps[0].bg[0][8] == 4);
    check("menu bar: the pointer moved to Search", strstr(row_text(&snaps[1], 2), "Find...") && snaps[1].bg[0][14] == 4);
    check("menu bar: F10 opens File", strstr(row_text(&snaps[2], 2), "Save") && snaps[2].bg[0][2] == 4);
    check("menu bar: Right goes to Edit", strstr(row_text(&snaps[3], 2), "Cut") != NULL);
    check("menu bar: Alt-S to Search", strstr(row_text(&snaps[4], 3), "Find next") != NULL);
    check("menu bar: saved by press, move, let go", !strstr(row_text(&snaps[5], 0), "[modified]"));
    check_file("menu bar: Go to line, Select all, Save", "T11.TXT", "hello world\nQsecond line\nthird\n");
    shot(&snaps[0], "shot_bar.raw");

    // A selection drawn for the screenshot, with the status row.
    put_file("T10.TXT", "#include <stdio.h>\n\nint main(void) {\n    printf(\"hello\\n\");\n    return 0;\n}\n");
    mouse(MOUSE_DOWN, MOUSE_LEFT, MOUSE_LEFT, 3, 4, false);
    mouse(MOUSE_MOVE, 0, MOUSE_LEFT, 4, 9, false);
    mouse(MOUSE_UP, MOUSE_LEFT, 0, 4, 9, false);
    mouse(MOUSE_DOWN, MOUSE_RIGHT, MOUSE_RIGHT, 4, 6, false);
    mouse(MOUSE_MOVE, 0, MOUSE_RIGHT, 7, 10, false);
    snap(0);
    mouse(MOUSE_UP, MOUSE_RIGHT, 0, 7, 10, false);  // Copy
    keys("\x11");
    run("/T10.TXT");
    shot(&snaps[0], "shot_select.raw");
    check("the pointer's item highlighted", snaps[0].bg[7][12] == 4);

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
