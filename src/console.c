/**
 * @file console.c
 * @brief Implementation of console.h: 80x30 text console drawn into the DVI framebuffer with an 8x16 font.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvi.h"
#include "font8x16.h"
#include "mouse.h"
#include "pico/time.h"
#include "psram_heap.h"

#define GLYPH_W 8
#define GLYPH_H 16
#define TEXT_ROW_BYTES (DVI_WIDTH * GLYPH_H)
#define CURSOR_BLINK_MS 500

#define DEFAULT_FG RGB332(6, 6, 3)
#define DEFAULT_BG RGB332(0, 0, 0)

/// The 16 ANSI colours: 0-7 normal, 8-15 bright.
static const uint8_t palette[16] = {
    RGB332(0, 0, 0), RGB332(5, 0, 0), RGB332(0, 5, 0), RGB332(5, 5, 0),
    RGB332(0, 0, 2), RGB332(5, 0, 2), RGB332(0, 5, 2), RGB332(5, 5, 2),
    RGB332(2, 2, 1), RGB332(7, 1, 1), RGB332(1, 7, 1), RGB332(7, 7, 1),
    RGB332(2, 2, 3), RGB332(7, 2, 3), RGB332(2, 7, 3), RGB332(7, 7, 3),
};

/// Current colours and attributes. fg_idx is the ANSI colour number behind fg
/// (-1 if fg isn't one), so bold can brighten it.
static uint8_t fg = DEFAULT_FG;
static uint8_t bg = DEFAULT_BG;
static int fg_idx = -1;
static bool bold, reverse;

static int cur_col, cur_row;
/// VT100 wrapping: writing the last column leaves the cursor there, and the
/// line only wraps when the next character arrives.
static bool wrap_pending;
static bool autowrap = true;
static int scroll_top, scroll_bottom = CONSOLE_ROWS - 1;  ///< scroll region, inclusive

static struct {
    ///@{
    int col, row;
    ///@}
    ///@{
    uint8_t fg, bg;
    ///@}
    int fg_idx;
    ///@{
    bool bold, reverse;
    ///@}
} saved;

/// Escape sequence parser.
enum { ST_NORMAL, ST_ESC, ST_CSI, ST_SKIP_ONE, ST_STRING, ST_STRING_ESC, ST_G0, ST_G1 };
static int esc_state;
#define MAX_PARAMS 8
static int params[MAX_PARAMS];
static int nparams;
static bool private_mode;  ///< CSI ? (or > < =) ...: modes, or xterm extras we ignore

/// xterm-style extras for remote sessions (ssh, telnet): DEC line drawing
/// characters (ESC ( 0 for G0, ESC ) 0 for G1, SO/SI to switch), cursor keys
/// in application mode (ESC [ ? 1 h), the alternate screen (ESC [ ? 1049 h)
/// and answers to status queries.
static bool g0_lines, g1_lines, shift_out;
static bool app_cursor;
static void (*reply_fn)(const char *text, int len);
static struct alt_screen {
    ///@{
    uint8_t ch[CONSOLE_ROWS][CONSOLE_COLS], attr[CONSOLE_ROWS][CONSOLE_COLS];
    ///@}
    ///@{
    int col, row;
    ///@}
} *alt_saved;  ///< the main screen while the alternate one is up

/// The blink timer runs in an IRQ, so it skips its turn while a write is
/// drawing; writes hide the cursor first and show it again when done.
static volatile bool busy;
static bool cursor_shown;
static bool cursor_enabled = true;  ///< ESC [ ? 25 h / l
static repeating_timer_t blink_timer;

/// What each cell shows: the character, and its colours as a PC attribute
/// byte (foreground + 16 * background), for BASIC's SCREEN() and PEEK.
static uint8_t cell_ch[CONSOLE_ROWS][CONSOLE_COLS];
static uint8_t cell_attr[CONSOLE_ROWS][CONSOLE_COLS];

/// The PC colour number (0-15) nearest an RGB332 colour.
static uint8_t pc_colour(uint8_t rgb) {
    static const uint8_t ansi_to_pc[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 16; ++i) {
        uint8_t p = palette[i];
        int dr = (p >> 5) - (rgb >> 5), dg = ((p >> 2) & 7) - ((rgb >> 2) & 7), db = (p & 3) - (rgb & 3);
        int d = dr * dr + dg * dg + 4 * db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return ansi_to_pc[best & 7] | (best & 8);
}

static uint8_t attr_of(uint8_t f, uint8_t b) {
    return pc_colour(f) | pc_colour(b) << 4;
}

static void draw_glyph(int col, int row, uint8_t ch) {
    const uint8_t *glyph = font8x16[ch];
    uint8_t f = reverse ? bg : fg, b = reverse ? fg : bg;
    cell_ch[row][col] = ch;
    cell_attr[row][col] = attr_of(f, b);
    uint8_t *dst = &dvi_framebuf[row * TEXT_ROW_BYTES + col * GLYPH_W];
    for (int y = 0; y < GLYPH_H; ++y, dst += DVI_WIDTH) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < GLYPH_W; ++x)
            dst[x] = (bits & (0x80 >> x)) ? f : b;
    }
}

/// The cursor is a two-pixel underline, toggled by XOR so it restores itself.
static void toggle_cursor(void) {
    uint8_t mask = fg ^ bg;
    if (!mask)
        mask = 0xff;
    uint8_t *dst = &dvi_framebuf[cur_row * TEXT_ROW_BYTES + (GLYPH_H - 2) * DVI_WIDTH +
                                 cur_col * GLYPH_W];
    for (int y = 0; y < 2; ++y, dst += DVI_WIDTH)
        for (int x = 0; x < GLYPH_W; ++x)
            dst[x] ^= mask;
    cursor_shown = !cursor_shown;
}

static bool blink_callback(repeating_timer_t *t) {
    (void)t;
    if (!busy && (cursor_enabled || cursor_shown))
        toggle_cursor();
    return true;
}

/// The mouse pointer in text mode (mouse.h): a timer draws the cell under it
/// with its colours swapped. Writes that move pixels around (scrolling) put
/// the cell back first; the timer shows it again.
#define POINTER_MS 20
static int pointer_row = -1, pointer_col;
static repeating_timer_t pointer_timer;

static void paint_cell(int row, int col, bool swapped) {
    static const uint8_t pc_to_ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    uint8_t a = cell_attr[row][col];
    uint8_t f = palette[pc_to_ansi[a & 7] | (a & 8)];
    uint8_t b = palette[pc_to_ansi[(a >> 4) & 7] | ((a >> 4) & 8)];
    if (swapped) {
        uint8_t t = f;
        f = b == t ? (uint8_t)~t : b;
        b = t;
    }
    const uint8_t *glyph = font8x16[cell_ch[row][col]];
    uint8_t *dst = &dvi_framebuf[row * TEXT_ROW_BYTES + col * GLYPH_W];
    for (int y = 0; y < GLYPH_H; ++y, dst += DVI_WIDTH)
        for (int x = 0; x < GLYPH_W; ++x)
            dst[x] = (glyph[y] & (0x80 >> x)) ? f : b;
}

/// Inside a write (busy, the cursor hidden): the pointer's cell back to normal.
static void pointer_off(void) {
    if (pointer_row < 0)
        return;
    paint_cell(pointer_row, pointer_col, false);
    pointer_row = -1;
}

void console_freeze(bool on) {
    busy = on;
}

void console_pointer_off(void) {
    bool was = busy;
    busy = true;
    bool cursor_there = cursor_shown && pointer_row == cur_row && pointer_col == cur_col;
    if (cursor_there)
        toggle_cursor();
    pointer_off();
    if (cursor_there)
        toggle_cursor();
    busy = was;
}

static bool pointer_callback(repeating_timer_t *t) {
    (void)t;
    if (busy)
        return true;
    int row = -1, col = 0;
    if (mouse_pointer_mode() == MOUSE_POINTER_TEXT && mouse_count() && !dvi_line_double) {
        int x, y, b;
        mouse_get(&x, &y, &b);
        row = y / GLYPH_H;
        col = x / GLYPH_W;
    }
    if (row == pointer_row && col == pointer_col && row < 0)
        return true;
    // The cursor's underline is drawn by XOR: keep it out of the way.
    bool cursor_there = cursor_shown && ((row == cur_row && col == cur_col) ||
                                         (pointer_row == cur_row && pointer_col == cur_col));
    if (cursor_there)
        toggle_cursor();
    if (pointer_row >= 0 && (pointer_row != row || pointer_col != col))
        paint_cell(pointer_row, pointer_col, false);
    if (row >= 0)
        paint_cell(row, col, true);  // again each time: a write may have drawn over it
    pointer_row = row;
    pointer_col = col;
    if (cursor_there)
        toggle_cursor();
    return true;
}

// ---- Screen operations, in character cells ---------------------------------

/// Blanks columns col0..col1-1 of one row with the background colour.
static void clear_cells(int row, int col0, int col1) {
    if (col1 <= col0)
        return;
    memset(&cell_ch[row][col0], ' ', col1 - col0);
    memset(&cell_attr[row][col0], attr_of(fg, bg), col1 - col0);
    uint8_t *dst = &dvi_framebuf[row * TEXT_ROW_BYTES + col0 * GLYPH_W];
    for (int y = 0; y < GLYPH_H; ++y, dst += DVI_WIDTH)
        memset(dst, bg, (col1 - col0) * GLYPH_W);
}

static void clear_rows(int row0, int row1) {
    if (row1 <= row0)
        return;
    memset(dvi_framebuf + row0 * TEXT_ROW_BYTES, bg, (row1 - row0) * TEXT_ROW_BYTES);
    memset(cell_ch[row0], ' ', (row1 - row0) * CONSOLE_COLS);
    memset(cell_attr[row0], attr_of(fg, bg), (row1 - row0) * CONSOLE_COLS);
}

/// Moves rows top..bottom (inclusive) up by n, blanking the n rows at the bottom.
static void scroll_up(int top, int bottom, int n) {
    int rows = bottom - top + 1;
    if (n > rows)
        n = rows;
    memmove(dvi_framebuf + top * TEXT_ROW_BYTES, dvi_framebuf + (top + n) * TEXT_ROW_BYTES,
            (rows - n) * TEXT_ROW_BYTES);
    memmove(cell_ch[top], cell_ch[top + n], (rows - n) * CONSOLE_COLS);
    memmove(cell_attr[top], cell_attr[top + n], (rows - n) * CONSOLE_COLS);
    clear_rows(bottom + 1 - n, bottom + 1);
}

static void scroll_down(int top, int bottom, int n) {
    int rows = bottom - top + 1;
    if (n > rows)
        n = rows;
    memmove(dvi_framebuf + (top + n) * TEXT_ROW_BYTES, dvi_framebuf + top * TEXT_ROW_BYTES,
            (rows - n) * TEXT_ROW_BYTES);
    memmove(cell_ch[top + n], cell_ch[top], (rows - n) * CONSOLE_COLS);
    memmove(cell_attr[top + n], cell_attr[top], (rows - n) * CONSOLE_COLS);
    clear_rows(top, top + n);
}

/// Inserts (n > 0) or deletes (n < 0) blank cells at the cursor, shifting the
/// rest of the row right or left.
static void shift_cells(int n) {
    int count = CONSOLE_COLS - cur_col;
    if (n > count)
        n = count;
    if (n < -count)
        n = -count;
    uint8_t *row = &dvi_framebuf[cur_row * TEXT_ROW_BYTES + cur_col * GLYPH_W];
    int keep = (count - (n > 0 ? n : -n)) * GLYPH_W;
    for (int y = 0; y < GLYPH_H; ++y, row += DVI_WIDTH) {
        if (n > 0)
            memmove(row + n * GLYPH_W, row, keep);
        else
            memmove(row, row - n * GLYPH_W, keep);
    }
    int kc = keep / GLYPH_W;
    uint8_t *ch = &cell_ch[cur_row][cur_col], *at = &cell_attr[cur_row][cur_col];
    if (n > 0) {
        memmove(ch + n, ch, kc);
        memmove(at + n, at, kc);
    } else {
        memmove(ch, ch - n, kc);
        memmove(at, at - n, kc);
    }
    if (n > 0)
        clear_cells(cur_row, cur_col, cur_col + n);
    else
        clear_cells(cur_row, CONSOLE_COLS + n, CONSOLE_COLS);
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void move_to(int row, int col) {
    cur_row = clamp(row, 0, CONSOLE_ROWS - 1);
    cur_col = clamp(col, 0, CONSOLE_COLS - 1);
    wrap_pending = false;
}

/// Down one row, scrolling if the cursor is on the bottom of the scroll region.
static void line_feed(void) {
    wrap_pending = false;
    if (cur_row == scroll_bottom)
        scroll_up(scroll_top, scroll_bottom, 1);
    else if (cur_row < CONSOLE_ROWS - 1)
        ++cur_row;
}

static void reverse_index(void) {
    wrap_pending = false;
    if (cur_row == scroll_top)
        scroll_down(scroll_top, scroll_bottom, 1);
    else if (cur_row > 0)
        --cur_row;
}

static void reset_attributes(void) {
    fg = DEFAULT_FG;
    bg = DEFAULT_BG;
    fg_idx = -1;
    bold = reverse = false;
}

static void alt_screen(bool on);

static void reset_terminal(void) {
    alt_screen(false);
    g0_lines = g1_lines = shift_out = app_cursor = false;
    reset_attributes();
    scroll_top = 0;
    scroll_bottom = CONSOLE_ROWS - 1;
    autowrap = true;
    cursor_enabled = true;
    clear_rows(0, CONSOLE_ROWS);
    move_to(0, 0);
    saved.col = saved.row = 0;
    saved.fg = fg;
    saved.bg = bg;
    saved.fg_idx = -1;
    saved.bold = saved.reverse = false;
}

static void set_fg_index(int idx) {
    fg_idx = idx;
    fg = palette[idx < 8 && bold ? idx + 8 : idx];
}

// ---- Control sequences ----------------------------------------------------

/// SGR: ESC [ ... m
static void select_graphic_rendition(void) {
    if (nparams == 0)
        nparams = 1;  // ESC [ m means ESC [ 0 m
    for (int i = 0; i < nparams; ++i) {
        int p = params[i];
        if (p == 0) {
            reset_attributes();
        } else if (p == 1) {
            bold = true;
            if (fg_idx >= 0 && fg_idx < 8)
                set_fg_index(fg_idx);
        } else if (p == 22) {
            bold = false;
            if (fg_idx >= 0 && fg_idx < 8)
                set_fg_index(fg_idx);
        } else if (p == 7) {
            reverse = true;
        } else if (p == 27) {
            reverse = false;
        } else if (p >= 30 && p <= 37) {
            set_fg_index(p - 30);
        } else if (p == 39) {
            fg = DEFAULT_FG;
            fg_idx = -1;
        } else if (p >= 40 && p <= 47) {
            bg = palette[p - 40];
        } else if (p == 49) {
            bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            set_fg_index(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            bg = palette[p - 100 + 8];
        }
    }
}

static void set_mode(bool on) {
    for (int i = 0; i < nparams; ++i) {
        if (!private_mode)
            continue;
        if (params[i] == 25)
            cursor_enabled = on;
        else if (params[i] == 7)
            autowrap = on;
        else if (params[i] == 1)
            app_cursor = on;
        else if (params[i] == 1049 || params[i] == 1047 || params[i] == 47)
            alt_screen(on);
    }
}

static void reply(const char *text) {
    if (reply_fn)
        reply_fn(text, strlen(text));
}

static void csi_dispatch(char final) {
    if (private_mode && final != 'h' && final != 'l')
        return;  // e.g. xterm's ESC [ > 4 ; 2 m, which isn't colour
    int p0 = nparams > 0 ? params[0] : 0;
    int p1 = nparams > 1 ? params[1] : 0;
    int n = p0 ? p0 : 1;  // count parameters default to 1
    switch (final) {
    case 'A': move_to(cur_row - n, cur_col); break;      // CUU
    case 'B': move_to(cur_row + n, cur_col); break;      // CUD
    case 'C': move_to(cur_row, cur_col + n); break;      // CUF
    case 'D': move_to(cur_row, cur_col - n); break;      // CUB
    case 'E': move_to(cur_row + n, 0); break;            // CNL
    case 'F': move_to(cur_row - n, 0); break;            // CPL
    case 'G': move_to(cur_row, n - 1); break;            // CHA
    case 'd': move_to(n - 1, cur_col); break;            // VPA
    case 'H':                                            // CUP
    case 'f': move_to((p0 ? p0 : 1) - 1, (p1 ? p1 : 1) - 1); break;
    case 'J':  // ED: 0 cursor to end, 1 start to cursor, 2 (or 3) whole screen
        if (p0 == 0) {
            clear_cells(cur_row, cur_col, CONSOLE_COLS);
            clear_rows(cur_row + 1, CONSOLE_ROWS);
        } else if (p0 == 1) {
            clear_rows(0, cur_row);
            clear_cells(cur_row, 0, cur_col + 1);
        } else {
            clear_rows(0, CONSOLE_ROWS);
        }
        break;
    case 'K':  // EL: 0 cursor to end, 1 start to cursor, 2 whole line
        if (p0 == 0)
            clear_cells(cur_row, cur_col, CONSOLE_COLS);
        else if (p0 == 1)
            clear_cells(cur_row, 0, cur_col + 1);
        else
            clear_cells(cur_row, 0, CONSOLE_COLS);
        break;
    case 'L':  // IL: insert lines at the cursor, inside the scroll region
        if (cur_row >= scroll_top && cur_row <= scroll_bottom)
            scroll_down(cur_row, scroll_bottom, n);
        cur_col = 0;
        wrap_pending = false;
        break;
    case 'M':  // DL: delete lines at the cursor
        if (cur_row >= scroll_top && cur_row <= scroll_bottom)
            scroll_up(cur_row, scroll_bottom, n);
        cur_col = 0;
        wrap_pending = false;
        break;
    case '@': shift_cells(n); wrap_pending = false; break;   // ICH
    case 'P': shift_cells(-n); wrap_pending = false; break;  // DCH
    case 'X':                                                // ECH
        clear_cells(cur_row, cur_col, clamp(cur_col + n, 0, CONSOLE_COLS));
        wrap_pending = false;
        break;
    case 'S': scroll_up(scroll_top, scroll_bottom, n); break;    // SU
    case 'T': scroll_down(scroll_top, scroll_bottom, n); break;  // SD
    case 'm': select_graphic_rendition(); break;
    case 'r': {  // DECSTBM: scroll region, then home
        int top = (p0 ? p0 : 1) - 1;
        int bottom = (p1 ? p1 : CONSOLE_ROWS) - 1;
        if (top < bottom && bottom < CONSOLE_ROWS) {
            scroll_top = top;
            scroll_bottom = bottom;
        }
        move_to(0, 0);
        break;
    }
    case 's':  // save cursor (SCO)
        saved.col = cur_col;
        saved.row = cur_row;
        break;
    case 'u':  // restore cursor (SCO)
        move_to(saved.row, saved.col);
        break;
    case 'h': set_mode(true); break;
    case 'l': set_mode(false); break;
    case 'n':  // DSR: status, or where the cursor is
        if (p0 == 5) {
            reply("\x1b[0n");
        } else if (p0 == 6) {
            char r[32];
            snprintf(r, sizeof r, "\x1b[%d;%dR", cur_row + 1, cur_col + 1);
            reply(r);
        }
        break;
    case 'c':  // DA: a VT100 with advanced video
        if (p0 == 0)
            reply("\x1b[?1;2c");
        break;
    }
}

static void put_char(uint8_t c) {
    if (wrap_pending) {
        cur_col = 0;
        line_feed();
    }
    draw_glyph(cur_col, cur_row, c);
    if (cur_col < CONSOLE_COLS - 1)
        ++cur_col;
    else if (autowrap)
        wrap_pending = true;
}

static void put_raw(char ch) {
    uint8_t c = (uint8_t)ch;

    switch (esc_state) {
    case ST_ESC:
        esc_state = ST_NORMAL;
        switch (c) {
        case '[':
            esc_state = ST_CSI;
            nparams = 0;
            params[0] = 0;
            private_mode = false;
            break;
        case '7':  // DECSC: save cursor and attributes
            saved.col = cur_col;
            saved.row = cur_row;
            saved.fg = fg;
            saved.bg = bg;
            saved.fg_idx = fg_idx;
            saved.bold = bold;
            saved.reverse = reverse;
            break;
        case '8':  // DECRC
            move_to(saved.row, saved.col);
            fg = saved.fg;
            bg = saved.bg;
            fg_idx = saved.fg_idx;
            bold = saved.bold;
            reverse = saved.reverse;
            break;
        case 'D': line_feed(); break;  // IND
        case 'M': reverse_index(); break;  // RI
        case 'E':                          // NEL
            cur_col = 0;
            line_feed();
            break;
        case 'c': reset_terminal(); break;  // RIS
        case '(': esc_state = ST_G0; break;  // character sets
        case ')': esc_state = ST_G1; break;
        case '#': esc_state = ST_SKIP_ONE; break;  // DEC tests: ignored
        case ']':  // OSC (window titles), DCS, APC, PM: skipped to the end
        case 'P':
        case '_':
        case '^':
        case 'X': esc_state = ST_STRING; break;
        }
        return;
    case ST_G0:
    case ST_G1:
        if (esc_state == ST_G0)
            g0_lines = c == '0';
        else
            g1_lines = c == '0';
        esc_state = ST_NORMAL;
        return;
    case ST_STRING:  // ends with BEL or ESC \ (ST)
        if (c == 7)
            esc_state = ST_NORMAL;
        else if (c == 0x1b)
            esc_state = ST_STRING_ESC;
        return;
    case ST_STRING_ESC:
        esc_state = c == '\\' ? ST_NORMAL : ST_STRING;
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (nparams == 0)
                nparams = 1;
            int *p = &params[nparams - 1];
            if (*p < 10000)
                *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (nparams == 0)
                nparams = 1;  // ESC [ ; 5 H: the first parameter is empty
            if (nparams < MAX_PARAMS)
                params[nparams++] = 0;
        } else if (c == '?' || c == '>' || c == '<' || c == '=') {
            private_mode = true;
        } else if (c >= 0x40 && c <= 0x7e) {
            esc_state = ST_NORMAL;
            csi_dispatch(c);
        } else if (c == 0x1b) {
            esc_state = ST_ESC;  // a new sequence cancels this one
        } else if (c < 0x20) {
            // C0 controls are obeyed even inside a sequence.
            esc_state = ST_CSI;
            goto control;
        }
        return;
    case ST_SKIP_ONE:
        esc_state = ST_NORMAL;
        return;
    }

control:
    switch (c) {
    case 0x1b:
        esc_state = ST_ESC;
        break;
    case '\n':
        cur_col = 0;
        line_feed();
        break;
    case '\v':
        line_feed();
        break;
    case '\r':
        cur_col = 0;
        wrap_pending = false;
        break;
    case '\b':
        wrap_pending = false;
        if (cur_col > 0)
            --cur_col;
        break;
    case '\t':
        move_to(cur_row, (cur_col / 8 + 1) * 8);
        break;
    case '\f':
        clear_rows(0, CONSOLE_ROWS);
        move_to(0, 0);
        break;
    case 0x0e:  // SO: G1
        shift_out = true;
        break;
    case 0x0f:  // SI: G0
        shift_out = false;
        break;
    default:
        if (c >= 0x5f && c <= 0x7e && (shift_out ? g1_lines : g0_lines)) {
            // DEC special graphics, as code page 437
            static const uint8_t lines[] = {
                ' ',  0x04, 0xb1, ' ',  ' ',  ' ',  ' ',  0xf8, 0xf1, ' ',  ' ',  0xd9,
                0xbf, 0xda, 0xc0, 0xc5, 0xc4, 0xc4, 0xc4, 0xc4, 0xc4, 0xc3, 0xb4, 0xc1,
                0xc2, 0xb3, 0xf3, 0xf2, 0xe3, '#',  0x9c, 0xfa};
            put_char(lines[c - 0x5f]);
        } else if (c >= 0x20) {
            put_char(c);
        }
        break;
    }
}

/// Draws a cell from a PC attribute byte (inside a write: no cursor work).
static void draw_cell(int row, int col, uint8_t ch, uint8_t attr) {
    static const uint8_t pc_to_ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    uint8_t f = fg, b = bg;
    bool r = reverse;
    fg = palette[pc_to_ansi[attr & 7] | (attr & 8)];
    bg = palette[pc_to_ansi[(attr >> 4) & 7] | ((attr >> 4) & 8)];
    reverse = false;
    draw_glyph(col, row, ch);
    fg = f;
    bg = b;
    reverse = r;
}

/// ESC [ ? 1049 h / l: full-screen programs draw on a blank screen and the
/// old one comes back when they finish.
static void alt_screen(bool on) {
    if (on && !alt_saved) {
        alt_saved = psram_malloc(sizeof *alt_saved);  // (only while a program has it up)
        if (!alt_saved)
            return;
        memcpy(alt_saved->ch, cell_ch, sizeof cell_ch);
        memcpy(alt_saved->attr, cell_attr, sizeof cell_attr);
        alt_saved->col = cur_col;
        alt_saved->row = cur_row;
        clear_rows(0, CONSOLE_ROWS);
    } else if (!on && alt_saved) {
        for (int r = 0; r < CONSOLE_ROWS; ++r)
            for (int c = 0; c < CONSOLE_COLS; ++c)
                draw_cell(r, c, alt_saved->ch[r][c], alt_saved->attr[r][c]);
        move_to(alt_saved->row, alt_saved->col);
        psram_free(alt_saved);
        alt_saved = NULL;
    }
}

void console_set_reply(void (*fn)(const char *text, int len)) {
    reply_fn = fn;
}

bool console_app_cursor(void) {
    return app_cursor;
}

void console_reset_modes(void) {
    busy = true;
    bool was_shown = cursor_shown;
    if (was_shown)
        toggle_cursor();
    pointer_off();
    alt_screen(false);
    g0_lines = g1_lines = shift_out = app_cursor = false;
    esc_state = ST_NORMAL;
    reset_attributes();
    scroll_top = 0;
    scroll_bottom = CONSOLE_ROWS - 1;
    autowrap = true;
    cursor_enabled = true;
    if (was_shown)
        toggle_cursor();
    busy = false;
}

void console_put_glyph(uint8_t c) {
    busy = true;
    bool was_shown = cursor_shown;
    if (was_shown)
        toggle_cursor();
    pointer_off();
    put_char(c);
    if (was_shown && cursor_enabled)
        toggle_cursor();
    busy = false;
}

void console_get_cursor(int *row, int *col) {
    *row = cur_row;
    *col = cur_col;
}

void console_get_scroll_region(int *top, int *bottom) {
    *top = scroll_top;
    *bottom = scroll_bottom;
}

void console_get_cell(int row, int col, uint8_t *ch, uint8_t *attr) {
    if (row < 0 || row >= CONSOLE_ROWS || col < 0 || col >= CONSOLE_COLS) {
        *ch = ' ';
        *attr = 0;
        return;
    }
    *ch = cell_ch[row][col];
    *attr = cell_attr[row][col];
}

void console_set_cell(int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= CONSOLE_ROWS || col < 0 || col >= CONSOLE_COLS)
        return;
    busy = true;
    bool was_shown = cursor_shown;
    if (was_shown)
        toggle_cursor();
    draw_cell(row, col, ch, attr);
    if (was_shown && cursor_enabled)
        toggle_cursor();
    busy = false;
}

void console_write(const char *buf, int len) {
    busy = true;
    bool was_shown = cursor_shown;
    if (was_shown)
        toggle_cursor();
    pointer_off();
    for (int i = 0; i < len; ++i)
        put_raw(buf[i]);
    if (was_shown && cursor_enabled)
        toggle_cursor();
    busy = false;
}

void console_putc(char c) {
    console_write(&c, 1);
}

void console_clear(void) {
    console_putc('\f');
}

void console_set_color(uint8_t new_fg, uint8_t new_bg) {
    busy = true;
    bool was_shown = cursor_shown;
    if (was_shown)
        toggle_cursor();  // hide with the old colours so XOR stays balanced
    fg = new_fg;
    bg = new_bg;
    fg_idx = -1;
    if (was_shown)
        toggle_cursor();
    busy = false;
}

void console_init(void) {
    memset(dvi_framebuf, bg, sizeof dvi_framebuf);
    memset(cell_ch, ' ', sizeof cell_ch);
    memset(cell_attr, attr_of(fg, bg), sizeof cell_attr);
    cur_col = cur_row = 0;
    add_repeating_timer_ms(CURSOR_BLINK_MS, blink_callback, NULL, &blink_timer);
    add_repeating_timer_ms(POINTER_MS, pointer_callback, NULL, &pointer_timer);
}

static void stdio_out_chars(const char *buf, int len) {
    console_write(buf, len);
}

stdio_driver_t console_stdio_driver = {
    .out_chars = stdio_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};
