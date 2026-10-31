/**
 * @file editor.c
 * @brief Implementation of editor.h: full-screen text editor (nano-style keys), drawn with VT100 sequences through stdio so it works on the HDMI console and the serial terminal.
 *
 * A USB mouse places the cursor, drags over text to select it (double-click:
 * a word), scrolls with the wheel and opens a menu with the right button
 * (cut, copy, paste, select all...).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "editor.h"
#include "psram_heap.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "pico/stdlib.h"
#include "hardware/xip_cache.h"
#include "mouse.h"
#include "shell.h"

/// The whole file lives in one gap buffer in PSRAM, with the cut buffer after
/// it. This board's PSRAM has had a few cells that forget after a minute or
/// more untouched, so while editing every 4 KB block is checked against its
/// checksum and rewritten every few seconds (see "Keeping the text safe").
#define EDIT_MAX    (1024 * 1024)
/// The cut buffer's size (the clipboard: cut lines, or a selection cut or copied).
#define CUT_MAX     (256 * 1024)
/// The longest path.
#define PATH_MAX_   128
/// Screen rows.
#define ROWS        30
/// Screen columns.
#define COLS        80
#define TEXT_ROWS   (ROWS - 2)  ///< row 0 title, rows 1..28 text, row 29 status
/// Tab stops every this many columns.
#define TAB_WIDTH   8

/// Keys above the byte range, decoded from escape sequences.
enum {
    KEY_ESC = 0x100, KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_F3,
    KEY_F10,
    KEY_MOUSE,  ///< a mouse event, in mouse_ev
};
/// Alt and a letter: KEY_ALT + the letter (lower case).
#define KEY_ALT 0x200
/// The control-key code of a letter.
#define CTRL(c) ((c) & 0x1f)

// ---- Gap buffer -----------------------------------------------------------

/// The text, with its gap (PSRAM).
static char *buf;
/// The gap is buf[gap_start, gap_end).
///@{
static int gap_start, gap_end;
///@}

// ---- Keeping the text safe in PSRAM -----------------------------------------
/// Each 4 KB block has a checksum, worked out again when the editor changes
/// it. A refresh, a few blocks at a time while waiting for keys, reads each
/// block straight from the chip, checks it against its sum and writes it back
/// (which renews the cells) every REFRESH_MS. A block that changed by itself
/// marks the text damaged: saving then won't overwrite the file.
#define BLOCK      4096
/// Blocks in the text and cut buffer.
#define BLOCKS     ((EDIT_MAX + CUT_MAX) / BLOCK)
/// Every block is checked this often (ms).
#define REFRESH_MS 10000
#ifndef PSRAM_UNCACHED_OFFSET
/// From the PSRAM's cached address to its uncached one.
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)
#endif

static uint32_t *sums;   ///< a sum for each block (SRAM)
static uint8_t *dirty;   ///< changed since its sum was taken
/// The next block to check.
static int refresh_next;
/// When the next check is due.
static absolute_time_t refresh_at;
static int damaged = -1;  ///< the first block found damaged, or -1

/// Marks the blocks under some bytes changed.
/// @param p the bytes
/// @param n their count
static void touch(const char *p, int n) {
    if (n <= 0)
        return;
    int a = p - buf, b = a + n - 1;
    for (int k = a / BLOCK; k <= b / BLOCK; ++k)
        dirty[k] = 1;
}

/// Whether a block is wholly in the gap (nothing to check).
/// @param k the block
/// @return it is
static bool in_gap(int k) {
    return k * BLOCK >= gap_start && (k + 1) * BLOCK <= gap_end;
}

/// One block: from the chip, summed and written back. False if a block the
/// editor hasn't changed no longer matches its sum.
/// @param k the block
/// @return it was all right
static bool refresh_block(int k) {
    volatile uint32_t *w = (volatile uint32_t *)(buf + k * BLOCK + PSRAM_UNCACHED_OFFSET);
    uint32_t sum = 0;
    for (int i = 0; i < BLOCK / 4; ++i) {
        uint32_t v = w[i];
        w[i] = v;
        sum = (sum << 5 | sum >> 27) ^ v;
    }
    if (dirty[k]) {
        sums[k] = sum;
        dirty[k] = 0;
        return true;
    }
    return sum == sums[k];
}

/// Checks (and renews) n blocks, or all of them if n < 0. False if any is damaged.
/// @param n blocks, or -1 for all
/// @return none damaged
static bool refresh(int n) {
    xip_cache_clean_all();  // the editor's writes out to the chip first
    bool ok = true;
    for (int done = 0; done < BLOCKS && (n < 0 || done < n); ++done) {
        int k = refresh_next;
        refresh_next = (refresh_next + 1) % BLOCKS;
        if (k * BLOCK < EDIT_MAX && in_gap(k)) {
            dirty[k] = 1;  // nothing in it; summed again when text moves in
            continue;
        }
        if (!refresh_block(k)) {
            ok = false;
            if (damaged < 0)
                damaged = k;
        }
    }
    return ok;
}

static void set_message(const char *fmt, const char *arg);
static void draw(void);

/// While waiting for keys: a few blocks at a time, every REFRESH_MS.
static void refresh_step(void) {
    if (!time_reached(refresh_at))
        return;
    int was = damaged;
    refresh(16);
    if (refresh_next == 0)
        refresh_at = make_timeout_time_ms(REFRESH_MS);
    if (was < 0 && damaged >= 0) {
        set_message("MEMORY ERROR in the text: save it under a new name (Ctrl-S)", NULL);
        draw();  // now, not at the next key
    }
}

/// The text's length.
/// @return it
static int text_len(void) {
    return EDIT_MAX - (gap_end - gap_start);
}

/// The character at a position.
/// @param i the position
/// @return it
static char at(int i) {
    return i < gap_start ? buf[i] : buf[i + gap_end - gap_start];
}

/// Moves the gap to a position.
/// @param pos the position
static void move_gap(int pos) {
    if (pos < gap_start) {
        int n = gap_start - pos;
        memmove(buf + gap_end - n, buf + pos, n);
        touch(buf + gap_end - n, n);
        gap_start -= n;
        gap_end -= n;
    } else if (pos > gap_start) {
        int n = pos - gap_start;
        memmove(buf + gap_start, buf + gap_end, n);
        touch(buf + gap_start, n);
        gap_start += n;
        gap_end += n;
    }
}

/// Inserts text.
/// @param pos where
/// @param s the text
/// @param n its length
/// @return false if it doesn't fit
static bool insert_text(int pos, const char *s, int n) {
    if (gap_end - gap_start < n)
        return false;
    move_gap(pos);
    memcpy(buf + gap_start, s, n);
    touch(buf + gap_start, n);
    gap_start += n;
    return true;
}

/// Deletes text.
/// @param pos where
/// @param n how much
static void erase_text(int pos, int n) {
    move_gap(pos);
    gap_end += n;
}

// ---- Lines and columns ----------------------------------------------------

/// The start of a position's line.
/// @param p the position
/// @return the line's start
static int line_start(int p) {
    while (p > 0 && at(p - 1) != '\n')
        --p;
    return p;
}

/// The end of a position's line (its newline, or the end).
/// @param p the position
/// @return the line's end
static int line_end(int p) {
    int n = text_len();
    while (p < n && at(p) != '\n')
        ++p;
    return p;
}

/// Start of the next line, or -1 on the last line.
/// @param ls a line's start
/// @return the next line's start, or -1
static int next_line(int ls) {
    int e = line_end(ls);
    return e < text_len() ? e + 1 : -1;
}

/// Start of the previous line, or -1 on the first.
/// @param ls a line's start
/// @return the previous line's start, or -1
static int prev_line(int ls) {
    return ls > 0 ? line_start(ls - 1) : -1;
}

/// The column after a character (tabs to the next stop).
/// @param col the column
/// @param c the character
/// @return the next column
static int advance_col(int col, char c) {
    return c == '\t' ? (col / TAB_WIDTH + 1) * TAB_WIDTH : col + 1;
}

/// Screen column of position p within its line.
/// @param p the position
/// @return its column
static int col_of(int p) {
    int col = 0;
    for (int i = line_start(p); i < p; ++i)
        col = advance_col(col, at(i));
    return col;
}

/// The position on the line starting at ls closest to screen column col.
/// @param ls the line's start
/// @param col the column
/// @return the position
static int pos_at_col(int ls, int col) {
    int e = line_end(ls), c = 0, p = ls;
    while (p < e) {
        int next = advance_col(c, at(p));
        if (next > col)
            break;
        c = next;
        ++p;
    }
    return p;
}

/// A position's line number (from 1).
/// @param p the position
/// @return the line number
static int line_number(int p) {
    int n = 1;
    for (int i = 0; i < p; ++i)
        n += at(i) == '\n';
    return n;
}

// ---- Editor state ---------------------------------------------------------

/// The file's path ("" for a new file).
static char path[PATH_MAX_];
static bool crlf;      ///< the file used CR LF line ends
/// Changed since saved.
static bool modified;
static int cur;        ///< cursor offset
/// The selection's other end (it runs from here to the cursor), or -1.
static int mark;
static int goal_col;   ///< column Up/Down try to keep
static int top;        ///< offset of the first line on screen
static int left;       ///< first screen column shown (horizontal scroll)
/// The status line's message.
static char message[COLS + 1];
/// The last text searched for.
static char find_text[64];
/// The cut buffer (PSRAM).
static char *cut;
/// Its length.
static int cut_len;
/// The last key cut a line (so cuts add up).
static bool last_was_cut;

static char (*shadow)[COLS];  ///< what each screen row shows now (with sums, while editing)
/// For each row: whether shadow is what the screen shows, and the columns
/// shown reversed (the selection) [from, to).
static struct {
    bool ok;               ///< shadow is right
    uint8_t from;          ///< the first reversed column
    uint8_t to;            ///< after the last
} shown[ROWS];
/// The view follows the cursor (not while the wheel has scrolled it away).
static bool follow;
/// The left button is held down over the text: moving selects.
static bool dragging;
/// The last mouse event (KEY_MOUSE).
static mouse_event_t mouse_ev;

/// The selection, if there is one.
/// @param a set to its start
/// @param b set to its end (just after it)
/// @return there is one
static bool selection(int *a, int *b) {
    if (mark < 0 || mark == cur)
        return false;
    *a = mark < cur ? mark : cur;
    *b = mark < cur ? cur : mark;
    return true;
}

/// Sets the status line's message.
/// @param fmt printf's format, one %s
/// @param arg its argument
static void set_message(const char *fmt, const char *arg) {
    snprintf(message, sizeof message, fmt, arg);
}

// ---- Drawing --------------------------------------------------------------

/// Rows are always written whole (80 columns), and only when they change.
/// @param row the screen row
/// @param text its 80 columns
/// @param from the first column in reverse video
/// @param to after the last (from == to: none)
static void put_row(int row, const char *text, int from, int to) {
    if (shown[row].ok && shown[row].from == from && shown[row].to == to &&
        memcmp(shadow[row], text, COLS) == 0)
        return;
    // printf, not fwrite: the SDK sends printf/putchar straight out, but
    // fwrite goes through newlib's buffer and would arrive after later output.
    printf("\x1b[%d;1H%.*s", row + 1, from, text);
    if (to > from)
        printf("\x1b[7m%.*s\x1b[0m", to - from, text + from);
    printf("%.*s", COLS - to, text + to);
    memcpy(shadow[row], text, COLS);
    shown[row].ok = true;
    shown[row].from = from;
    shown[row].to = to;
}

/// Adds the text columns [c0, c1) to a row's reversed span, as far as they are on the screen.
/// @param c0 the first column
/// @param c1 after the last
/// @param from the span's first screen column
/// @param to after its last
static void span_add(int c0, int c1, int *from, int *to) {
    int x0 = (c0 > left ? c0 : left) - left, x1 = (c1 < left + COLS ? c1 : left + COLS) - left;
    if (x0 >= x1)
        return;
    if (*from == *to) {
        *from = x0;
        *to = x1;
    } else {
        *from = x0 < *from ? x0 : *from;
        *to = x1 > *to ? x1 : *to;
    }
}

/// A line as the screen shows it (tabs expanded, scrolled by left), and the
/// selected part of it.
/// @param ls the line's start, or -1 for a blank row
/// @param out 80 columns
/// @param from set to the first selected screen column
/// @param to set to after the last (the same as from: none)
static void render_line(int ls, char *out, int *from, int *to) {
    memset(out, ' ', COLS);
    *from = *to = 0;
    if (ls < 0)
        return;
    int sa, sb;
    bool sel = selection(&sa, &sb);
    int e = line_end(ls), col = 0, p;
    for (p = ls; p < e && col < left + COLS; ++p) {
        char c = at(p);
        int next = advance_col(col, c);
        for (int x = col; x < next; ++x)
            if (x >= left && x < left + COLS)
                out[x - left] = c == '\t' ? ' ' : (unsigned char)c < 0x20 ? '.' : c;
        if (sel && p >= sa && p < sb)
            span_add(col, next, from, to);
        col = next;
    }
    // A selected line end shows as a reversed space after the line.
    if (sel && p == e && e >= sa && e < sb)
        span_add(col, col + 1, from, to);
}

/// Rows between the line starting at `from` and the one starting at `to`
/// (to >= from), stopping counting at `limit`.
/// @param from a line's start
/// @param to a later line's start
/// @param limit where to stop
/// @return the rows
static int rows_between(int from, int to, int limit) {
    int n = 0;
    while (from < to && n < limit) {
        from = next_line(from);
        if (from < 0)
            break;
        ++n;
    }
    return n;
}

/// Scrolls so the cursor is on screen. Small vertical moves use the
/// terminal's scroll region so only the new rows need sending.
static void scroll_to_cursor(void) {
    int ls = line_start(cur);
    int old_top = top, shift = 0;
    if (ls < top) {
        int n = rows_between(ls, top, TEXT_ROWS);
        top = ls;
        shift = -n;
    } else {
        int n = rows_between(top, ls, TEXT_ROWS * 4);
        while (n >= TEXT_ROWS) {
            top = next_line(top);
            --n;
            ++shift;
        }
    }
    if (shift != 0 && top != old_top && abs(shift) < TEXT_ROWS / 2) {
        printf("\x1b[2;%dr\x1b[%d%c\x1b[r", TEXT_ROWS + 1, abs(shift), shift > 0 ? 'S' : 'T');
        if (shift > 0) {
            memmove(shadow[1], shadow[1 + shift], (TEXT_ROWS - shift) * COLS);
            memmove(&shown[1], &shown[1 + shift], (TEXT_ROWS - shift) * sizeof *shown);
            for (int r = TEXT_ROWS + 1 - shift; r <= TEXT_ROWS; ++r)
                shown[r].ok = false;
        } else {
            int s = -shift;
            memmove(shadow[1 + s], shadow[1], (TEXT_ROWS - s) * COLS);
            memmove(&shown[1 + s], &shown[1], (TEXT_ROWS - s) * sizeof *shown);
            for (int r = 1; r <= s; ++r)
                shown[r].ok = false;
        }
    } else if (shift != 0) {
        for (int r = 1; r <= TEXT_ROWS; ++r)
            shown[r].ok = false;
    }

    int col = col_of(cur);
    int old_left = left;
    if (col < left)
        left = col;
    else if (col >= left + COLS)
        left = col - COLS + 1;
    if (left != old_left)
        for (int r = 1; r <= TEXT_ROWS; ++r)
            shown[r].ok = false;
}

/// The menu bar's titles, on the title row.
static const char *const menu_titles[] = {"File", "Edit", "Search"};
/// Their count.
#define NMENUS 3

/// A menu title's column on the title row.
/// @param m the menu
/// @return its first letter's column
static int menu_col(int m) {
    int c = 1;
    for (int i = 0; i < m; ++i)
        c += (int)strlen(menu_titles[i]) + 2;
    return c;
}

/// The menu title at a column of the title row, or -1.
/// @param col the column
/// @return the menu, or -1
static int menu_at(int col) {
    for (int i = 0; i < NMENUS; ++i)
        if (col >= menu_col(i) - 1 && col <= menu_col(i) + (int)strlen(menu_titles[i]))
            return i;
    return -1;
}

/// Draws the title and status bars.
/// @param status the status line's text
static void draw_bars(const char *status) {
    char row[COLS + 1];
    char right[24];
    snprintf(right, sizeof right, "%s ", crlf ? "CRLF" : "LF");
    snprintf(row, sizeof row, " File  Edit  Search   %-.*s%s", COLS - 34, path[0] ? path : "(new file)",
             modified ? "  [modified]" : "");
    int n = strlen(row);
    memset(row + n, ' ', COLS - n);
    memcpy(row + COLS - strlen(right), right, strlen(right));
    put_row(0, row, 0, COLS);

    if (!status) {
        static char st[COLS + 1];
        int sa, sb;
        if (message[0])
            snprintf(st, sizeof st, " %.*s", COLS - 1, message);
        else if (selection(&sa, &sb))
            snprintf(st, sizeof st, " Ln %d, Col %d   %d selected   ^C Copy ^X Cut ^V Paste",
                     line_number(cur), col_of(cur) + 1, sb - sa);
        else
            snprintf(st, sizeof st, " Ln %d, Col %d   ^S Save ^Q Quit ^F Find ^G Go to ^K Cut ^U Paste",
                     line_number(cur), col_of(cur) + 1);
        status = st;
    }
    snprintf(row, sizeof row, "%s", status);
    n = strlen(row);
    memset(row + n, ' ', COLS - n);
    put_row(ROWS - 1, row, 0, COLS);
}

/// Draws what changed on the screen.
static void draw(void) {
    printf("\x1b[?25l");
    if (follow)
        scroll_to_cursor();
    char row[COLS];
    int ls = top, from, to;
    for (int r = 1; r <= TEXT_ROWS; ++r) {
        render_line(ls, row, &from, &to);  // rows past the end of the file come out blank
        put_row(r, row, from, to);
        if (ls >= 0)
            ls = next_line(ls);
    }
    draw_bars(NULL);
    // The cursor, unless the wheel has scrolled it off the screen.
    int cl = line_start(cur), n = cl < top ? TEXT_ROWS : rows_between(top, cl, TEXT_ROWS);
    int x = col_of(cur) - left;
    if (n < TEXT_ROWS && x >= 0 && x < COLS)
        printf("\x1b[%d;%dH\x1b[?25h", 2 + n, x + 1);
    fflush(stdout);
}

// ---- Input ----------------------------------------------------------------

/// A byte read ahead, or -1.
static int pushed_back = -1;

/// A byte from the keyboard or serial port.
/// @param timeout_us how long to wait
/// @return the byte, or PICO_ERROR_TIMEOUT
static int get_byte(uint32_t timeout_us) {
    if (pushed_back >= 0) {
        int c = pushed_back;
        pushed_back = -1;
        return c;
    }
    return getchar_timeout_us(timeout_us);
}

/// True if more input arrives within a few character times at 115200 baud,
/// as in a paste: drawing and auto-indent then wait until it's all in, which
/// also keeps the serial output from swamping the line.
/// @return more is coming
static bool input_waiting(void) {
    if (pushed_back < 0) {
        int c = getchar_timeout_us(2000);
        if (c >= 0)
            pushed_back = c;
    }
    return pushed_back >= 0;
}

/// When the next pretend move is due while dragging at the text's edge.
static absolute_time_t drag_at;

/// While dragging with the pointer on (or past) an edge of the text, a
/// pretend move every 50 ms, so the text scrolls on under a still pointer.
/// @param e set to the move
/// @return one is due
static bool drag_scroll(mouse_event_t *e) {
    int x, y, b;
    mouse_get(&x, &y, &b);
    int row = y / 16, col = x / 8;
    if (!dragging || !(b & MOUSE_LEFT) || !time_reached(drag_at) ||
        !(row < 1 || row > TEXT_ROWS || (col == 0 && left > 0) || col >= COLS - 1))
        return false;
    drag_at = make_timeout_time_ms(50);
    *e = (mouse_event_t){.type = MOUSE_MOVE, .buttons = (uint8_t)b, .x = (int16_t)x, .y = (int16_t)y};
    return true;
}

/// A key, escape sequences decoded, or KEY_MOUSE with the event in mouse_ev
/// (checking blocks while waiting).
/// @return the key
static int read_key(void) {
    int c;
    refresh_step();
    while ((c = get_byte(10000)) == PICO_ERROR_TIMEOUT) {
        refresh_step();
        mouse_pointer_update();
        if (mouse_event(&mouse_ev) || drag_scroll(&mouse_ev))
            return KEY_MOUSE;
    }
    if (c != 0x1b)
        return c;
    // An escape sequence arrives all at once; a lone ESC is the Esc key.
    int c1 = get_byte(50000);
    if (c1 == PICO_ERROR_TIMEOUT)
        return KEY_ESC;
    if (c1 == 'O') {
        int c2 = get_byte(50000);
        return c2 == 'R' ? KEY_F3 : c2 == 'H' ? KEY_HOME : c2 == 'F' ? KEY_END : KEY_ESC;
    }
    if (c1 >= 'a' && c1 <= 'z')
        return KEY_ALT + c1;  // Alt-x arrives as Esc x
    if (c1 >= 'A' && c1 <= 'Z')
        return KEY_ALT + c1 - 'A' + 'a';
    if (c1 != '[')
        return KEY_ESC;
    int param = 0, c2;
    while ((c2 = get_byte(50000)) >= '0' && c2 <= '9')
        param = param * 10 + (c2 - '0');
    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch (param) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 3: return KEY_DEL;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        case 13: return KEY_F3;
        case 21: return KEY_F10;
        }
    }
    return KEY_ESC;
}

/// Reads a line of text on the status bar. Returns false if cancelled.
/// @param label the prompt
/// @param text the text, edited in place
/// @param max room in text
/// @return Enter (else cancelled)
static bool prompt(const char *label, char *text, int max) {
    int n = strlen(text);
    for (;;) {
        char st[COLS + 1];
        snprintf(st, sizeof st, " %s%s", label, text);
        draw_bars(st);
        printf("\x1b[%d;%dH", ROWS, (int)strlen(st) + 1);
        fflush(stdout);
        int k = read_key();
        if (k == '\r' || k == '\n')
            return true;
        if (k == KEY_ESC || k == CTRL('c') || k == CTRL('q'))
            return false;
        if ((k == '\b' || k == 0x7f) && n > 0)
            text[--n] = 0;
        else if (k >= 0x20 && k < 0x100 && k != 0x7f && n < max - 1 &&
                 (int)(strlen(label) + n) < COLS - 3) {
            text[n++] = k;
            text[n] = 0;
        }
    }
}

// ---- File I/O -------------------------------------------------------------

/// Loads path (a new file if it has no name or isn't there).
/// @return loaded
static bool load(void) {
    gap_start = 0;
    gap_end = EDIT_MAX;
    crlf = false;
    if (!path[0]) {
        set_message("New file", NULL);
        return true;
    }
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r == FR_NO_FILE) {
        set_message("New file: it is created when you save (Ctrl-S)", NULL);
        return true;
    }
    if (r != FR_OK) {
        printf("%s: %s\n", path, fresult_name(r));
        return false;
    }
    if (f_size(&f) > EDIT_MAX) {
        printf("%s: too big to edit (%lu bytes; the limit is %d KB)\n", path,
               (unsigned long)f_size(&f), EDIT_MAX / 1024);
        f_close(&f);
        return false;
    }
    UINT got = 0;
    r = f_read(&f, buf, EDIT_MAX, &got);
    f_close(&f);
    if (r != FR_OK) {
        printf("%s: %s\n", path, fresult_name(r));
        return false;
    }
    // Drop the CR of each CR LF, and remember to put them back on save.
    int n = 0;
    for (UINT i = 0; i < got; ++i) {
        if (buf[i] == '\r' && i + 1 < got && buf[i + 1] == '\n') {
            crlf = true;
            continue;
        }
        buf[n++] = buf[i];
    }
    gap_start = n;
    touch(buf, got);
    set_message("Read %.70s", path);
    return true;
}

/// Writes to a temporary file in the same folder first, so an error or a
/// pulled drive can't destroy the original.
/// @return saved
static bool save(void) {
    // Everything checked first: damaged text doesn't replace the file.
    refresh(-1);
    if (damaged >= 0) {
        char name[PATH_MAX_] = "";
        if (!prompt("Memory error in the text! Save it under a NEW name: ", name, sizeof name) ||
            !name[0] || !strcmp(name, path)) {
            set_message("Not saved (it must be a new name)", NULL);
            return false;
        }
        strcpy(path, name);
        damaged = -1;  // saved as it is, under its new name
    }
    if (!path[0]) {
        char name[PATH_MAX_] = "";
        if (!prompt("Save as: ", name, sizeof name) || !name[0]) {
            set_message("Not saved", NULL);
            return false;
        }
        strcpy(path, name);
    }
    char tmp[PATH_MAX_ + 16];
    const char *slash = strrchr(path, '/');
    int dir_len = slash ? slash - path + 1 : 0;
    snprintf(tmp, sizeof tmp, "%.*s~edit.tmp", dir_len, path);

    FIL f;
    FRESULT r = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        set_message("Can't save: %s", fresult_name(r));
        return false;
    }
    char out[512];
    int n = 0, total = text_len(), lines = 0;
    UINT put;
    for (int i = 0; i <= total && r == FR_OK; ++i) {
        if (i < total) {
            char c = at(i);
            if (c == '\n') {
                ++lines;
                if (crlf)
                    out[n++] = '\r';
            }
            out[n++] = c;
        }
        if (n >= (int)sizeof out - 1 || (i == total && n > 0)) {
            r = f_write(&f, out, n, &put);
            if (r == FR_OK && put != (UINT)n)
                r = FR_DENIED;  // drive full
            n = 0;
        }
    }
    FRESULT rc = f_close(&f);
    if (r == FR_OK)
        r = rc;
    if (r == FR_OK) {
        FRESULT ru = f_unlink(path);
        if (ru != FR_OK && ru != FR_NO_FILE)
            r = ru;
    }
    if (r == FR_OK)
        r = f_rename(tmp, path);
    if (r != FR_OK) {
        f_unlink(tmp);
        set_message(r == FR_DENIED ? "Can't save: drive full or read-only" : "Can't save: %s",
                    fresult_name(r));
        return false;
    }
    modified = false;
    snprintf(message, sizeof message, "Saved %s: %d lines, %d bytes", path, lines,
             total + (crlf ? lines : 0));
    return true;
}

// ---- Editing commands -----------------------------------------------------

/// Inserts text at the cursor.
/// @param s the text
/// @param n its length
static void insert_chars(const char *s, int n) {
    if (!insert_text(cur, s, n)) {
        set_message("The file is full (%s limit)", "1 MB");
        return;
    }
    cur += n;
    modified = true;
}

/// Enter: a new line, indented as the one before.
static void new_line(void) {
    // Keep the current line's indentation (not for pasted text, which brings
    // its own).
    char indent[COLS + 1];
    int ls = line_start(cur), n = 1;
    indent[0] = '\n';
    bool pasting = input_waiting();
    for (int p = ls; p < cur && n < COLS && (at(p) == ' ' || at(p) == '\t') && !pasting; ++p)
        indent[n++] = at(p);
    insert_chars(indent, n);
}

/// Deletes the selection (if any).
static void delete_selection(void) {
    int a, b;
    if (!selection(&a, &b))
        return;
    erase_text(a, b - a);
    cur = a;
    mark = -1;
    modified = true;
}

/// Copies the selection to the cut buffer (in place of what was there).
/// @return copied (false: nothing selected, or too much)
static bool copy_selection(void) {
    int a, b;
    if (!selection(&a, &b)) {
        set_message("Nothing selected: drag over the text with the mouse, or Ctrl-A for all", NULL);
        return false;
    }
    if (b - a > CUT_MAX) {
        set_message("Too much for the cut buffer (%s)", "256 KB");
        return false;
    }
    touch(cut, b - a);
    for (int p = a; p < b; ++p)
        cut[p - a] = at(p);
    cut_len = b - a;
    snprintf(message, sizeof message, "%d characters in the cut buffer", cut_len);
    return true;
}

/// Ctrl-K: cuts the line (cuts in a row add up).
static void cut_line(void) {
    int ls = line_start(cur), next = next_line(ls);
    int end = next >= 0 ? next : text_len();
    if (end == ls) {
        set_message("Nothing to cut", NULL);
        return;
    }
    if (!last_was_cut)
        cut_len = 0;
    if (cut_len + (end - ls) > CUT_MAX) {
        set_message("Cut buffer full (%s)", "256 KB");
        return;
    }
    touch(cut + cut_len, end - ls);
    for (int p = ls; p < end; ++p)
        cut[cut_len++] = at(p);
    erase_text(ls, end - ls);
    cur = ls;
    modified = true;
}

/// Finds the next find_text after the cursor (wrapping round).
static void find_next(void) {
    int n = strlen(find_text), total = text_len();
    if (!n)
        return;
    for (int k = 1; k <= total; ++k) {
        int p = (cur + k) % total;
        if (p + n > total)
            continue;
        int i = 0;
        while (i < n && at(p + i) == find_text[i])
            ++i;
        if (i == n) {
            if (p <= cur)
                set_message("Search wrapped", NULL);
            cur = p;
            return;
        }
    }
    set_message("Not found: %s", find_text);
}

/// Moves the cursor to a line.
/// @param n the line (from 1)
static void go_to_line(int n) {
    int p = 0;
    while (--n > 0) {
        int next = next_line(p);
        if (next < 0)
            break;
        p = next;
    }
    cur = p;
}

/// Moves the cursor up or down lines, keeping its column.
/// @param lines how many (- up)
static void move_vertical(int lines) {
    int ls = line_start(cur);
    for (; lines < 0; ++lines) {
        int p = prev_line(ls);
        if (p < 0)
            break;
        ls = p;
    }
    for (; lines > 0; --lines) {
        int p = next_line(ls);
        if (p < 0)
            break;
        ls = p;
    }
    cur = pos_at_col(ls, goal_col);
}

/// Returns false when the editor should close.
/// @param k the key
/// @return keep editing
static bool handle_key(int k) {
    static int prev_key;
    int prev = prev_key;
    prev_key = k;
    bool keep_goal = false, keep_mark = false, was_cut = last_was_cut;
    int sa, sb;
    bool sel = selection(&sa, &sb);
    last_was_cut = false;
    message[0] = 0;
    switch (k) {
    case KEY_UP: move_vertical(-1); keep_goal = true; break;
    case KEY_DOWN: move_vertical(1); keep_goal = true; break;
    case KEY_PGUP: move_vertical(-(TEXT_ROWS - 1)); keep_goal = true; break;
    case KEY_PGDN: move_vertical(TEXT_ROWS - 1); keep_goal = true; break;
    case KEY_LEFT:
        if (cur > 0)
            --cur;
        break;
    case KEY_RIGHT:
        if (cur < text_len())
            ++cur;
        break;
    case KEY_HOME: cur = line_start(cur); break;
    case KEY_END: cur = line_end(cur); break;
    case KEY_DEL:
        if (sel) {
            delete_selection();
        } else if (cur < text_len()) {
            erase_text(cur, 1);
            modified = true;
        }
        break;
    case '\b':
    case 0x7f:
        if (sel) {
            delete_selection();
        } else if (cur > 0) {
            erase_text(--cur, 1);
            modified = true;
        }
        break;
    case '\n':
        if (prev == '\r')  // the second half of a pasted CR LF
            break;
        // fall through
    case '\r':
        delete_selection();
        new_line();
        break;
    case '\t': {
        char t = '\t';
        delete_selection();
        insert_chars(&t, 1);
        break;
    }
    case CTRL('s'):
        save();
        keep_mark = true;
        break;
    case CTRL('q'):
        if (modified) {
            for (;;) {
                draw_bars(" Save changes? (y)es, (n)o, Esc to keep editing");
                fflush(stdout);
                int a = read_key();
                if (a == 'y' || a == 'Y') {
                    if (save())
                        return false;
                    break;
                }
                if (a == 'n' || a == 'N')
                    return false;
                if (a == KEY_ESC || a == CTRL('c'))
                    break;
            }
            break;
        }
        return false;
    case CTRL('f'): {
        char text[sizeof find_text];
        strcpy(text, find_text);
        if (prompt("Find: ", text, sizeof text) && text[0]) {
            strcpy(find_text, text);
            find_next();
        }
        break;
    }
    case KEY_F3:
        if (find_text[0])
            find_next();
        else
            set_message("Nothing to find yet: use Ctrl-F", NULL);
        break;
    case CTRL('g'): {
        char text[12] = "";
        if (prompt("Go to line: ", text, sizeof text) && atoi(text) > 0)
            go_to_line(atoi(text));
        break;
    }
    case CTRL('k'):
        if (sel) {  // the selection, if there is one
            if (copy_selection())
                delete_selection();
            break;
        }
        last_was_cut = was_cut;
        cut_line();
        last_was_cut = true;
        break;
    case CTRL('x'):
        if (copy_selection())
            delete_selection();
        break;
    case CTRL('c'):
        copy_selection();
        keep_mark = true;
        break;
    case CTRL('u'):
    case CTRL('v'):
        if (cut_len) {
            delete_selection();
            insert_chars(cut, cut_len);
        } else {
            set_message("Nothing to paste: cut or copy something first", NULL);
            keep_mark = true;
        }
        break;
    case CTRL('a'):
        mark = 0;
        cur = text_len();
        keep_mark = true;
        break;
    case CTRL('l'):
        printf("\x1b[2J");
        memset(shown, 0, sizeof shown);
        keep_mark = true;
        break;
    default:
        if (k >= 0x20 && k < 0x100 && k != 0x7f) {
            char c = k;
            delete_selection();
            insert_chars(&c, 1);
        }
    }
    if (!keep_mark)
        mark = -1;
    if (!keep_goal)
        goal_col = col_of(cur);
    return true;
}

// ---- The mouse ------------------------------------------------------------

/// The text position under a screen cell. Above or below the text it is on
/// the line just past that edge, so dragging there scrolls; while dragging,
/// the first and last columns scroll sideways too.
/// @param row the screen row
/// @param col the screen column
/// @param drag the left button is being dragged
/// @return the position
static int pos_at_cell(int row, int col, bool drag) {
    int ls = top;
    if (row < 1) {
        int p = prev_line(top);
        if (p >= 0)
            ls = p;
    }
    for (int r = 1; r < row && r <= TEXT_ROWS; ++r) {
        int n = next_line(ls);
        if (n < 0)
            break;
        ls = n;
    }
    int c = left + (col < 0 ? 0 : col);
    if (drag && col <= 0 && left > 0)
        c = left > 4 ? left - 4 : 0;
    else if (drag && col >= COLS - 1)
        c = left + COLS + 3;
    return pos_at_col(ls, c);
}

/// What kind of character it is, for a double-click's word: letters and
/// digits, spaces, a line end, or other marks.
/// @param c the character
/// @return its kind
static int char_kind(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
        (unsigned char)c >= 0x80)
        return 0;
    return c == ' ' || c == '\t' ? 1 : c == '\n' ? 2 : 3;
}

/// Selects the word (or run of spaces or marks) at a position.
/// @param p the position
static void select_word(int p) {
    int n = text_len();
    cur = mark = p;
    if (p >= n || at(p) == '\n')
        return;
    int kind = char_kind(at(p)), a = p, b = p;
    while (a > 0 && char_kind(at(a - 1)) == kind)
        --a;
    while (b < n && char_kind(at(b)) == kind)
        ++b;
    mark = a;
    cur = b;
}

/// An item in a menu.
typedef struct {
    const char *text;  ///< its name ("-": a line between items)
    const char *keys;  ///< the keys that do the same
    int key;           ///< the key it runs
} popup_item_t;

/// A line between items.
#define SEPARATOR {"-", NULL, 0}

/// The right button's menu.
static const popup_item_t popup_items[] = {
    {"Cut", "Ctrl-X", CTRL('x')},  {"Copy", "Ctrl-C", CTRL('c')},
    {"Paste", "Ctrl-V", CTRL('v')}, {"Delete", "Del", KEY_DEL},
    SEPARATOR,                     {"Select all", "Ctrl-A", CTRL('a')},
    SEPARATOR,                     {"Find...", "Ctrl-F", CTRL('f')},
    {"Find next", "F3", KEY_F3},   {"Go to line...", "Ctrl-G", CTRL('g')},
    SEPARATOR,                     {"Save", "Ctrl-S", CTRL('s')},
    {"Quit", "Ctrl-Q", CTRL('q')},
};
/// The File menu.
static const popup_item_t file_items[] = {
    {"Save", "Ctrl-S", CTRL('s')}, {"Redraw screen", "Ctrl-L", CTRL('l')},
    SEPARATOR,                     {"Quit", "Ctrl-Q", CTRL('q')},
};
/// The Edit menu.
static const popup_item_t edit_items[] = {
    {"Cut", "Ctrl-X", CTRL('x')},  {"Copy", "Ctrl-C", CTRL('c')},
    {"Paste", "Ctrl-V", CTRL('v')}, {"Delete", "Del", KEY_DEL},
    SEPARATOR,                     {"Select all", "Ctrl-A", CTRL('a')},
    SEPARATOR,                     {"Cut line", "Ctrl-K", CTRL('k')},
};
/// The Search menu.
static const popup_item_t search_items[] = {
    {"Find...", "Ctrl-F", CTRL('f')}, {"Find next", "F3", KEY_F3},
    {"Go to line...", "Ctrl-G", CTRL('g')},
};
/// The items in an array of them, and their count.
#define ITEMS(a) a, (int)(sizeof a / sizeof *a)
/// The menu bar's menus, in menu_titles' order.
static const struct {
    const popup_item_t *items;  ///< its items
    int n;                      ///< their count
} menus[NMENUS] = {{ITEMS(file_items)}, {ITEMS(edit_items)}, {ITEMS(search_items)}};

/// Whether a menu item can be chosen now (Cut wants a selection, and so on).
/// @param it the item
/// @return it can
static bool popup_enabled(const popup_item_t *it) {
    int a, b;
    switch (it->key) {
    case 0: return false;
    case CTRL('x'): case CTRL('c'): case KEY_DEL: return selection(&a, &b);
    case CTRL('v'): return cut_len > 0;
    case KEY_F3: return find_text[0] != 0;
    }
    return true;
}

/// Draws a menu (box lines in the DEC line-drawing set, which the console
/// and serial terminals both have).
/// @param items its items
/// @param n their count
/// @param row its top row
/// @param col its left column
/// @param w its width
/// @param sel the item highlighted, or -1
static void popup_draw(const popup_item_t *items, int n, int row, int col, int w, int sel) {
    printf("\x1b[0;30;47m\x1b[%d;%dH\x1b(0l", row + 1, col + 1);
    for (int x = 2; x < w; ++x)
        putchar('q');
    printf("k\x1b(B");
    for (int i = 0; i < n; ++i) {
        const popup_item_t *it = &items[i];
        printf("\x1b[%d;%dH", row + 2 + i, col + 1);
        if (it->text[0] == '-') {
            printf("\x1b(0t");
            for (int x = 2; x < w; ++x)
                putchar('q');
            printf("u\x1b(B");
            continue;
        }
        const char *colour = i == sel ? "\x1b[97;44m" : popup_enabled(it) ? "" : "\x1b[90m";
        printf("\x1b(0x\x1b(B%s %-*s%s \x1b[0;30;47m\x1b(0x\x1b(B", colour, w - 4 - (int)strlen(it->keys),
               it->text, it->keys);
    }
    printf("\x1b[%d;%dH\x1b(0m", row + n + 2, col + 1);
    for (int x = 2; x < w; ++x)
        putchar('q');
    printf("j\x1b(B\x1b[0m");
    fflush(stdout);
}

/// What run_menu did besides choosing an item: go to the menu bar's menu
/// on the left or right, or to another one the pointer went to.
enum { MENU_STAY = -1 };

/// Runs a menu with its top-left corner at a cell; returns the key of the
/// item chosen (by the keys, a click, or letting go of the button held over
/// it), or 0. A menu-bar menu (bar >= 0, the menu's number) also goes to
/// another one with Left / Right or the pointer on its title: *next is set
/// to it (else MENU_STAY).
/// @param items its items
/// @param n their count
/// @param row its top row
/// @param col its left column
/// @param bar its number on the menu bar, or -1 for the pop-up
/// @param next set to the menu to go to next, or MENU_STAY
/// @return the item's key, or 0
static int run_menu(const popup_item_t *items, int n, int row, int col, int bar, int *next) {
    int w = 0;
    for (int i = 0; i < n; ++i)
        if (items[i].keys) {
            int len = (int)strlen(items[i].text) + (int)strlen(items[i].keys) + 2;
            if (len > w)
                w = len;
        }
    w += 4;
    int h = n + 2;
    if (row + h > ROWS - 1)
        row = ROWS - 1 - h;
    if (row < 1)
        row = 1;
    if (col + w > COLS)
        col = COLS - w;
    int sel = -1, key = 0;
    bool moved = false;  // the pointer has moved since the menu opened
    *next = MENU_STAY;
    printf("\x1b[?25l");
    for (;;) {
        popup_draw(items, n, row, col, w, sel);
        int k = read_key();
        if (k == KEY_MOUSE) {
            const mouse_event_t *e = &mouse_ev;
            int i = e->y / 16 - row - 1, c = e->x / 8;
            bool inside = c > col && c < col + w - 1 && i >= 0 && i < n;
            bool ok = inside && popup_enabled(&items[i]);
            int title = bar >= 0 && e->y / 16 == 0 ? menu_at(c) : -1;
            if (e->type == MOUSE_MOVE) {
                moved = true;
                sel = ok ? i : -1;
                if (title >= 0 && title != bar) {
                    *next = title;  // the pointer went to another title
                    break;
                }
            } else if (ok && (e->type == MOUSE_DOWN || (e->type == MOUSE_UP && moved))) {
                key = items[i].key;
                break;
            } else if (e->type == MOUSE_DOWN && title >= 0 && title != bar) {
                *next = title;
                break;
            } else if (e->type == MOUSE_DOWN && !inside) {
                break;  // a click outside (or on its own title) closes it
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c') || k == CTRL('q') || k == KEY_F10)
            break;
        if (k == '\r' && sel >= 0) {
            key = items[sel].key;
            break;
        }
        if (bar >= 0 && (k == KEY_LEFT || k == KEY_RIGHT)) {
            *next = (bar + (k == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS;
            break;
        }
        if (bar >= 0 && k >= KEY_ALT) {  // Alt and a title's first letter
            for (int m = 0; m < NMENUS; ++m)
                if (k == KEY_ALT + (menu_titles[m][0] | 0x20) && m != bar)
                    *next = m;
            if (*next != MENU_STAY)
                break;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int d = k == KEY_UP ? n - 1 : 1;
            if (sel < 0)
                sel = k == KEY_UP ? 0 : n - 1;
            do
                sel = (sel + d) % n;
            while (!popup_enabled(&items[sel]));
        }
    }
    for (int r = row; r < row + h; ++r)
        shown[r].ok = false;
    return key;
}

/// Shows the pop-up menu below a cell (see run_menu).
/// @param row the row clicked
/// @param col the column
/// @return the item's key, or 0
static int popup(int row, int col) {
    int next;
    return run_menu(ITEMS(popup_items), row + 1, col, -1, &next);
}

/// Opens the menu bar at menu m, going from menu to menu until an item is
/// chosen or the menus close; returns the item's key, or 0.
/// @param m the menu
/// @return the key, or 0
static int bar_menu(int m) {
    for (;;) {
        // Its title highlighted on the title row.
        printf("\x1b[1;%dH\x1b[97;44m %s \x1b[0m", menu_col(m), menu_titles[m]);
        shown[0].ok = false;
        int next, key = run_menu(menus[m].items, menus[m].n, 1, menu_col(m) - 1, m, &next);
        if (next == MENU_STAY)
            return key;
        draw();  // the menu gone from under the next one
        m = next;
    }
}

/// A mouse event: the left button places the cursor and drags a selection
/// (a double-click selects a word), the wheel scrolls, the right button
/// opens the menu. Returns a key to run (from the menu), or 0.
/// @param e the event
/// @return the key, or 0
static int mouse_action(const mouse_event_t *e) {
    int row = e->y / 16, col = e->x / 8;
    switch (e->type) {
    case MOUSE_WHEEL:  // the view moves; the cursor stays where it is
        for (int n = e->wheel * 3; n > 0; --n) {
            int p = prev_line(top);
            if (p < 0)
                break;
            top = p;
        }
        for (int n = -e->wheel * 3; n > 0; --n) {
            int p = next_line(top);
            if (p < 0)
                break;
            top = p;
        }
        follow = false;
        return 0;
    case MOUSE_MOVE:
        if (dragging && (e->buttons & MOUSE_LEFT)) {
            cur = pos_at_cell(row, col, true);
            follow = true;
        }
        break;
    case MOUSE_UP:
        if (e->button == MOUSE_LEFT)
            dragging = false;
        return 0;
    case MOUSE_DOWN:
        message[0] = 0;
        last_was_cut = false;
        if (row == 0 && menu_at(col) >= 0)
            return bar_menu(menu_at(col));
        if (row < 1 || row > TEXT_ROWS)
            return 0;
        int p = pos_at_cell(row, col, false), a, b;
        follow = true;
        if (e->button == MOUSE_LEFT) {
            if (e->double_click) {
                select_word(p);
            } else {
                cur = mark = p;
                dragging = true;
                drag_at = make_timeout_time_ms(200);
            }
        } else if (e->button == MOUSE_RIGHT) {
            // Outside the selection, the click moves the cursor first.
            if (!selection(&a, &b) || p < a || p > b)
                cur = p, mark = -1;
            goal_col = col_of(cur);
            draw();
            return popup(row, col);
        }
        break;
    }
    goal_col = col_of(cur);
    return 0;
}

void editor_run(const char *name) {
    // The text and the cut buffer in PSRAM (the SRAM heap is small), kept
    // fresh by the scrubber while editing: this board's PSRAM has had a few
    // cells that forget after a minute or more untouched.
    buf = psram_malloc(EDIT_MAX + CUT_MAX);
    sums = buf ? malloc(BLOCKS * sizeof *sums + BLOCKS + ROWS * COLS) : NULL;
    if (!buf || !sums) {
        printf("edit: not enough memory for the editor\n");
        psram_free(buf);
        return;
    }
    cut = buf + EDIT_MAX;
    dirty = (uint8_t *)(sums + BLOCKS);
    memset(dirty, 1, BLOCKS);
    shadow = (char (*)[COLS])(dirty + BLOCKS);
    memset(shown, 0, sizeof shown);
    refresh_next = 0;
    refresh_at = make_timeout_time_ms(REFRESH_MS);
    damaged = -1;
    snprintf(path, sizeof path, "%s", name ? name : "");
    message[0] = 0;
    modified = false;
    cur = top = left = goal_col = 0;
    mark = -1;
    follow = true;
    dragging = false;
    cut_len = 0;
    last_was_cut = false;
    if (load()) {
        int pointer = mouse_pointer_mode();
        mouse_pointer(MOUSE_POINTER_TEXT);  // the cell under it reversed
        mouse_flush();
        printf("\x1b[0m\x1b[2J");
        memset(shown, 0, sizeof shown);
        bool pending = true;  // something to draw
        for (;;) {
            if (pending && !input_waiting()) {
                draw();
                pending = false;
            }
            int k = read_key();
            if (k == KEY_MOUSE) {
                if (mouse_ev.type == MOUSE_MOVE && !dragging)
                    continue;  // the pointer just moving: nothing changes
                pending = true;
                if (!(k = mouse_action(&mouse_ev)))
                    continue;
            }
            // F10, or Alt and a menu's first letter: the menu bar.
            for (int m = 0; m < NMENUS && k >= KEY_F10; ++m)
                if (k == KEY_F10 ? m == 0 : k == KEY_ALT + (menu_titles[m][0] | 0x20)) {
                    pending = true;
                    k = bar_menu(m);
                    break;
                }
            if (!k)
                continue;
            pending = true;
            follow = true;
            if (!handle_key(k))
                break;
        }
        mouse_pointer(pointer);
        printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
        fflush(stdout);
    }
    psram_free(buf);
    free(sums);
    buf = cut = NULL;
    sums = NULL;
}
