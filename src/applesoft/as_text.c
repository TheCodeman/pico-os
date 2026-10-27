/**
 * @file as_text.c
 * @brief The text screen and keyboard, as the enhanced IIe's monitor and 80-column firmware run them.
 *
 * COUT with its text window, inverse and flash, GETLN's line editing (with
 * escape-mode cursor moves and copying from the screen), RDKEY's cursor, and
 * the keyboard latch at $C000.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "app_overlay.h"
#include "as_internal.h"
#include "as_port.h"

static uint8_t kbd;             ///< $C000: bit 7 set while a key is waiting
static bool col80;              ///< PR#3 is active
static uint8_t aux_text[0x400] APP_OV_DATA(apple2); ///< 80 columns: the even columns ($400-$7FF aux)
static bool last_was_cr = true; ///< DOS commands start a line
static bool capturing;          ///< after a Ctrl-D: a DOS command is being printed
static char dos_line[256] APP_OV_DATA(apple2);
static int dos_len;

uint8_t *as_aux_text(void) {
    return aux_text;
}

bool text_80col(void) {
    return col80;
}

int text_column(void) {
    return col80 ? M[0x57b] : M[Z_CH];
}

int text_list_wrap(void) {
    return col80 ? 73 : 33;
}

/// BASCALC: the address of the start of text row r.
static uint16_t row_addr(int r) {
    return (uint16_t)(0x400 + (r & 7) * 128 + (r >> 3) * 40);
}

void as_vtab(void) {
    uint16_t b = row_addr(M[Z_CV]);
    if (!col80)
        b += M[Z_WNDLFT];
    sw16(Z_BASL, b);
}

/// A character into the screen at column col of the row at BASL (80 columns:
/// even columns in auxiliary memory).
static void put_cell(int col, uint8_t c) {
    if (col80) {
        uint16_t base = (uint16_t)(w16(Z_BASL) + M[Z_WNDLFT] / 2);
        int x = col + (M[Z_WNDLFT] & 1);
        uint16_t a = (uint16_t)(base + x / 2);
        if (x & 1)
            M[a] = c;
        else
            aux_text[(a - 0x400) & 0x3ff] = c;
        video_mark(a);
        return;
    }
    uint16_t a = (uint16_t)(w16(Z_BASL) + col);
    M[a] = c;
    video_mark(a);
}

static uint8_t get_cell(int col) {
    if (col80) {
        uint16_t base = (uint16_t)(w16(Z_BASL) + M[Z_WNDLFT] / 2);
        int x = col + (M[Z_WNDLFT] & 1);
        uint16_t a = (uint16_t)(base + x / 2);
        return (x & 1) ? M[a] : aux_text[(a - 0x400) & 0x3ff];
    }
    return M[(uint16_t)(w16(Z_BASL) + col)];
}

static int width(void) {
    return M[Z_WNDWDTH];
}

static void clear_row_from(int from) {
    for (int x = from; x < width(); ++x)
        put_cell(x, 0xa0);
}

void as_clreol(void) {
    clear_row_from(M[Z_CH]);
}

static void scroll(void) {
    uint8_t cv = M[Z_CV];
    for (int r = M[Z_WNDTOP]; r < M[Z_WNDBTM] - 1; ++r) {
        M[Z_CV] = (uint8_t)(r + 1);
        as_vtab();
        uint8_t line[80];
        for (int x = 0; x < width() && x < 80; ++x)
            line[x] = get_cell(x);
        M[Z_CV] = (uint8_t)r;
        as_vtab();
        for (int x = 0; x < width() && x < 80; ++x)
            put_cell(x, line[x]);
    }
    M[Z_CV] = (uint8_t)(M[Z_WNDBTM] - 1);
    as_vtab();
    clear_row_from(0);
    M[Z_CV] = cv;
    as_vtab();
}

static void lf(void) {
    M[Z_CV]++;
    if (M[Z_CV] < M[Z_WNDBTM]) {
        as_vtab();
        return;
    }
    M[Z_CV]--;
    as_vtab();
    scroll();
}

static void cr(void) {
    M[Z_CH] = 0;
    lf();
}

static void up(void) {
    if (M[Z_WNDTOP] < M[Z_CV]) {
        M[Z_CV]--;
        as_vtab();
    }
}

static void bs(void) {
    M[Z_CH]--;
    if (M[Z_CH] & 0x80) {
        M[Z_CH] = (uint8_t)(width() - 1);
        up();
    }
}

void as_clreop(void) {
    uint8_t cv = M[Z_CV], ch = M[Z_CH];
    as_clreol();
    for (int r = cv + 1; r < M[Z_WNDBTM]; ++r) {
        M[Z_CV] = (uint8_t)r;
        as_vtab();
        clear_row_from(0);
    }
    M[Z_CV] = cv;
    M[Z_CH] = ch;
    as_vtab();
}

void as_home(void) {
    M[Z_CV] = M[Z_WNDTOP];
    M[Z_CH] = 0;
    as_vtab();
    as_clreop();
    if (col80)
        M[0x57b] = 0;
}

void as_bell(void) {
    asp_tone(940, 102);
}

/// STOADV: a character into the window, then on.
static void stoadv(uint8_t c) {
    put_cell(M[Z_CH], c);
    M[Z_CH]++;
    if (M[Z_CH] >= width())
        cr();
}

static void wait_ctrl_s(void);

/// COUT1: the screen's output routine.
static void cout1(uint8_t c) {
    asp_mirror(c & 0x7f);
    uint8_t v = c >= 0xa0 ? (uint8_t)(c & M[Z_INVFLG]) : c;
    if (col80 && c >= 0xa0 && M[Z_INVFLG] != 0xff) {
        // The 80-column firmware shows inverse lowercase properly.
        v = (uint8_t)(c & 0x7f);
        if (v >= 0x40 && v < 0x60)
            v -= 0x40;
    }
    if (v == 0x8d)
        wait_ctrl_s();
    if (v >= 0xa0 || v < 0x80) {
        stoadv(v);
    } else {
        switch (v) {
        case 0x8d: cr(); break;
        case 0x8a: lf(); break;
        case 0x88: bs(); break;
        case 0x87: as_bell(); break;
        default:
            if (col80) {
                if (v == 0x8c)  // Ctrl-L: home
                    as_home();
                else if (v == 0x99)  // Ctrl-Y: home cursor
                    M[Z_CH] = 0, M[Z_CV] = M[Z_WNDTOP], as_vtab();
                else if (v == 0x9d)  // Ctrl-]: clear to end of line
                    as_clreol();
                else if (v == 0x8b)  // Ctrl-K: clear to end of screen
                    as_clreop();
                else if (v == 0x95) {  // Ctrl-U: to 40 columns
                    extern void text_set_80col(bool on);
                    text_set_80col(false);
                }
            }
            break;
        }
    }
    if (col80)
        M[0x57b] = M[Z_CH];
}

void as_cout(uint8_t c) {
    if (capturing) {
        if ((c & 0x7f) == '\r') {
            capturing = false;
            dos_line[dos_len] = 0;
            last_was_cr = true;
            dos_command(dos_line, true);
        } else if (dos_len < 255) {
            dos_line[dos_len++] = (char)(c & 0x7f);
        }
        return;
    }
    if ((c & 0x7f) == 4 && (last_was_cr || M[Z_CH] == 0)) {
        capturing = true;
        dos_len = 0;
        return;
    }
    last_was_cr = (c & 0x7f) == '\r';
    if (dos_output(c))
        return;
    cout1(c);
}

/// OUTDO: what PRINT uses. SPEED= slows it down.
void as_outdo(uint8_t c) {
    c |= 0x80;
    if (c >= 0xa0)
        c |= M[Z_FLASH];
    as_cout(c);
    uint8_t s = M[Z_SPEED];
    if (s > 1) {
        // The monitor's WAIT: about (26 + 27A + 5A^2) / 2 cycles.
        uint32_t cycles = (26u + 27u * s + 5u * s * s) / 2;
        asp_sleep_us(cycles);
    }
}

void as_crdo(void) {
    as_outdo('\r');
}

void text_htab(int n) {
    uint8_t x = (uint8_t)(n - 1);
    for (;;) {
        uint8_t w = width() > 40 ? (uint8_t)width() : 40;
        if (x < w) {
            M[Z_CH] = x;
            if (col80)
                M[0x57b] = x;
            return;
        }
        x -= w;
        M[Z_CH] = w;
        as_crdo();
    }
}

void text_set_80col(bool on) {
    col80 = on;
    vid.col80 = on;
    vid.altchar = on;  // the 80-column firmware uses the alternate set
    M[Z_WNDLFT] = 0;
    M[Z_WNDWDTH] = on ? 80 : 40;
    M[Z_WNDTOP] = vid.mixed && !vid.text ? 20 : 0;
    M[Z_WNDBTM] = 24;
    if (M[Z_CH] >= M[Z_WNDWDTH])
        M[Z_CH] = 0;
    as_vtab();
    as_home();  // the two layouts don't share screen memory
    video_mark_all();
}

void as_text_mode(void) {
    as_softswitch(0xc054, false, 0);  // page 1
    as_softswitch(0xc051, false, 0);  // text
    M[Z_WNDLFT] = 0;
    M[Z_WNDWDTH] = col80 ? 80 : 40;
    M[Z_WNDTOP] = 0;
    M[Z_WNDBTM] = 24;
    M[Z_CV] = 23;
    as_vtab();
}

void as_pr(int slot) {
    if (slot == 3)
        text_set_80col(true);
    else if (slot == 0 && col80) {
        // PR#0 with the 80-column firmware running leaves it to ESC Ctrl-Q;
        // we take it as the simple way back to 40 columns.
        text_set_80col(false);
    }
}

void as_in(int slot) {
    (void)slot;
}

void text_init(void) {
    col80 = false;
    memset(aux_text, 0xa0, sizeof aux_text);
    M[Z_WNDLFT] = 0;
    M[Z_WNDWDTH] = 40;
    M[Z_WNDTOP] = 0;
    M[Z_WNDBTM] = 24;
    M[Z_INVFLG] = 0xff;
    M[Z_PROMPT] = 0xdd;
    sw16(Z_CSWL, 0xfdf0);
    sw16(Z_KSWL, 0xfd1b);
    as_home();
    last_was_cr = true;
    capturing = false;
}

// ---- Keyboard ------------------------------------------------------------------

/// Keys from the PC side: ANSI escape sequences become the Apple's arrow keys.
static int pending_esc;  ///< characters held while an escape sequence decodes

static int read_host_key(uint32_t timeout_us) {
    int c = dos_input_char();
    if (c >= 0)
        return c;
    c = asp_key(timeout_us);
    if (c != 0x1b)
        return c == 0x7f ? 0x08 : c == '\n' ? '\r' : c;
    int d = asp_key(20000);
    if (d < 0)
        return 0x1b;
    if (d != '[' && d != 'O') {
        pending_esc = d;
        return 0x1b;
    }
    int e = asp_key(20000);
    switch (e) {
    case 'A': return 0x0b;
    case 'B': return 0x0a;
    case 'C': return 0x15;
    case 'D': return 0x08;
    default:
        // Some other sequence (function keys): swallow it.
        while (e >= '0' && e <= '9')
            e = asp_key(20000);
        return -1;
    }
}

static int next_key(uint32_t timeout_us) {
    if (pending_esc) {
        int c = pending_esc;
        pending_esc = 0;
        return c;
    }
    return read_host_key(timeout_us);
}

uint8_t as_kbd(void) {
    if (!(kbd & 0x80)) {
        int c = next_key(0);
        if (c >= 0)
            kbd = (uint8_t)(c | 0x80);
    }
    return kbd;
}

void as_kbd_strobe(void) {
    kbd &= 0x7f;
}

bool as_kbd_poll_break(void) {
    return as_kbd() == 0x83;
}

/// VIDWAIT: Ctrl-S stops output at the end of a line until another key.
static void wait_ctrl_s(void) {
    if (as_kbd() != 0x93)
        return;
    as_kbd_strobe();
    for (;;) {
        uint8_t k = as_kbd();
        if (k & 0x80) {
            if (k != 0x83)
                as_kbd_strobe();
            return;
        }
        video_update(true);
        if (asp_input_done())
            return;
        asp_sleep_us(5000);
    }
}

/// RDKEY: waits for a key with the cursor flashing.
uint8_t as_rdkey(void) {
    uint64_t start = asp_micros();
    for (;;) {
        uint8_t k = as_kbd();
        if (k & 0x80) {
            as_kbd_strobe();
            video_cursor(false, 0, 0);
            return k;
        }
        if (asp_input_done())
            as_quit();
        bool on = ((asp_micros() - start) / 250000) % 2 == 0;
        video_cursor(on, M[Z_CV], text_column());
        video_update(true);
        int c = next_key(20000);
        if (c >= 0)
            kbd = (uint8_t)(c | 0x80);
    }
}

/// RDCHAR: RDKEY, with escape mode: ESC, then I J K M or the arrows move the
/// cursor (for copying text from the screen with the right arrow) until some
/// other key.
static uint8_t rdchar(void) {
    for (;;) {
        uint8_t c = as_rdkey();
        if (c != 0x9b)
            return c;
        for (;;) {
            uint8_t k = as_rdkey();
            uint8_t u = k >= 0xe0 ? (uint8_t)(k - 0x20) : k;
            if (u == 0xc9 || k == 0x8b)  // I, up
                up();
            else if (u == 0xcd || k == 0x8a) {  // M, down
                if (M[Z_CV] + 1 < M[Z_WNDBTM]) {
                    M[Z_CV]++;
                    as_vtab();
                }
            } else if (u == 0xca || k == 0x88)  // J, left
                bs();
            else if (u == 0xcb || k == 0x95) {  // K, right
                M[Z_CH]++;
                if (M[Z_CH] >= width())
                    cr();
            } else if (k == 0x8c) {  // ESC Ctrl-L... (@: home)
                as_home();
            } else if (u == 0xc0) {
                as_home();
            } else if (u == 0xc5) {  // E: clear to end of line
                as_clreol();
            } else if (u == 0xc6) {  // F: clear to end of screen
                as_clreop();
            } else {
                break;
            }
            if (col80)
                M[0x57b] = M[Z_CH];
        }
    }
}

int as_getln(void) {
    as_cout(M[Z_PROMPT]);
    int x = 0;
    for (;;) {
        uint8_t c = rdchar();
        if (c == 0x95)
            c = get_cell(M[Z_CH]);
        M[0x200 + x] = c;
        if (c == 0x8d) {
            as_clreol();
            as_cout(0x8d);
            return x;
        }
        as_cout(c);
        if (c == 0x88) {
            if (x == 0) {
                as_cout(0x8d);
                as_cout(M[Z_PROMPT]);
                continue;
            }
            x--;
            continue;
        }
        if (c == 0x98) {
            as_cout(0xdc);
            as_cout(0x8d);
            as_cout(M[Z_PROMPT]);
            x = 0;
            continue;
        }
        if (x >= 0xf8)
            as_bell();
        if (++x == 256) {
            as_cout(0xdc);
            as_cout(0x8d);
            as_cout(M[Z_PROMPT]);
            x = 0;
        }
    }
}

int as_inlin(uint8_t prompt) {
    M[Z_PROMPT] = prompt | 0x80;
    int x;
    if (dos_reading()) {
        // DOS READ: the line comes from the file, unseen.
        x = 0;
        for (;;) {
            int c = dos_input_char();
            if (c < 0 || (c & 0x7f) == '\r')
                break;
            if (x < 255)
                M[0x200 + x++] = (uint8_t)(c | 0x80);
        }
    } else {
        x = as_getln();
    }
    if (x > 239)
        x = 239;
    M[0x200 + x] = 0;
    for (int i = 0; i < x; ++i)
        M[0x200 + i] &= 0x7f;
    return x;
}

void as_exec_feed(void) {
}
