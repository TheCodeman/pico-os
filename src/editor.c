/**
 * @file editor.c
 * @brief Implementation of editor.h: full-screen text editor (nano-style keys), drawn with VT100 sequences through stdio so it works on the HDMI console and the serial terminal.
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
#include "shell.h"

/// The whole file lives in one gap buffer in PSRAM, with the cut buffer after
/// it. This board's PSRAM has had a few cells that forget after a minute or
/// more untouched, so while editing every 4 KB block is checked against its
/// checksum and rewritten every few seconds (see "Keeping the text safe").
#define EDIT_MAX    (1024 * 1024)
#define CUT_MAX     (8 * 1024)
#define PATH_MAX_   128
#define ROWS        30
#define COLS        80
#define TEXT_ROWS   (ROWS - 2)  ///< row 0 title, rows 1..28 text, row 29 status
#define TAB_WIDTH   8

/// Keys above the byte range, decoded from escape sequences.
enum {
    KEY_ESC = 0x100, KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_F3,
};
#define CTRL(c) ((c) & 0x1f)

// ---- Gap buffer -----------------------------------------------------------

static char *buf;
static int gap_start, gap_end;  ///< the gap is buf[gap_start, gap_end)

// ---- Keeping the text safe in PSRAM -----------------------------------------
/// Each 4 KB block has a checksum, worked out again when the editor changes
/// it. A refresh, a few blocks at a time while waiting for keys, reads each
/// block straight from the chip, checks it against its sum and writes it back
/// (which renews the cells) every REFRESH_MS. A block that changed by itself
/// marks the text damaged: saving then won't overwrite the file.
#define BLOCK      4096
#define BLOCKS     ((EDIT_MAX + CUT_MAX) / BLOCK)
#define REFRESH_MS 10000
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)

static uint32_t *sums;   ///< a sum for each block (SRAM)
static uint8_t *dirty;   ///< changed since its sum was taken
static int refresh_next;
static absolute_time_t refresh_at;
static int damaged = -1;  ///< the first block found damaged, or -1

static void touch(const char *p, int n) {
    if (n <= 0)
        return;
    int a = p - buf, b = a + n - 1;
    for (int k = a / BLOCK; k <= b / BLOCK; ++k)
        dirty[k] = 1;
}

static bool in_gap(int k) {
    return k * BLOCK >= gap_start && (k + 1) * BLOCK <= gap_end;
}

/// One block: from the chip, summed and written back. False if a block the
/// editor hasn't changed no longer matches its sum.
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

static int text_len(void) {
    return EDIT_MAX - (gap_end - gap_start);
}

static char at(int i) {
    return i < gap_start ? buf[i] : buf[i + gap_end - gap_start];
}

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

static bool insert_text(int pos, const char *s, int n) {
    if (gap_end - gap_start < n)
        return false;
    move_gap(pos);
    memcpy(buf + gap_start, s, n);
    touch(buf + gap_start, n);
    gap_start += n;
    return true;
}

static void erase_text(int pos, int n) {
    move_gap(pos);
    gap_end += n;
}

// ---- Lines and columns ----------------------------------------------------

static int line_start(int p) {
    while (p > 0 && at(p - 1) != '\n')
        --p;
    return p;
}

static int line_end(int p) {
    int n = text_len();
    while (p < n && at(p) != '\n')
        ++p;
    return p;
}

/// Start of the next line, or -1 on the last line.
static int next_line(int ls) {
    int e = line_end(ls);
    return e < text_len() ? e + 1 : -1;
}

/// Start of the previous line, or -1 on the first.
static int prev_line(int ls) {
    return ls > 0 ? line_start(ls - 1) : -1;
}

static int advance_col(int col, char c) {
    return c == '\t' ? (col / TAB_WIDTH + 1) * TAB_WIDTH : col + 1;
}

/// Screen column of position p within its line.
static int col_of(int p) {
    int col = 0;
    for (int i = line_start(p); i < p; ++i)
        col = advance_col(col, at(i));
    return col;
}

/// The position on the line starting at ls closest to screen column col.
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

static int line_number(int p) {
    int n = 1;
    for (int i = 0; i < p; ++i)
        n += at(i) == '\n';
    return n;
}

// ---- Editor state ---------------------------------------------------------

static char path[PATH_MAX_];
static bool crlf;      ///< the file used CR LF line ends
static bool modified;
static int cur;        ///< cursor offset
static int goal_col;   ///< column Up/Down try to keep
static int top;        ///< offset of the first line on screen
static int left;       ///< first screen column shown (horizontal scroll)
static char message[COLS + 1];
static char find_text[64];
static char *cut;
static int cut_len;
static bool last_was_cut;

static char (*shadow)[COLS];  ///< what each screen row shows now (with sums, while editing)
static bool shadow_ok[ROWS];

static void set_message(const char *fmt, const char *arg) {
    snprintf(message, sizeof message, fmt, arg);
}

// ---- Drawing --------------------------------------------------------------

/// Rows are always written whole (80 columns), and only when they change.
static void put_row(int row, const char *text, bool reverse) {
    if (shadow_ok[row] && memcmp(shadow[row], text, COLS) == 0)
        return;
    printf("\x1b[%d;1H%s", row + 1, reverse ? "\x1b[7m" : "");
    // printf, not fwrite: the SDK sends printf/putchar straight out, but
    // fwrite goes through newlib's buffer and would arrive after later output.
    printf("%.*s", COLS, text);
    if (reverse)
        printf("\x1b[0m");
    memcpy(shadow[row], text, COLS);
    shadow_ok[row] = true;
}

static void render_line(int ls, char *out) {
    memset(out, ' ', COLS);
    if (ls < 0)
        return;
    int e = line_end(ls), col = 0;
    for (int p = ls; p < e && col < left + COLS; ++p) {
        char c = at(p);
        int next = advance_col(col, c);
        for (int x = col; x < next; ++x)
            if (x >= left && x < left + COLS)
                out[x - left] = c == '\t' ? ' ' : (unsigned char)c < 0x20 ? '.' : c;
        col = next;
    }
}

/// Rows between the line starting at `from` and the one starting at `to`
/// (to >= from), stopping counting at `limit`.
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
            memmove(&shadow_ok[1], &shadow_ok[1 + shift], TEXT_ROWS - shift);
            for (int r = TEXT_ROWS + 1 - shift; r <= TEXT_ROWS; ++r)
                shadow_ok[r] = false;
        } else {
            int s = -shift;
            memmove(shadow[1 + s], shadow[1], (TEXT_ROWS - s) * COLS);
            memmove(&shadow_ok[1 + s], &shadow_ok[1], TEXT_ROWS - s);
            for (int r = 1; r <= s; ++r)
                shadow_ok[r] = false;
        }
    } else if (shift != 0) {
        for (int r = 1; r <= TEXT_ROWS; ++r)
            shadow_ok[r] = false;
    }

    int col = col_of(cur);
    int old_left = left;
    if (col < left)
        left = col;
    else if (col >= left + COLS)
        left = col - COLS + 1;
    if (left != old_left)
        for (int r = 1; r <= TEXT_ROWS; ++r)
            shadow_ok[r] = false;
}

static void draw_bars(const char *status) {
    char row[COLS + 1];
    char right[24];
    snprintf(right, sizeof right, "%s ", crlf ? "CRLF" : "LF");
    snprintf(row, sizeof row, " Edit: %-.*s%s", COLS - 20, path[0] ? path : "(new file)",
             modified ? "  [modified]" : "");
    int n = strlen(row);
    memset(row + n, ' ', COLS - n);
    memcpy(row + COLS - strlen(right), right, strlen(right));
    put_row(0, row, true);

    if (!status) {
        static char st[COLS + 1];
        if (message[0])
            snprintf(st, sizeof st, " %.*s", COLS - 1, message);
        else
            snprintf(st, sizeof st, " Ln %d, Col %d   ^S Save ^Q Quit ^F Find ^G Go to ^K Cut ^U Paste",
                     line_number(cur), col_of(cur) + 1);
        status = st;
    }
    snprintf(row, sizeof row, "%s", status);
    n = strlen(row);
    memset(row + n, ' ', COLS - n);
    put_row(ROWS - 1, row, true);
}

static void draw(void) {
    printf("\x1b[?25l");
    scroll_to_cursor();
    char row[COLS];
    int ls = top;
    for (int r = 1; r <= TEXT_ROWS; ++r) {
        render_line(ls, row);  // rows past the end of the file come out blank
        put_row(r, row, false);
        if (ls >= 0)
            ls = next_line(ls);
    }
    draw_bars(NULL);
    int screen_row = 2 + rows_between(top, line_start(cur), TEXT_ROWS);
    printf("\x1b[%d;%dH\x1b[?25h", screen_row, col_of(cur) - left + 1);
    fflush(stdout);
}

// ---- Input ----------------------------------------------------------------

static int pushed_back = -1;

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
static bool input_waiting(void) {
    if (pushed_back < 0) {
        int c = getchar_timeout_us(2000);
        if (c >= 0)
            pushed_back = c;
    }
    return pushed_back >= 0;
}

static int read_key(void) {
    int c;
    refresh_step();
    while ((c = get_byte(100000)) == PICO_ERROR_TIMEOUT)
        refresh_step();
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
        }
    }
    return KEY_ESC;
}

/// Reads a line of text on the status bar. Returns false if cancelled.
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

static void insert_chars(const char *s, int n) {
    if (!insert_text(cur, s, n)) {
        set_message("The file is full (%s limit)", "64 KB");
        return;
    }
    cur += n;
    modified = true;
}

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
        set_message("Cut buffer full (%s)", "8 KB");
        return;
    }
    touch(cut + cut_len, end - ls);
    for (int p = ls; p < end; ++p)
        cut[cut_len++] = at(p);
    erase_text(ls, end - ls);
    cur = ls;
    modified = true;
}

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
static bool handle_key(int k) {
    static int prev_key;
    int prev = prev_key;
    prev_key = k;
    bool keep_goal = false, was_cut = last_was_cut;
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
        if (cur < text_len()) {
            erase_text(cur, 1);
            modified = true;
        }
        break;
    case '\b':
    case 0x7f:
        if (cur > 0) {
            erase_text(--cur, 1);
            modified = true;
        }
        break;
    case '\n':
        if (prev == '\r')  // the second half of a pasted CR LF
            break;
        // fall through
    case '\r':
        new_line();
        break;
    case '\t': {
        char t = '\t';
        insert_chars(&t, 1);
        break;
    }
    case CTRL('s'):
        save();
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
        last_was_cut = was_cut;
        cut_line();
        last_was_cut = true;
        break;
    case CTRL('u'):
        if (cut_len)
            insert_chars(cut, cut_len);
        break;
    case CTRL('l'):
        printf("\x1b[2J");
        memset(shadow_ok, 0, sizeof shadow_ok);
        break;
    default:
        if (k >= 0x20 && k < 0x100 && k != 0x7f) {
            char c = k;
            insert_chars(&c, 1);
        }
    }
    if (!keep_goal)
        goal_col = col_of(cur);
    return true;
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
    memset(shadow_ok, 0, sizeof shadow_ok);
    refresh_next = 0;
    refresh_at = make_timeout_time_ms(REFRESH_MS);
    damaged = -1;
    snprintf(path, sizeof path, "%s", name ? name : "");
    message[0] = 0;
    modified = false;
    cur = top = left = goal_col = 0;
    cut_len = 0;
    last_was_cut = false;
    if (load()) {
        printf("\x1b[0m\x1b[2J");
        memset(shadow_ok, 0, sizeof shadow_ok);
        do
            if (!input_waiting())
                draw();
        while (handle_key(read_key()));
        printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
        fflush(stdout);
    }
    psram_free(buf);
    free(sums);
    buf = cut = NULL;
    sums = NULL;
}
