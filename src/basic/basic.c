/**
 * @file basic.c
 * @brief GW-BASIC style interpreter.
 *
 * Program lines are kept as text (keywords upper-cased when entered) in a
 * sorted array and interpreted directly, so LIST, SAVE and EDIT show exactly
 * what was typed. Expression results that are strings are temporaries, freed
 * after every statement (and on errors); variables hold their own copies.
 * Errors unwind with longjmp to the prompt or the program's caller.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "basic.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "basic_port.h"
#include "ff.h"
#include "font8x16.h"
#include "image.h"

/// All of BASIC's memory comes from the port (on the Pico: PSRAM for big
/// blocks, SRAM for small ones).
#define malloc(n)     port_alloc(n)
#define calloc(n, sz) port_calloc(n, sz)
#define realloc(p, n) port_realloc(p, n)
#define free(p)       port_release(p)

#define LINE_MAX_LEN   255
#define STR_MAX        255
#define NAME_MAX       40
/// Program text, variables, strings and arrays all come from the heap. A
/// program line is refused if it would leave less than DATA_RESERVE for the
/// program's data, and any allocation if it would leave less than
/// HEAP_MARGIN (the SDK's malloc panics rather than failing).
#define DATA_RESERVE   (16 * 1024)
#define HEAP_MARGIN    (4 * 1024)
#define STACK_MAX      32           ///< FOR, GOSUB and WHILE frames each
#define SCREEN_COLS    80
#define SCREEN_ROWS    30
#define PAGE_LINES     28

// ---- Errors ---------------------------------------------------------------

enum {
    E_NEXT_WITHOUT_FOR = 1, E_SYNTAX = 2, E_RETURN_WITHOUT_GOSUB = 3, E_OUT_OF_DATA = 4,
    E_ILLEGAL_CALL = 5, E_OVERFLOW = 6, E_OUT_OF_MEMORY = 7, E_UNDEFINED_LINE = 8,
    E_SUBSCRIPT = 9, E_DUPLICATE_DEF = 10, E_DIV_ZERO = 11, E_ILLEGAL_DIRECT = 12,
    E_TYPE_MISMATCH = 13, E_STRING_TOO_LONG = 15, E_CANT_CONTINUE = 17,
    E_UNDEFINED_FN = 18, E_NO_RESUME = 19, E_RESUME_WITHOUT_ERROR = 20, E_FOR_WITHOUT_NEXT = 26, E_WHILE_WITHOUT_WEND = 29,
    E_WEND_WITHOUT_WHILE = 30, E_FIELD_OVERFLOW = 50, E_BAD_FILE_NUMBER = 52,
    E_FILE_NOT_FOUND = 53, E_BAD_FILE_MODE = 54, E_FILE_ALREADY_OPEN = 55,
    E_INPUT_PAST_END = 62, E_BAD_RECORD_NUMBER = 63, E_BAD_FILE_NAME = 64,
    E_TOO_MANY_FILES = 67, E_DEVICE_UNAVAILABLE = 68, E_DEVICE_IO = 57, E_FILE_EXISTS = 58, E_DISK_FULL = 61,
    E_PERMISSION = 70, E_DISK_NOT_READY = 71, E_PATH_ACCESS = 75, E_PATH_NOT_FOUND = 76, E_PROTECTED = 100, E_BREAK = 1000,
};

static const struct {
    int code;
    const char *text;
} error_texts[] = {
    {E_NEXT_WITHOUT_FOR, "NEXT without FOR"}, {E_SYNTAX, "Syntax error"},
    {E_RETURN_WITHOUT_GOSUB, "RETURN without GOSUB"}, {E_OUT_OF_DATA, "Out of DATA"},
    {E_ILLEGAL_CALL, "Illegal function call"}, {E_OVERFLOW, "Overflow"},
    {E_OUT_OF_MEMORY, "Out of memory"}, {E_UNDEFINED_LINE, "Undefined line number"},
    {E_SUBSCRIPT, "Subscript out of range"}, {E_DUPLICATE_DEF, "Duplicate Definition"},
    {E_DIV_ZERO, "Division by zero"}, {E_ILLEGAL_DIRECT, "Illegal direct"},
    {E_TYPE_MISMATCH, "Type mismatch"}, {E_STRING_TOO_LONG, "String too long"},
    {E_CANT_CONTINUE, "Can't continue"}, {E_UNDEFINED_FN, "Undefined user function"},
    {E_FOR_WITHOUT_NEXT, "FOR without NEXT"}, {E_WHILE_WITHOUT_WEND, "WHILE without WEND"},
    {E_WEND_WITHOUT_WHILE, "WEND without WHILE"}, {E_BAD_FILE_NAME, "Bad file name"},
    {E_FILE_NOT_FOUND, "File not found"}, {E_BAD_FILE_MODE, "Bad file mode"},
    {E_DEVICE_IO, "Device I/O error"}, {E_FILE_EXISTS, "File already exists"},
    {E_DISK_FULL, "Disk full"}, {E_PERMISSION, "Permission denied"},
    {E_DISK_NOT_READY, "Disk not ready"}, {E_PROTECTED, "Protected file"},
    {E_FIELD_OVERFLOW, "FIELD overflow"}, {E_BAD_FILE_NUMBER, "Bad file number"},
    {E_FILE_ALREADY_OPEN, "File already open"}, {E_INPUT_PAST_END, "Input past end"},
    {E_BAD_RECORD_NUMBER, "Bad record number"}, {E_TOO_MANY_FILES, "Too many files"},
    {E_DEVICE_UNAVAILABLE, "Device unavailable"}, {14, "Out of string space"},
    {16, "String formula too complex"}, {E_NO_RESUME, "No RESUME"},
    {E_RESUME_WITHOUT_ERROR, "RESUME without error"}, {22, "Missing operand"},
    {23, "Line buffer overflow"}, {24, "Device Timeout"}, {25, "Device Fault"},
    {27, "Out of paper"}, {51, "Internal error"}, {66, "Direct statement in file"},
    {69, "Communication buffer overflow"}, {72, "Disk Media Error"},
    {73, "Advanced Feature"}, {74, "Rename across disks"}, {E_PATH_ACCESS, "Path/File Access Error"},
    {E_PATH_NOT_FOUND, "Path not found"},
};

static jmp_buf *error_jump_p;  ///< (basic_ram, while BASIC runs)
#define error_jump (*error_jump_p)

/// Data files opened with OPEN (the code is further down).
#define MAX_FILES     4
#define MAX_FIELDS    16
#define MAX_RECLEN    1024

enum { FM_INPUT = 1, FM_OUTPUT, FM_APPEND, FM_RANDOM, FM_COM };

typedef struct {
    FIL f;
    int mode;
    int col;              ///< output column, for PRINT # commas and TAB
    int peek;             ///< a character read ahead; -1 at the end, -2 none
    bool past_end;        ///< the last GET was past the end of the file
    uint16_t reclen;
    uint32_t record;      ///< last record read or written (LOC)
    char *buf;            ///< random-access record buffer
    int nfields;
    struct {
        char **slot;      ///< the FIELD variable
        uint16_t off, len;
    } fields[MAX_FIELDS];
    bool com_lf;          ///< COM1: send LF after each CR (the LF option)
    char name[64];
} bfile_t;

static bfile_t *files[MAX_FILES + 1];  ///< #1 .. #MAX_FILES

static bfile_t *open_file_arg(void);
static bfile_t *reading_file(void);
static int file_getc(bfile_t *b);
static int file_peek(bfile_t *b);
static int free_file_number(void);
static void check_break(void);

/// PRINT # and WRITE # send output through out_char to this file (or COM1),
/// with out_col counting its column; the screen's column waits here.
static bfile_t *out_dev;
static int out_col, out_row, screen_col;

static void error(int code) {
    if (out_dev) {
        out_dev = NULL;
        out_col = screen_col;
    }
    longjmp(error_jump, code);
}

static int fresult_error(FRESULT r) {
    switch (r) {
    case FR_NO_FILE:
    case FR_NO_PATH: return E_FILE_NOT_FOUND;
    case FR_INVALID_NAME: return E_BAD_FILE_NAME;
    case FR_EXIST: return E_FILE_EXISTS;
    case FR_DENIED:
    case FR_WRITE_PROTECTED: return E_PERMISSION;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
    case FR_INVALID_DRIVE: return E_DISK_NOT_READY;
    default: return E_DEVICE_IO;
    }
}

// ---- Output, with the cursor position tracked for POS, CSRLIN and TAB ----

static void file_write(const char *data, UINT n) {
    if (out_dev->mode == FM_COM) {
        port_serial_write(data, n);
        return;
    }
    UINT w;
    FRESULT r = f_write(&out_dev->f, data, n, &w);
    if (r != FR_OK)
        error(E_DEVICE_IO);
    if (w != n)
        error(E_DISK_FULL);
}

static void out_char(int c) {
    if (out_dev) {
        char ch = c;
        if (c == '\n') {
            // Text files end lines in CR LF; COM1 sends CR, and LF only
            // with the LF option, as GW-BASIC does.
            if (out_dev->mode != FM_COM || out_dev->com_lf)
                file_write("\r\n", 2);
            else
                file_write("\r", 1);
            out_col = 0;
        } else {
            file_write(&ch, 1);
            if ((unsigned char)c >= 0x20)
                ++out_col;
        }
        return;
    }
    // As on the IBM PC, most control codes show a picture (CHR$(1) is a
    // smiley); 11 and 28-31 move the cursor.
    if (c < 0x20 && c != 7 && c != '\b' && c != '\t' && c != '\n' && c != '\f' &&
        c != '\r' && c != 0x1b) {
        static const char *const moves[] = {[11] = "\x1b[H", [28] = "\x1b[C", [29] = "\x1b[D",
                                            [30] = "\x1b[A", [31] = "\x1b[B"};
        if (c == 11 || c >= 28) {
            fputs(moves[c], stdout);
            if (c == 11)
                out_row = out_col = 0;
            else if (c == 28 && out_col < SCREEN_COLS - 1)
                ++out_col;
            else if (c == 29 && out_col > 0)
                --out_col;
            else if (c == 30 && out_row > 0)
                --out_row;
            else if (c == 31 && out_row < SCREEN_ROWS - 1)
                ++out_row;
            return;
        }
        fflush(stdout);
        port_put_glyph(c);
        c = ' ';  // for the column count below
    } else {
        putchar(c);
    }
    if (c == '\f') {
        out_row = out_col = 0;
    } else if (c == '\n') {
        out_col = 0;
        if (out_row < SCREEN_ROWS - 1)
            ++out_row;
    } else if (c == '\r') {
        out_col = 0;
    } else if (c == '\b') {
        if (out_col > 0)
            --out_col;
    } else if ((unsigned char)c >= 0x20 && ++out_col >= SCREEN_COLS) {
        out_col = 0;
        if (out_row < SCREEN_ROWS - 1)
            ++out_row;
    }
}

static void out_str(const char *s) {
    while (*s)
        out_char((unsigned char)*s++);
}

static void out_bytes(const char *s, int n) {
    for (int i = 0; i < n; ++i)
        out_char((unsigned char)s[i]);
}

static void out_newline(void) {
    out_char('\n');
}

static void out_flush(void) {
    fflush(stdout);
}

// ---- Keyboard: Ctrl-C checks must not eat other keys ----------------------

static char key_queue[64];
static int key_head, key_tail;
static bool break_pending;

/// Moves typed keys into key_queue, watching for Ctrl-C. Normally it reads
/// only while there is room, so the rest of a paste waits in the input driver
/// instead of being lost. Break checks in a running program pass drop_extra,
/// so a Ctrl-C still gets through after the queue fills.
static void read_keys(bool drop_extra) {
    int c;
    for (;;) {
        bool full = (key_head + 1) % (int)sizeof key_queue == key_tail;
        if ((full && !drop_extra) || (c = port_key(0)) < 0)
            break;
        if (c == 3) {
            break_pending = true;
        } else if (!full) {
            key_queue[key_head] = c;
            key_head = (key_head + 1) % (int)sizeof key_queue;
        }
    }
}

static void poll_keys(void) {
    read_keys(false);
}

static int key_now(void) {
    poll_keys();
    if (key_head == key_tail)
        return -1;
    int c = (unsigned char)key_queue[key_tail];
    key_tail = (key_tail + 1) % (int)sizeof key_queue;
    return c;
}

static int key_wait(void) {
    for (;;) {
        if (break_pending) {
            break_pending = false;
            return 3;
        }
        int c = key_now();
        if (c >= 0)
            return c;
        c = port_key(100000);
        if (c >= 0)
            return c;
    }
}

// ---- Line input ------------------------------------------------------------
//
// INPUT, the Ok prompt and AUTO read lines with editing: Left/Right, Home/
// End, Backspace/Delete, Ins (insert or overwrite), Esc (clear the line),
// and at the Ok prompt Up/Down for earlier lines. A line can run over
// several screen rows; positions are offsets from the start of the row the
// line begins on (SCREEN_COLS to a row).

#define HISTORY 8
static char (*history)[LINE_MAX_LEN + 1];  ///< HISTORY of them (allocated: SRAM is short)
static int history_count;  ///< lines in history (the newest is history[0])

static int le_start, le_cur;  ///< where the text starts, where the cursor is

static void le_move(int off) {
    if (off == le_cur)
        return;
    int rows = off / SCREEN_COLS - le_cur / SCREEN_COLS;
    if (rows < 0)
        printf("\x1b[%dA", -rows);
    else if (rows > 0)
        printf("\x1b[%dB", rows);
    printf("\x1b[%dG", off % SCREEN_COLS + 1);
    le_cur = off;
}

/// Draws buf[from, n) and `erase` blanks after it, then puts the cursor at
/// pos.
static void le_draw(const char *buf, int n, int from, int pos, int erase) {
    le_move(le_start + from);
    fwrite(buf + from, 1, n - from, stdout);
    for (int i = 0; i < erase; ++i)
        putchar(' ');
    int end = le_start + n + erase;
    // A line ending exactly at the edge leaves the cursor waiting to wrap;
    // move it on so it agrees with `end`.
    if (end % SCREEN_COLS == 0 && end > le_start + from)
        printf("\r\n");
    le_cur = end;
    le_move(le_start + pos);
}

static void history_add(const char *line) {
    if (!*line || (history_count && !strcmp(history[0], line)))
        return;
    memmove(history[1], history[0], sizeof history[0] * (HISTORY - 1));
    snprintf(history[0], sizeof history[0], "%s", line);
    if (history_count < HISTORY)
        ++history_count;
}

/// Reads the rest of an escape sequence: returns 'A'-'D', 'H', 'F' (Home,
/// End), 'I' (Ins), 'X' (Delete), or 0 for one we don't use; -1 for a lone
/// Esc.
/// The next key, from the queue or arriving within `us`; -1 if none.
static int next_key(uint32_t us) {
    int c = key_now();
    return c >= 0 ? c : port_key(us);
}

#define KEY_F(n) (0x100 + (n))  ///< function keys F1-F12

static int read_escape(void) {
    int c1 = next_key(20000);
    if (c1 == 'O') {
        int c2 = next_key(20000);
        if (c2 >= 'P' && c2 <= 'S')
            return KEY_F(c2 - 'P' + 1);
        return c2 == 'H' || c2 == 'F' ? c2 : 0;
    }
    if (c1 != '[')
        return -1;
    int param = 0, c2;
    while ((c2 = next_key(20000)) >= '0' && c2 <= '9')
        param = param * 10 + c2 - '0';
    if (c2 == '~') {
        static const int fkeys[] = {[11] = 1, [12] = 2, [13] = 3, [14] = 4, [15] = 5, [17] = 6,
                                    [18] = 7, [19] = 8, [20] = 9, [21] = 10, [23] = 11, [24] = 12};
        if (param < 25 && fkeys[param])
            return KEY_F(fkeys[param]);
        return param == 1 || param == 7 ? 'H' : param == 4 || param == 8 ? 'F'
             : param == 2 ? 'I' : param == 3 ? 'X' : param == 5 ? 'U' : param == 6 ? 'V' : 0;
    }
    return (c2 >= 'A' && c2 <= 'D') || c2 == 'H' || c2 == 'F' ? c2 : 0;
}

/// KEY n, "text": what F1-F10 type at the Ok prompt and in INPUT ("\r"
/// ends the line), GW-BASIC's defaults to start with.
static char soft_keys[10][16] = {"LIST ", "RUN\r", "LOAD\"", "SAVE\"", "CONT\r",
                                 ",\"LPT1:\"\r", "TRON\r", "TROFF\r", "KEY ", "SCREEN 0,0,0\r"};

/// Reads a line with editing (and, with `use_history`, Up/Down for earlier
/// lines). Returns false on Ctrl-C.
static bool read_line_ex(char *buf, int max, const char *initial, bool use_history) {
    out_flush();
    le_start = le_cur = out_col;
    int n = 0, pos = 0, view = -1;
    bool overwrite = false;
    char saved[LINE_MAX_LEN + 1] = "";  // what was typed before browsing history
    buf[0] = 0;
    if (initial) {
        snprintf(buf, max, "%s", initial);
        n = pos = strlen(buf);
        le_draw(buf, n, 0, pos, 0);
    }
    out_flush();
    for (;;) {
        int c = key_wait();
        if (c == 3) {
            le_move(le_start + n);
            out_str("^C");
            out_newline();
            return false;
        }
        if (c == '\r' || c == '\n') {
            le_move(le_start + n);
            buf[n] = 0;
            out_col = (le_start + n) % SCREEN_COLS;
            out_newline();
            if (use_history)
                history_add(buf);
            return true;
        }
        int old_n = n;
        if (c == 0x1b) {
            int k = read_escape();
            if (k == -1) {  // Esc: clear the line
                n = pos = 0;
                le_draw(buf, n, 0, 0, old_n);
            } else if (k == 'D' && pos > 0) {
                le_move(le_start + --pos);
            } else if (k == 'C' && pos < n) {
                le_move(le_start + ++pos);
            } else if (k == 'H') {
                le_move(le_start + (pos = 0));
            } else if (k == 'F') {
                le_move(le_start + (pos = n));
            } else if (k == 'I') {
                overwrite = !overwrite;
            } else if (k >= KEY_F(1) && k <= KEY_F(10)) {
                // A soft key types its text; a CR in it ends the line.
                for (const char *t = soft_keys[k - KEY_F(1)]; *t; ++t) {
                    if (*t == '\r') {
                        le_draw(buf, n, 0, n, 0);
                        le_move(le_start + n);
                        buf[n] = 0;
                        out_col = (le_start + n) % SCREEN_COLS;
                        out_newline();
                        if (use_history)
                            history_add(buf);
                        return true;
                    }
                    if (n < max - 1) {
                        memmove(buf + pos + 1, buf + pos, n - pos);
                        buf[pos++] = *t;
                        ++n;
                    }
                }
                le_draw(buf, n, 0, pos, 0);
            } else if (k == 'X' && pos < n) {
                memmove(buf + pos, buf + pos + 1, n - pos - 1);
                --n;
                le_draw(buf, n, pos, pos, 1);
            } else if ((k == 'A' || k == 'B') && use_history) {
                int v = view + (k == 'A' ? 1 : -1);
                if (v >= history_count || v < -1)
                    continue;
                if (view == -1) {
                    memcpy(saved, buf, n);
                    saved[n] = 0;
                }
                view = v;
                snprintf(buf, max, "%.255s", v < 0 ? saved : history[v]);
                n = pos = strlen(buf);
                le_draw(buf, n, 0, pos, old_n > n ? old_n - n : 0);
            }
            continue;
        }
        if ((c == '\b' || c == 0x7f) && pos > 0) {
            memmove(buf + pos - 1, buf + pos, n - pos);
            --n;
            --pos;
            le_draw(buf, n, pos, pos, 1);
        } else if (c >= 0x20 && c != 0x7f) {
            if (overwrite && pos < n) {
                buf[pos++] = c;
                le_draw(buf, n, pos - 1, pos, 0);
            } else if (n < max - 1) {
                memmove(buf + pos + 1, buf + pos, n - pos);
                buf[pos++] = c;
                ++n;
                if (pos == n) {  // typing at the end: just echo it
                    putchar(c);
                    le_cur = le_start + n;
                    if (le_cur % SCREEN_COLS == 0)
                        printf("\r\n");
                } else {
                    le_draw(buf, n, pos - 1, pos, 0);
                }
            }
        }
        out_flush();
    }
}

static bool read_line(char *buf, int max, const char *initial) {
    return read_line_ex(buf, max, initial, false);
}

// ---- Temporary strings ----------------------------------------------------

static char **temps;
static int temp_count, temp_cap;

static void need_mem(size_t n, size_t keep) {
    if (port_free_mem() < n + keep)
        error(E_OUT_OF_MEMORY);
}

static void *checked_malloc(size_t n) {
    need_mem(n + 16, HEAP_MARGIN);
    void *p = malloc(n ? n : 1);
    if (!p)
        error(E_OUT_OF_MEMORY);
    return p;
}

static void *checked_realloc(void *old, size_t n) {
    need_mem(n + 16, HEAP_MARGIN);
    void *p = realloc(old, n);
    if (!p)
        error(E_OUT_OF_MEMORY);
    return p;
}

/// BASIC strings keep their length in the two bytes before the first
/// character, so they can hold any byte, 0 included (MKI$, CHR$(0), binary
/// records); they also end in a 0, so C code can read text ones as they are.
#define SHDR 2

static char *salloc(int len) {
    char *b = checked_malloc(len + 1 + SHDR);
    uint16_t l = len;
    memcpy(b, &l, SHDR);
    b[SHDR + len] = 0;
    return b + SHDR;
}

static void sfree(char *s) {
    if (s)
        free(s - SHDR);
}

static int slen(const char *s) {
    uint16_t l;
    memcpy(&l, s - SHDR, SHDR);
    return l;
}

static const char empty_str[SHDR + 1];  ///< length 0
#define EMPTY (empty_str + SHDR)

/// A writable temporary of len characters (plus the terminator).
static char *temp_alloc(int len) {
    if (len > STR_MAX)
        error(E_STRING_TOO_LONG);
    if (temp_count == temp_cap) {
        int cap = temp_cap ? temp_cap * 2 : 32;
        temps = checked_realloc(temps, cap * sizeof *temps);
        temp_cap = cap;
    }
    char *t = salloc(len);
    temps[temp_count++] = t;
    return t;
}

static char *temp_str(const char *s, int len) {
    char *t = temp_alloc(len);
    memcpy(t, s, len);
    return t;
}

static void free_temps(void) {
    while (temp_count)
        sfree(temps[--temp_count]);
}

// ---- Values ---------------------------------------------------------------

typedef struct {
    bool is_str;
    double n;
    const char *s;  ///< a temporary or a variable's string; never freed here
} value_t;

static value_t num(double n) {
    return (value_t){.is_str = false, .n = n};
}

static value_t str(const char *s, int len) {
    return (value_t){.is_str = true, .s = temp_str(s, len)};
}

static double want_num(value_t v) {
    if (v.is_str)
        error(E_TYPE_MISMATCH);
    return v.n;
}

static const char *want_str(value_t v) {
    if (!v.is_str)
        error(E_TYPE_MISMATCH);
    return v.s;
}

/// GW-BASIC style: no leading zero, a space in place of a + sign, and a
/// trailing space when printed.
static void format_number(double n, char *out, int size) {
    char buf[40];
    if (n == 0) {
        snprintf(out, size, " 0");
        return;
    }
    if (fabs(n) < 1e15 && n == floor(n))
        snprintf(buf, sizeof buf, "%.0f", fabs(n));
    else if ((double)(float)n == n)  // a single-precision value: 7 digits, as GW-BASIC
        snprintf(buf, sizeof buf, "%.7G", fabs(n));
    else
        snprintf(buf, sizeof buf, "%.9G", fabs(n));
    const char *b = buf;
    if (b[0] == '0' && b[1] == '.')
        ++b;
    // "1E+20" rather than "1E+020"; drop a "+0" padding some libcs add.
    char tidy[40];
    int t = 0;
    for (int i = 0; b[i] && t < (int)sizeof tidy - 1; ++i) {
        tidy[t++] = b[i];
        if ((b[i] == '+' || b[i] == '-') && i > 0 && b[i - 1] == 'E')
            while (b[i + 1] == '0' && b[i + 2])
                ++i;
    }
    tidy[t] = 0;
    snprintf(out, size, "%c%.38s", n < 0 ? '-' : ' ', tidy);
}

// ---- Program --------------------------------------------------------------

typedef struct {
    uint16_t num;
    char *text;  ///< statements after the line number
} line_t;

static line_t *lines;
static int line_count, line_cap;
static char program_name[64];

static int find_line(int num, bool *exact) {
    int lo = 0, hi = line_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (lines[mid].num < num)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (exact)
        *exact = lo < line_count && lines[lo].num == num;
    return lo;
}

static void delete_line_at(int i) {
    free(lines[i].text);
    memmove(&lines[i], &lines[i + 1], (line_count - i - 1) * sizeof *lines);
    --line_count;
}

static void clear_program(void) {
    while (line_count)
        delete_line_at(line_count - 1);
    free(lines);
    lines = NULL;
    line_cap = 0;
}

static bool is_ident_start(int c) {
    return isalpha(c);
}

static bool is_ident_char(int c) {
    return isalnum(c) || c == '.';
}

/// Upper-cases keywords and names, leaving strings, REM text and DATA as typed.
static void normalise(char *s) {
    bool in_str = false, in_data = false;
    for (char *p = s; *p; ++p) {
        if (*p == '"') {
            in_str = !in_str;
            continue;
        }
        if (in_str)
            continue;
        if (in_data) {
            if (*p == ':')
                in_data = false;
            continue;
        }
        if (*p == '\'')
            return;
        *p = toupper((unsigned char)*p);
        if (!strncasecmp(p, "REM", 3) && (p == s || !is_ident_char(p[-1]))) {
            p[1] = 'E';
            p[2] = 'M';
            return;
        }
        if (!strncasecmp(p, "DATA", 4) && (p == s || !is_ident_char(p[-1]))) {
            p[1] = 'A';
            p[2] = 'T';
            p[3] = 'A';
            p += 3;
            in_data = true;
        }
    }
}

static int last_line_num;  ///< the line EDIT . means: the last one typed or that failed

static void store_line(int num, const char *text) {
    last_line_num = num;
    bool exact;
    int i = find_line(num, &exact);
    while (*text == ' ')
        ++text;
    if (!*text) {
        if (!exact)
            error(E_UNDEFINED_LINE);
        delete_line_at(i);
        return;
    }
    size_t len = strlen(text);
    need_mem(len + 1 + (line_count == line_cap ? line_cap * sizeof *lines : 0), DATA_RESERVE);
    char *copy = checked_malloc(len + 1);
    memcpy(copy, text, len + 1);
    normalise(copy);
    if (exact) {
            free(lines[i].text);
    } else {
        if (line_count == line_cap) {
            int cap = line_cap ? line_cap * 2 : 64;
            line_t *l = port_free_mem() >= cap * sizeof *l + HEAP_MARGIN
                            ? realloc(lines, cap * sizeof *l) : NULL;
            if (!l) {
                free(copy);
                error(E_OUT_OF_MEMORY);
            }
            lines = l;
            line_cap = cap;
        }
        memmove(&lines[i + 1], &lines[i], (line_count - i) * sizeof *lines);
        ++line_count;
        lines[i].num = num;
    }
    lines[i].text = copy;
}

// ---- Variables ------------------------------------------------------------

typedef struct var {
    struct var *next;
    char name[NAME_MAX + 2];  ///< including a $, %, ! or # suffix
    bool is_str;
    double n;
    char *s;
} var_t;

typedef struct array {
    struct array *next;
    char name[NAME_MAX + 2];
    bool is_str;
    int dims;
    int size[8];  ///< upper bound + 1 per dimension
    int count;
    double *n;
    char **s;
} array_t;

typedef struct fn {
    struct fn *next;
    char name[NAME_MAX + 2];
    char params[4][NAME_MAX + 2];
    int param_count;
    int line;            ///< where the body lives
    const char *body;    ///< text after '='
    /// Across CHAIN ... ALL (see keep_fns_for_chain): the line number, where
    /// the body was in it, and a copy of it.
    int line_num, body_off;
    char *saved;
} fn_t;

#define BUCKETS 64
static var_t *(*vars_p)[BUCKETS];  ///< (basic_ram, while BASIC runs)
static array_t *(*arrays_p)[BUCKETS];
#define vars   (*vars_p)
#define arrays (*arrays_p)
static fn_t *fns;

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % BUCKETS;
}

static void forget_fields(void);

/// The type suffix a name without one gets, per first letter (DEFINT, DEFSNG,
/// DEFDBL, DEFSTR); 0 means the default, single precision ('!'). So A and A!
/// are the same variable, and after DEFINT A, so are A and A%.
static char def_type[26];

/// Frees the variables and arrays `keep` doesn't want (all of them if it's
/// NULL), and every DEF FN.
static void prune_vars(bool (*keep)(const char *name, bool array), bool keep_fns) {
    forget_fields();
    memset(def_type, 0, sizeof def_type);
    for (int b = 0; b < BUCKETS; ++b) {
        for (var_t **vp = &vars[b]; *vp;) {
            var_t *v = *vp;
            if (keep && keep(v->name, false)) {
                vp = &v->next;
                continue;
            }
            *vp = v->next;
            sfree(v->s);
            free(v);
        }
        for (array_t **ap = &arrays[b]; *ap;) {
            array_t *a = *ap;
            if (keep && keep(a->name, true)) {
                ap = &a->next;
                continue;
            }
            *ap = a->next;
            if (a->s)
                for (int i = 0; i < a->count; ++i)
                    sfree(a->s[i]);
            free(a->s);
            free(a->n);
            free(a);
        }
    }
    while (fns && !keep_fns) {
        fn_t *f = fns;
        fns = f->next;
        free(f->saved);
        free(f);
    }
}

/// CHAIN ... ALL keeps DEF FN functions, but they only work while their
/// definition is in the program, unchanged at the same place. That's how it
/// works out in GW-BASIC, which keeps a pointer into the program text: a game
/// that chains to another program and back to the first finds its functions
/// again. Elsewhere calling one is "Undefined user function" rather than
/// running garbage. Before loading: note where each one is (body NULL while
/// it's not in the program); a call finds it again (fn_body).
static void keep_fns_for_chain(void) {
    for (fn_t *f = fns; f; f = f->next) {
        if (!f->body)
            continue;  // already waiting for its program to come back
        f->line_num = f->line >= 0 && f->line < line_count ? lines[f->line].num : -1;
        f->body_off = f->line_num >= 0 ? (int)(f->body - lines[f->line].text) : 0;
        f->saved = malloc(strlen(f->body) + 1);
        if (f->saved)
            strcpy(f->saved, f->body);
        f->body = NULL;
    }
}

/// The function's body in the current program, or NULL.
static const char *fn_body(fn_t *f) {
    if (f->body)
        return f->body;
    bool exact = false;
    int i = f->line_num >= 0 && f->saved ? find_line(f->line_num, &exact) : 0;
    if (!exact || (int)strlen(lines[i].text) < f->body_off ||
        strcmp(lines[i].text + f->body_off, f->saved))
        return NULL;
    f->line = i;
    f->body = lines[i].text + f->body_off;
    free(f->saved);
    f->saved = NULL;
    return f->body;
}

static int option_base;  ///< OPTION BASE: the lowest subscript, 0 or 1

static void sprites_reset(void);
static void fonts_reset(void);

static void clear_vars(void) {
    sprites_reset();
    fonts_reset();
    prune_vars(NULL, false);
    option_base = 0;
}

static var_t *find_var(const char *name) {
    unsigned h = hash(name);
    for (var_t *v = vars[h]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    var_t *v = checked_malloc(sizeof *v);
    memset(v, 0, sizeof *v);
    strcpy(v->name, name);
    v->is_str = name[strlen(name) - 1] == '$';
    v->next = vars[h];
    vars[h] = v;
    return v;
}

/// Stores len bytes of s (any bytes) in a string variable.
static void set_strn(char **slot, const char *s, int len) {
    char *copy = salloc(len);
    memcpy(copy, s, len);
    sfree(*slot);
    *slot = copy;
}

/// Stores a BASIC string value in a string variable.
static void set_str(char **slot, const char *s) {
    set_strn(slot, s, slen(s));
}

static array_t *find_array(const char *name) {
    for (array_t *a = arrays[hash(name)]; a; a = a->next)
        if (!strcmp(a->name, name))
            return a;
    return NULL;
}

static array_t *make_array(const char *name, int dims, const int *bounds) {
    if (find_array(name))
        error(E_DUPLICATE_DEF);
    long count = 1;
    for (int i = 0; i < dims; ++i) {
        if (bounds[i] < 0)
            error(E_ILLEGAL_CALL);
        count *= bounds[i] + 1;
        if (count > 65536)
            error(E_OUT_OF_MEMORY);
    }
    array_t *a = checked_malloc(sizeof *a);
    memset(a, 0, sizeof *a);
    strcpy(a->name, name);
    a->is_str = name[strlen(name) - 1] == '$';
    a->dims = dims;
    for (int i = 0; i < dims; ++i)
        a->size[i] = bounds[i] + 1;
    a->count = count;
    if (port_free_mem() < count * (a->is_str ? sizeof *a->s : sizeof *a->n) + HEAP_MARGIN) {
        free(a);
        error(E_OUT_OF_MEMORY);
    }
    if (a->is_str) {
        a->s = calloc(count, sizeof *a->s);
        if (!a->s) {
            free(a);
            error(E_OUT_OF_MEMORY);
        }
    } else {
        a->n = calloc(count, sizeof *a->n);
        if (!a->n) {
            free(a);
            error(E_OUT_OF_MEMORY);
        }
    }
    unsigned h = hash(name);
    a->next = arrays[h];
    arrays[h] = a;
    return a;
}

// ---- Execution state ------------------------------------------------------

/// A place in the program: a line index (-1 for the direct-mode line) and a
/// position in its text.
typedef struct {
    int line;
    const char *p;
} pos_t;

static char (*direct_line_p)[LINE_MAX_LEN + 1];  ///< (basic_ram, while BASIC runs)
#define direct_line (*direct_line_p)
static int cur_line;    ///< index into lines[], or -1 in direct mode
static const char *p;   ///< the parse position
static bool running;    ///< a program (not just a direct line) is executing
static bool jumped;     ///< the statement moved p (GOTO, IF, NEXT, ...)
static bool trace;
static pos_t cont_pos;  ///< where CONT resumes, line < -1 if it can't
static pos_t data_pos;  ///< next DATA item

typedef struct {
    var_t *var;
    double limit, step;
    pos_t body;
} for_frame_t;
static for_frame_t *for_stack;  ///< STACK_MAX each, while BASIC runs (SRAM is short)
static int for_depth;
/// A GOSUB also remembers the FOR and WHILE depths: as in GW-BASIC, loops
/// started in a subroutine end at its RETURN, and a FOR or NEXT there doesn't
/// touch the caller's loops (a FOR X inside a FOR X loop's subroutine).
typedef struct {
    pos_t ret;
    int for_base, while_base;
    int event;  ///< the trapped event this GOSUB handles, or -1
} gosub_frame_t;
static gosub_frame_t *gosub_stack;
static int gosub_depth;
static pos_t *while_stack;
static int while_depth;

/// The first FOR frame belonging to the current subroutine (or main program).
static int for_base(void) {
    return gosub_depth ? gosub_stack[gosub_depth - 1].for_base : 0;
}

/// Pushes a GOSUB returning to the next statement.
static void push_gosub(void);

/// ON ERROR GOTO: the handler's line number (-1: none), whether it's running,
/// and what ERR, ERL and RESUME need.
static int on_error_line = -1;
static bool in_error_handler;
static int err_code, err_line;
static pos_t err_pos;   ///< the statement that failed
static pos_t stmt_pos;  ///< the statement being run

static void reset_events(void);

static void reset_run_state(void) {
    reset_events();
    on_error_line = -1;
    in_error_handler = false;
    err_code = err_line = 0;
    for_depth = gosub_depth = while_depth = 0;
    data_pos.line = 0;
    data_pos.p = NULL;
}

static int current_line_number(void) {
    return cur_line >= 0 && cur_line < line_count ? lines[cur_line].num : -1;
}

static void jump_to_line(int num) {
    bool exact;
    int i = find_line(num, &exact);
    if (!exact)
        error(E_UNDEFINED_LINE);
    cur_line = i;
    p = lines[i].text;
    running = true;
    jumped = true;
}

// ---- Lexing ---------------------------------------------------------------

static void skip_spaces(void) {
    while (*p == ' ' || *p == '\t')
        ++p;
}

/// Matches a keyword at p (no word boundary needed, as in GW-BASIC).
static bool accept(const char *kw) {
    skip_spaces();
    if (*p != kw[0])
        return false;  // the usual case, decided without strlen
    size_t n = strlen(kw);
    if (strncmp(p, kw, n))
        return false;
    p += n;
    return true;
}

/// statement() and builtin() try their keywords one at a time, but most of the
/// time the text is a variable (A=A+1). These lists hold the start of every
/// keyword each one tries, so when none of them matches, the whole chain can be
/// skipped. tools/check_keywords.py checks they are complete.
static const char *const builtin_words[] = {
    "ABS", "ADC", "ASC", "ATN", "CDBL", "CHR", "CINT", "COS", "CSNG", "CSRLIN", "CVD", "CVI",
    "CVS", "DATE", "DIR", "ENVIRON", "EOF", "ERDEV", "ERL", "ERR", "EXP", "EXTERR", "FIX", "FONTH", "FONTW", "FRE",
    "GWIDTH", "HEX", "I2CREAD", "INKEY", "INP", "INSTR", "INT", "IOCTL", "KEYDOWN", "LCASE", "LEFT", "LEN", "LOC", "LOF",
    "LOG", "LPOS", "MID", "MKD", "MKI", "MKS", "MOUSEB", "MOUSEW", "MOUSEX", "MOUSEY", "MUSIC", "OCT", "PEEK", "PEN", "PIN", "PLAY", "PMAP",
    "POINT", "POS", "RGB", "RIGHT", "RND", "SCREEN", "SGN", "SIN", "SPACE", "SPI", "SPRITEHIT", "SPRITEON", "SPRITEX", "SPRITEY", "SQR", "STICK",
    "STR", "TAN", "TEMP", "TIME", "UCASE", "USR", "VAL", "VARPTR",
};
static const char *const statement_words[] = {
    "BASE", "BEEP", "BLOAD", "BSAVE", "BYE", "CALL", "CHAIN", "CHDIR", "CIRCLE", "CLEAR",
    "CLOSE", "CLS", "COLOR", "COM", "CONT", "DATA", "DATE", "DEF", "DELETE", "DIM", "DRAW",
    "EDIT", "END", "ENVIRON", "ERASE", "ERROR", "EXIT", "FIELD", "FILES", "FONT", "FOR", "GET",
    "GOSUB", "GOTO", "GPRINT", "I2C", "IF", "IMAGE", "INPUT", "IOCTL", "KEY", "KILL", "LET", "LINE", "LIST",
    "LLIST", "LOAD", "LOCATE", "LOCK", "LPRINT", "LSET", "MERGE", "MID", "MKDIR", "MOTOR", "MOUSE", "MUSIC",
    "NAME", "NEW", "NEXT", "ON", "OPEN", "OPTION", "OUT", "PAINT", "PALETTE", "PAUSE",
    "PCOPY", "PEN", "PIN", "PLAY", "POKE", "PRESET", "PRINT", "PSET", "PUT", "PWM", "R",
    "SAVE", "SCREEN", "SERVO", "SHELL", "SOUND", "SPI", "SPRITE", "STOP", "STRIG", "SWAP", "SYSTEM", "TIME",
    "TROFF", "TRON", "UNLOCK", "VIEW", "WAIT", "WEND", "WHILE", "WIDTH", "WINDOW", "WRITE",
};

/// The words of a list grouped by first letter, built on first use.
typedef struct {
    const char *const *words;
    uint8_t count;
    bool ready;
    uint8_t at[27];  ///< words for letter L: order[at[L]] .. order[at[L + 1] - 1]
    uint8_t order[128];
    uint8_t len[128];
} word_set_t;

static word_set_t builtin_set = {.words = builtin_words,
                                 .count = sizeof builtin_words / sizeof *builtin_words};
static word_set_t statement_set = {.words = statement_words,
                                   .count = sizeof statement_words / sizeof *statement_words};

/// False only if no word of the set starts the text at s.
static bool may_be_keyword(word_set_t *w, const char *s) {
    if (*s < 'A' || *s > 'Z')
        return true;  // not a plain upper-case name: let the full chain decide
    if (!w->ready) {
        int n = 0;
        for (int l = 0; l < 26; ++l) {
            w->at[l] = n;
            for (int i = 0; i < w->count; ++i)
                if (w->words[i][0] == 'A' + l)
                    w->order[n++] = i;
        }
        w->at[26] = n;
        for (int i = 0; i < w->count; ++i)
            w->len[i] = strlen(w->words[i]);
        w->ready = true;
    }
    int l = *s - 'A';
    for (int j = w->at[l]; j < w->at[l + 1]; ++j) {
        int i = w->order[j];
        if (!strncmp(s, w->words[i], w->len[i]))
            return true;
    }
    return false;
}

static void expect(const char *kw) {
    if (!accept(kw))
        error(E_SYNTAX);
}

static bool at_end_of_statement(void) {
    skip_spaces();
    return !*p || *p == ':' || *p == '\'' || !strncmp(p, "ELSE", 4);
}

static bool accept_word(const char *kw);

/// Keywords that end a name when they follow it directly ("FORI=1TO9").
static const char *const stop_words[] = {"THEN", "TO", "STEP", "ELSE", "GOTO", "GOSUB",
                                         "AND", "OR", "XOR", "MOD", "EQV", "IMP"};

static bool read_name(char *name) {
    skip_spaces();
    if (!is_ident_start((unsigned char)*p))
        return false;
    int n = 0;
    while (is_ident_char((unsigned char)*p)) {
        if (n > 0 && strchr("TSEGAOXMI", *p)) {  // the stop words' first letters
            bool stop = false;
            // Only when what follows can't be part of a name: SCORE, HAND(5),
            // PHOTO$ and TOTO.1 stay names, as in GW-BASIC.
            for (size_t k = 0; k < sizeof stop_words / sizeof *stop_words; ++k) {
                size_t l = strlen(stop_words[k]);
                char after = p[l];
                if (!strncmp(p, stop_words[k], l) && !is_ident_char((unsigned char)after) &&
                    !strchr("($%!#", after))
                    stop = true;
            }
            if (stop)
                break;
        }
        if (n < NAME_MAX)
            name[n++] = *p;
        ++p;
    }
    if (*p == '$' || *p == '%' || *p == '!' || *p == '#') {
        name[n++] = *p++;
    } else {
        char t = isalpha((unsigned char)name[0]) ? def_type[toupper((unsigned char)name[0]) - 'A'] : 0;
        name[n++] = t ? t : '!';
    }
    name[n] = 0;
    return true;
}

/// The value of numeric variable `name` (as written in a DRAW or PLAY string,
/// "=X1;"): a name without a type gets its DEF type, as in a program.
static double named_number(const char *raw) {
    char name[NAME_MAX + 3];
    int n = 0;
    for (; *raw && n < NAME_MAX + 1; ++raw)
        name[n++] = toupper((unsigned char)*raw);
    if (n && !strchr("$%!#", name[n - 1])) {
        char t = isalpha((unsigned char)name[0]) ? def_type[name[0] - 'A'] : 0;
        name[n++] = t ? t : '!';
    }
    name[n] = 0;
    var_t *v = find_var(name);
    if (v->is_str)
        error(E_TYPE_MISMATCH);
    return v->n;
}

static double read_number_literal(void) {
    skip_spaces();
    if (*p == '&') {
        int base = 8;
        ++p;
        if (*p == 'H') {
            base = 16;
            ++p;
        } else if (*p == 'O') {
            ++p;
        }
        char *end;
        long v = strtol(p, &end, base);
        if (end == p)
            error(E_SYNTAX);
        p = end;
        if (base == 16 && v >= 0x8000 && v <= 0xffff)
            v -= 0x10000;
        return v;
    }
    char buf[64];
    int n = 0;
    while ((isdigit((unsigned char)*p) || *p == '.') && n < 60)
        buf[n++] = *p++;
    if ((*p == 'E' || *p == 'D') &&
        (isdigit((unsigned char)p[1]) ||
         ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])))) {
        buf[n++] = 'E';
        ++p;
        if (*p == '+' || *p == '-')
            buf[n++] = *p++;
        while (isdigit((unsigned char)*p) && n < 62)
            buf[n++] = *p++;
    }
    buf[n] = 0;
    if (*p == '#' || *p == '!' || *p == '%')
        ++p;
    if (!n || !strcmp(buf, "."))
        error(E_SYNTAX);
    // Up to 15 digits with an optional point: the digits as an integer and a
    // power of ten are both exact, so one division rounds just as strtod does,
    // only much faster.
    static const double pow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
                                   1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    int64_t mant = 0;
    int digits = 0, frac = -1;
    for (int i = 0; i < n; ++i) {
        if (buf[i] == '.') {
            if (frac >= 0)
                goto slow;
            frac = 0;
        } else if (buf[i] == 'E' || ++digits > 15) {
            goto slow;
        } else {
            mant = mant * 10 + (buf[i] - '0');
            if (frac >= 0)
                ++frac;
        }
    }
    return frac > 0 ? (double)mant / pow10[frac] : (double)mant;
slow:
    return strtod(buf, NULL);
}

static int read_line_number(void) {
    skip_spaces();
    if (!isdigit((unsigned char)*p))
        error(E_SYNTAX);
    long n = strtol(p, (char **)&p, 10);
    if (n > 65529)
        error(E_SYNTAX);
    return n;
}

// ---- Expressions ----------------------------------------------------------

static value_t expr(void);

static double num_expr(void) {
    return want_num(expr());
}

static const char *str_expr(void) {
    return want_str(expr());
}

static int arg_int(void) {
    double d = num_expr();
    if (d < -32768.0 * 65536 || d > 32767.0 * 65536)
        error(E_OVERFLOW);
    return (int)lround(d);
}

/// A GPIO number (or LED for the board's LED) that BASIC may use.
static int pin_arg(void) {
    int pin = accept("LED") ? port_led_pin() : arg_int();
    if (pin < 0 || pin >= port_pin_count())
        error(E_ILLEGAL_CALL);
    const char *why = port_pin_reserved(pin);
    if (why) {
        out_flush();
        printf("GP%d is used by %s\n", pin, why);
        error(E_ILLEGAL_CALL);
    }
    return pin;
}

/// The element of `name(...)`, creating a default 0..10 array if needed.
static void *array_element(const char *name, bool *is_str) {
    int idx[8], n = 0;
    do {
        if (n == 8)
            error(E_SUBSCRIPT);
        idx[n++] = arg_int();
    } while (accept(","));
    expect(")");
    array_t *a = find_array(name);
    if (!a) {
        int bounds[8];
        for (int i = 0; i < n; ++i)
            bounds[i] = 10;
        a = make_array(name, n, bounds);
    }
    if (n != a->dims)
        error(E_SUBSCRIPT);
    long off = 0;
    for (int i = 0; i < n; ++i) {
        if (idx[i] < option_base || idx[i] >= a->size[i])
            error(E_SUBSCRIPT);
        off = off * a->size[i] + idx[i];
    }
    *is_str = a->is_str;
    return a->is_str ? (void *)&a->s[off] : (void *)&a->n[off];
}

static uint32_t rnd_state = 0x12345678u;
static double rnd_last;

static double rnd_next(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    rnd_last = (rnd_state >> 8) / 16777216.0;
    return rnd_last;
}

static value_t call_fn(const char *name);
static value_t environ_value(value_t arg);
static value_t inkey(void);
static bool key_line_on;
static void draw_key_line(void);
static void set_key_line(bool on);
static int peek_byte(long addr);
static long seg_address(double offset);
static double phys_x(double x);
static double phys_y(double y);
static int px_of(double x);
static int py_of(double y);
static double pmap(double v, int n);
static double point_cursor(int n);
static char type_of(const char *name);
static double typed_value(char type, double d);

static value_t primary(void);

static value_t paren_expr(void) {
    expect("(");
    value_t v = expr();
    expect(")");
    return v;
}

static double one_num_arg(void) {
    expect("(");
    double d = num_expr();
    expect(")");
    return d;
}

static int gfx_point(int x, int y);

/// Kept out of line so its locals only take stack when a function is called.
__attribute__((noinline)) // Microsoft Binary Format, GW-BASIC's MKS$/MKD$ layout: little-endian
/// mantissa (sign in the top bit of the byte below the exponent), then an
/// exponent byte biased by 129 (0 means zero). Single precision is 4 bytes,
/// double 8.
static void to_mbf(double x, char *out, int size) {
    memset(out, 0, size);
    if (x == 0)
        return;
    int e;
    double m = frexp(fabs(x), &e);  // x = m * 2^e, 0.5 <= m < 1
    // MBF: value = 0.1mmm (binary) * 2^(exp - 128), so exp = e + 128.
    int exp = e + 128;
    if (exp > 255)
        error(E_OVERFLOW);
    if (exp < 1)
        return;  // too small: zero
    int bits = size == 4 ? 23 : 55;  // mantissa bits after the implied 1
    uint64_t mant = (uint64_t)ldexp(m * 2 - 1, bits);  // drop the implied 1
    // Round to nearest.
    double rest = ldexp(m * 2 - 1, bits) - (double)mant;
    if (rest >= 0.5 && ++mant >> bits) {
        mant = 0;
        if (++exp > 255)
            error(E_OVERFLOW);
    }
    for (int i = 0; i < size - 1; ++i)
        out[i] = (char)(mant >> (8 * i));
    if (x < 0)
        out[size - 2] |= 0x80;
    out[size - 1] = (char)exp;
}

static double from_mbf(const char *in, int size) {
    int exp = (uint8_t)in[size - 1];
    if (!exp)
        return 0;
    int bits = size == 4 ? 23 : 55;
    uint64_t mant = 0;
    for (int i = 0; i < size - 1; ++i)
        mant |= (uint64_t)(uint8_t)in[i] << (8 * i);
    bool neg = mant >> bits & 1;
    mant &= ((uint64_t)1 << bits) - 1;
    double m = 1 + ldexp((double)mant, -bits);  // 1.mmm
    double x = ldexp(m, exp - 129);
    return neg ? -x : x;
}

static int mouse_coord(bool want_x);

static value_t sprite_function(int what);
static int text_width(int n, const char *s, int len);
static int font_slot(int n, bool loaded);
static int font_size(int n, bool width);
static value_t dir_function(void);
static value_t i2c_read_function(bool string);
static value_t spi_function(void);
static int cur_font;

static value_t builtin(void) {
    skip_spaces();
    const char *start = p;
    if (!may_be_keyword(&builtin_set, p))
        return (value_t){.is_str = false, .n = NAN, .s = (const char *)1};  // not a builtin
    // Functions that take no parentheses.
    if (accept("RND")) {
        skip_spaces();
        if (*p != '(')
            return num(rnd_next());
        double x = one_num_arg();
        if (x < 0) {
            rnd_state = (uint32_t)(x * -1000003.0) | 1;
            return num(rnd_next());
        }
        return num(x == 0 ? rnd_last : rnd_next());
    }
    if (accept("TIMER"))
        return num(port_seconds());
    if (accept_word("MOUSEX") || accept_word("MOUSEY")) {
        // In the screen's own units: pixels as PSET takes them, or in text
        // mode the column or row as LOCATE takes them. 0 without a mouse.
        return num(mouse_coord(start[5] == 'X'));
    }
    if (accept_word("MOUSEB")) {
        int x, y, b;
        return num(port_mouse(&x, &y, &b) ? b : 0);
    }
    if (accept_word("MOUSEW"))
        return num(port_mouse_wheel());
    if (accept("SPRITEHIT("))
        return sprite_function(0);
    if (accept("DIR$("))
        return dir_function();
    if (accept("I2CREAD$("))
        return i2c_read_function(true);
    if (accept("I2CREAD("))
        return i2c_read_function(false);
    if (accept("SPI$("))
        return spi_function();
    if (accept("GWIDTH(")) {
        const char *t = str_expr();
        expect(")");
        return num(text_width(cur_font, t, slen(t)));
    }
    if (accept("FONTW(") || accept("FONTH(")) {
        bool w = p[-2] == 'W';
        int n = font_slot(arg_int(), false);
        expect(")");
        return num(font_size(n, w));
    }
    if (accept("SPRITEX("))
        return sprite_function(1);
    if (accept("SPRITEY("))
        return sprite_function(2);
    if (accept("SPRITEON("))
        return sprite_function(3);
    if (accept("KEYDOWN(")) {
        int n = arg_int();
        expect(")");
        return num(port_key_down(n) ? -1 : 0);
    }
    if (accept("MUSIC(")) {
        int n = arg_int();
        expect(")");
        return num(port_music_state(n));
    }
    if (accept("INKEY$"))
        return inkey();
    if (accept("CSRLIN"))
        return num(out_row + 1);
    if (accept("DATE$") || accept("TIME$")) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        char buf[40];
        if (start[0] == 'D')
            snprintf(buf, sizeof buf, "%02d-%02d-%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
        else
            snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        return str(buf, strlen(buf));
    }
    if (accept("PMAP(")) {
        double v = num_expr();
        expect(",");
        int n = arg_int();
        expect(")");
        return num(pmap(v, n));
    }
    if (accept("PEEK(")) {
        double off = num_expr();
        expect(")");
        return num(peek_byte(seg_address(off)));
    }
    if (accept("SCREEN(")) {
        int row = arg_int();
        expect(",");
        int col = arg_int();
        double z = 0;
        if (accept(","))
            z = num_expr();
        expect(")");
        if (row < 1 || row > SCREEN_ROWS || col < 1 || col > SCREEN_COLS)
            error(E_ILLEGAL_CALL);
        out_flush();
        uint8_t ch, at;
        port_get_cell(row - 1, col - 1, &ch, &at);
        return num(z != 0 ? at : ch);
    }
    if (accept("ENVIRON$(")) {
        value_t v = expr();
        expect(")");
        return environ_value(v);
    }
    if (accept("LPOS(") || accept("STICK(") || accept("STRIG(") || accept("PEN(")) {
        arg_int();
        expect(")");
        return num(0);  // no printer, joystick or light pen
    }
    if (accept("INP(") || accept("USR") || accept("VARPTR") || accept("IOCTL$(")) {
        error(E_ILLEGAL_CALL);  // no PC I/O ports or machine code
    }
    if (accept("PLAY(")) {
        arg_int();
        expect(")");
        return num(port_play_pending());
    }
    if (accept_word("ERR"))
        return num(err_code);
    if (accept_word("ERL"))
        return num(err_line);
    if (accept_word("ERDEV"))
        return num(0);  // no DOS devices to fail
    if (accept_word("ERDEV$"))
        return str("", 0);
    if (accept("EXTERR(")) {
        arg_int();
        expect(")");
        return num(0);
    }
    if (accept("FREEFILE"))
        return num(free_file_number());

    static const char *const names[] = {
        "ABS(", "INT(", "FIX(", "SGN(", "SQR(", "SIN(", "COS(", "TAN(", "ATN(", "EXP(", "LOG(",
        "LEN(", "LEFT$(", "RIGHT$(", "MID$(", "CHR$(", "ASC(", "STR$(", "VAL(", "INSTR(",
        "STRING$(", "SPACE$(", "HEX$(", "OCT$(", "UCASE$(", "LCASE$(", "FRE(", "POS(", "CINT(",
        "CSNG(", "CDBL(", "POINT(", "RGB(", "PIN(", "ADC(", "TEMP(", "EOF(", "LOF(",
        "LOC(", "INPUT$(", "MKI$(", "MKS$(", "MKD$(", "CVI(", "CVS(", "CVD(",
    };
    int which = -1;
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        size_t n = strlen(names[i]);
        if (!strncmp(p, names[i], n - 1)) {
            const char *q = p + n - 1;
            while (*q == ' ')
                ++q;
            if (*q == '(') {
                p = q;
                which = i;
                break;
            }
        }
    }
    if (which < 0) {
        p = start;
        return (value_t){.is_str = false, .n = NAN, .s = (const char *)1};  // not a builtin
    }
    switch (which) {
    case 0: return num(fabs(one_num_arg()));
    case 1: return num(floor(one_num_arg()));
    case 2: return num(trunc(one_num_arg()));
    case 3: {
        double d = one_num_arg();
        return num(d > 0 ? 1 : d < 0 ? -1 : 0);
    }
    case 4: {
        double d = one_num_arg();
        if (d < 0)
            error(E_ILLEGAL_CALL);
        return num(sqrt(d));
    }
    case 5: return num(sin(one_num_arg()));
    case 6: return num(cos(one_num_arg()));
    case 7: return num(tan(one_num_arg()));
    case 8: return num(atan(one_num_arg()));
    case 9: {
        double d = exp(one_num_arg());
        if (isinf(d))
            error(E_OVERFLOW);
        return num(d);
    }
    case 10: {
        double d = one_num_arg();
        if (d <= 0)
            error(E_ILLEGAL_CALL);
        return num(log(d));
    }
    case 11: return num(slen(want_str(paren_expr())));
    case 12:
    case 13: {
        expect("(");
        const char *s = str_expr();
        expect(",");
        int n = arg_int();
        expect(")");
        if (n < 0)
            error(E_ILLEGAL_CALL);
        int len = slen(s);
        if (n > len)
            n = len;
        return which == 12 ? str(s, n) : str(s + len - n, n);
    }
    case 14: {
        expect("(");
        const char *s = str_expr();
        expect(",");
        int start = arg_int();
        int n = STR_MAX;
        if (accept(","))
            n = arg_int();
        expect(")");
        if (start < 1 || n < 0)
            error(E_ILLEGAL_CALL);
        int len = slen(s);
        if (start > len)
            return str("", 0);
        if (n > len - start + 1)
            n = len - start + 1;
        return str(s + start - 1, n);
    }
    case 15: {
        int c = (int)lround(one_num_arg());
        if (c < 0 || c > 255)
            error(E_ILLEGAL_CALL);
        char ch = c;
        return str(&ch, 1);
    }
    case 16: {
        const char *s = want_str(paren_expr());
        if (!slen(s))
            error(E_ILLEGAL_CALL);
        return num((unsigned char)s[0]);
    }
    case 17: {
        char buf[40];
        format_number(one_num_arg(), buf, sizeof buf);
        return str(buf, strlen(buf));
    }
    case 18: {
        const char *s = want_str(paren_expr());
        while (*s == ' ')
            ++s;
        char *end;
        double d = strtod(s, &end);
        if (end == s && (s[0] == '&'))
            d = strtol(s + 2, NULL, s[1] == 'H' || s[1] == 'h' ? 16 : 8);
        return num(end == s && s[0] != '&' ? 0 : d);
    }
    case 19: {
        expect("(");
        int start = 1;
        value_t first = expr();
        const char *hay, *needle;
        if (!first.is_str) {
            start = (int)lround(first.n);
            expect(",");
            hay = str_expr();
        } else {
            hay = first.s;
        }
        expect(",");
        needle = str_expr();
        expect(")");
        if (start < 1)
            error(E_ILLEGAL_CALL);
        int len = slen(hay), nlen = slen(needle);
        if (start > len)
            return num(0);
        if (!nlen)
            return num(start);
        for (int i = start - 1; i + nlen <= len; ++i)
            if (!memcmp(hay + i, needle, nlen))
                return num(i + 1);
        return num(0);
    }
    case 20: {
        expect("(");
        int n = arg_int();
        expect(",");
        value_t v = expr();
        expect(")");
        int c = v.is_str ? (slen(v.s) ? (unsigned char)v.s[0] : -1) : (int)lround(v.n);
        if (n < 0 || n > STR_MAX || c < 0 || c > 255)
            error(E_ILLEGAL_CALL);
        char *t = temp_alloc(n);
        memset(t, c, n);
        return (value_t){.is_str = true, .s = t};
    }
    case 21: {
        int n = (int)lround(one_num_arg());
        if (n < 0 || n > STR_MAX)
            error(E_ILLEGAL_CALL);
        char *t = temp_alloc(n);
        memset(t, ' ', n);
        return (value_t){.is_str = true, .s = t};
    }
    case 22:
    case 23: {
        long n = lround(one_num_arg());
        if (n < -32768 || n > 65535)
            error(E_OVERFLOW);
        char buf[16];
        snprintf(buf, sizeof buf, which == 22 ? "%lX" : "%lo", n & 0xffff);
        return str(buf, strlen(buf));
    }
    case 24:
    case 25: {
        const char *s = want_str(paren_expr());
        int len = slen(s);
        char *t = temp_str(s, len);
        for (int i = 0; i < len; ++i)
            t[i] = which == 24 ? toupper((unsigned char)t[i]) : tolower((unsigned char)t[i]);
        return (value_t){.is_str = true, .s = t};
    }
    case 26: {
        paren_expr();
        size_t free_mem = port_free_mem();
        return num(free_mem > HEAP_MARGIN ? free_mem - HEAP_MARGIN : 0);
    }
    case 27:
        paren_expr();
        return num(out_col + 1);
    case 31: {
        expect("(");
        double x = num_expr();
        if (accept(")")) {  // POINT(n): where the graphics cursor is
            int n = (int)lround(x);
            if (n < 0 || n > 3)
                error(E_ILLEGAL_CALL);
            return num(point_cursor(n));
        }
        expect(",");
        double y = num_expr();
        expect(")");
        return num(gfx_point(px_of(x), py_of(y)));
    }
    case 32: {
        expect("(");
        int r = arg_int();
        expect(",");
        int g = arg_int();
        expect(",");
        int b = arg_int();
        expect(")");
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
            error(E_ILLEGAL_CALL);
        return num(256 + ((r * 7 + 127) / 255 << 5 | (g * 7 + 127) / 255 << 2 | (b * 3 + 127) / 255));
    }
    case 28: {
        double d = one_num_arg();
        if (d < -32768.5 || d > 32767.5)
            error(E_OVERFLOW);
        return num(lround(d));
    }
    case 33: {
        expect("(");
        int pin = pin_arg();
        expect(")");
        return num(port_pin_read(pin));
    }
    case 34: {
        expect("(");
        int pin = pin_arg();
        expect(")");
        int v = port_adc(pin);
        if (v < 0) {
            out_flush();
            printf("GP%d has no ADC (GP40-46 do)\n", pin);
            error(E_ILLEGAL_CALL);
        }
        return num(v);
    }
    case 35:
        expect("(");
        expect(")");
        return num(port_temperature());
    case 36: {  // EOF(n)
        expect("(");
        bfile_t *b = open_file_arg();
        expect(")");
        if (b->mode == FM_RANDOM)
            return num(b->past_end ? -1 : 0);
        if (b->mode == FM_COM)  // nothing waiting
            return num(b->peek == -2 && !port_serial_waiting() ? -1 : 0);
        if (b->mode != FM_INPUT)
            return num(-1);
        return num(file_peek(b) < 0 ? -1 : 0);
    }
    case 37: {  // LOF(n)
        expect("(");
        bfile_t *b = open_file_arg();
        expect(")");
        if (b->mode == FM_COM)  // room left in the receive buffer
            return num(port_serial_room());
        return num(f_size(&b->f));
    }
    case 38: {  // LOC(n): the last record, or for text files 128-byte blocks
        expect("(");
        bfile_t *b = open_file_arg();
        expect(")");
        if (b->mode == FM_COM)  // bytes waiting
            return num(port_serial_waiting() + (b->peek >= 0));
        return num(b->mode == FM_RANDOM ? b->record : f_tell(&b->f) / 128);
    }
    case 39: {  // INPUT$(n [, [#]f])
        expect("(");
        int n = arg_int();
        bfile_t *b = NULL;
        if (accept(","))
            b = reading_file();
        expect(")");
        if (n < 0 || n > STR_MAX)
            error(E_ILLEGAL_CALL);
        char *t = temp_alloc(n);
        for (int i = 0; i < n; ++i) {
            int c;
            if (b) {
                if ((c = file_getc(b)) < 0)
                    error(E_INPUT_PAST_END);
            } else {
                out_flush();
                while ((c = key_now()) < 0) {
                    check_break();
                    port_sleep_ms(5);
                }
            }
            t[i] = c;
        }
        t[n] = 0;
        return (value_t){.is_str = true, .s = t};
    }
    case 40: {  // MKI$(n): 2-byte little-endian integer
        long n = lround(one_num_arg());
        if (n < -32768 || n > 32767)
            error(E_OVERFLOW);
        char b[2] = {(char)(n & 0xff), (char)((n >> 8) & 0xff)};
        return str(b, 2);
    }
    case 41:
    case 42: {  // MKS$(x), MKD$(x): Microsoft Binary Format, as GW-BASIC
        char b[8];
        int size = which == 41 ? 4 : 8;
        to_mbf(one_num_arg(), b, size);
        return str(b, size);
    }
    case 43:
    case 44:
    case 45: {  // CVI, CVS, CVD
        const char *s = want_str(paren_expr());
        int size = which == 43 ? 2 : which == 44 ? 4 : 8;
        if (slen(s) < size)
            error(E_ILLEGAL_CALL);
        if (which == 43)
            return num((int16_t)((uint8_t)s[0] | (uint8_t)s[1] << 8));
        return num(from_mbf(s, size));
    }
    default: return num(one_num_arg());
    }
}

static value_t primary(void) {
    skip_spaces();
    char c = *p;
    if (c == '"') {
        const char *s = ++p;
        while (*p && *p != '"')
            ++p;
        int len = p - s;
        if (*p == '"')
            ++p;
        return str(s, len);
    }
    if (isdigit((unsigned char)c) || c == '.' || c == '&')
        return num(read_number_literal());
    if (c == '(')
        return paren_expr();
    if (accept("FN")) {
        char name[NAME_MAX + 2];
        if (!read_name(name))
            error(E_SYNTAX);
        return call_fn(name);
    }
    value_t b = builtin();
    if (!(isnan(b.n) && b.s == (const char *)1))
        return b;
    char name[NAME_MAX + 2];
    if (!read_name(name))
        error(E_SYNTAX);
    skip_spaces();
    if (*p == '(') {
        ++p;
        bool is_str;
        void *slot = array_element(name, &is_str);
        if (is_str) {
            const char *s = *(char **)slot;
            return (value_t){.is_str = true, .s = s ? s : EMPTY};
        }
        return num(*(double *)slot);
    }
    var_t *v = find_var(name);
    if (v->is_str)
        return (value_t){.is_str = true, .s = v->s ? v->s : EMPTY};
    return num(v->n);
}

static value_t power(void) {
    value_t a = primary();
    while (accept("^")) {
        double x = want_num(a);
        double y;
        skip_spaces();
        if (*p == '-') {
            ++p;
            y = -want_num(primary());
        } else {
            y = want_num(primary());
        }
        if (x == 0 && y < 0)
            error(E_DIV_ZERO);
        double r = pow(x, y);
        if (isnan(r))
            error(E_ILLEGAL_CALL);
        a = num(r);
    }
    return a;
}

static value_t unary(void) {
    skip_spaces();
    if (*p == '-') {
        ++p;
        return num(-want_num(unary()));
    }
    if (*p == '+') {
        ++p;
        return unary();
    }
    return power();
}

static value_t term(void) {
    value_t a = unary();
    for (;;) {
        skip_spaces();
        char op = *p;
        if (op != '*' && op != '/')
            return a;
        ++p;
        double x = want_num(a), y = want_num(unary());
        if (op == '/' && y == 0)
            error(E_DIV_ZERO);
        a = num(op == '*' ? x * y : x / y);
    }
}

static value_t int_div(void) {
    value_t a = term();
    while (accept("\\")) {
        double y = round(want_num(term()));
        if (y == 0)
            error(E_DIV_ZERO);
        a = num(trunc(round(want_num(a)) / y));
    }
    return a;
}

static value_t mod_expr(void) {
    value_t a = int_div();
    while (accept("MOD")) {
        double y = round(want_num(int_div()));
        if (y == 0)
            error(E_DIV_ZERO);
        a = num(fmod(round(want_num(a)), y));
    }
    return a;
}

static value_t additive(void) {
    value_t a = mod_expr();
    for (;;) {
        skip_spaces();
        char op = *p;
        if (op != '+' && op != '-')
            return a;
        ++p;
        value_t b = mod_expr();
        if (op == '+' && a.is_str) {
            const char *y = want_str(b);
            int la = slen(a.s), lb = slen(y);
            char *t = temp_alloc(la + lb);
            memcpy(t, a.s, la);
            memcpy(t + la, y, lb);
            a = (value_t){.is_str = true, .s = t};
        } else {
            double x = want_num(a), y = want_num(b);
            a = num(op == '+' ? x + y : x - y);
        }
    }
}

/// Relational results are -1 (true) or 0, as in GW-BASIC.
static value_t relational(void) {
    value_t a = additive();
    for (;;) {
        skip_spaces();
        int op = 0;  // bit 1 <, bit 2 =, bit 4 >
        while (*p == '<' || *p == '=' || *p == '>') {
            op |= *p == '<' ? 1 : *p == '=' ? 2 : 4;
            ++p;
            skip_spaces();
        }
        if (!op)
            return a;
        value_t b = additive();
        int cmp;
        if (a.is_str != b.is_str)
            error(E_TYPE_MISMATCH);
        if (a.is_str) {
            int la = slen(a.s), lb = slen(b.s);
            cmp = memcmp(a.s, b.s, la < lb ? la : lb);
            if (!cmp)
                cmp = la - lb;
        }
        else
            cmp = a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
        bool r = (cmp < 0 && (op & 1)) || (cmp == 0 && (op & 2)) || (cmp > 0 && (op & 4));
        a = num(r ? -1 : 0);
    }
}

static long to_int16ish(value_t v) {
    double d = want_num(v);
    if (d < -2147483648.0 || d > 2147483647.0)
        error(E_OVERFLOW);
    return lround(d);
}

static value_t not_expr(void) {
    if (accept("NOT"))
        return num(~to_int16ish(not_expr()));
    return relational();
}

static value_t and_expr(void) {
    value_t a = not_expr();
    while (accept("AND"))
        a = num(to_int16ish(a) & to_int16ish(not_expr()));
    return a;
}

static value_t or_expr(void) {
    value_t a = and_expr();
    for (;;) {
        if (accept("OR"))
            a = num(to_int16ish(a) | to_int16ish(and_expr()));
        else if (accept("XOR"))
            a = num(to_int16ish(a) ^ to_int16ish(and_expr()));
        else
            return a;
    }
}

static value_t expr(void) {
    // Deeply nested expressions recurse; stop before the stack runs into
    // whatever lies below it (on the Pico, core 1's stack).
    if (port_stack_low())
        error(E_OUT_OF_MEMORY);
    value_t a = or_expr();
    for (;;) {
        if (accept("EQV"))
            a = num(~(to_int16ish(a) ^ to_int16ish(or_expr())));
        else if (accept("IMP"))
            a = num(~to_int16ish(a) | to_int16ish(or_expr()));
        else
            return a;
    }
}

static value_t call_fn(const char *name) {
    fn_t *f;
    for (f = fns; f; f = f->next)
        if (!strcmp(f->name, name))
            break;
    if (!f)
        error(E_UNDEFINED_FN);
    value_t args[4];
    int n = 0;
    if (accept("(")) {
        do {
            if (n == 4)
                error(E_SYNTAX);
            args[n++] = expr();
        } while (accept(","));
        expect(")");
    }
    if (n != f->param_count)
        error(E_SYNTAX);
    const char *body = fn_body(f);
    if (!body)
        error(E_UNDEFINED_FN);  // its program isn't loaded (CHAIN)
    // Parameters are ordinary variables, saved and restored around the call.
    double saved_n[4];
    char *saved_s[4];
    var_t *pv[4];
    for (int i = 0; i < n; ++i) {
        pv[i] = find_var(f->params[i]);
        if (pv[i]->is_str != args[i].is_str)
            error(E_TYPE_MISMATCH);
        saved_n[i] = pv[i]->n;
        saved_s[i] = pv[i]->s;
        pv[i]->s = NULL;
    }
    for (int i = 0; i < n; ++i) {
        if (args[i].is_str)
            set_str(&pv[i]->s, args[i].s);
        else
            pv[i]->n = typed_value(type_of(pv[i]->name), args[i].n);
    }
    const char *saved_p = p;
    p = body;
    value_t r = expr();
    p = saved_p;
    if (r.is_str)
        r = str(r.s, slen(r.s));  // the parameter's copy is about to go
    for (int i = 0; i < n; ++i) {
        sfree(pv[i]->s);
        pv[i]->n = saved_n[i];
        pv[i]->s = saved_s[i];
    }
    if (r.is_str != (name[strlen(name) - 1] == '$'))
        error(E_TYPE_MISMATCH);
    return r;
}

// ---- Assignment targets ---------------------------------------------------

typedef struct {
    bool is_str;
    char type;  ///< '%', '!' or '#' for a number
    double *n;
    char **s;
} lvalue_t;

static char type_of(const char *name) {
    return name[strlen(name) - 1];
}

/// What a numeric variable of `type` holds when given d. A % variable rounds
/// to -32768..32767 and a ! one keeps single precision (so 0.05 added up
/// twenty times reaches 1, as in GW-BASIC); # keeps d as it is.
static double typed_value(char type, double d) {
    if (type == '%') {
        if (!(d > -32768.5 && d < 32767.5))
            error(E_OVERFLOW);
        return (double)lround(d);
    }
    if (type == '!') {
        float f = (float)d;
        if (isinf(f) && !isinf(d))
            error(E_OVERFLOW);
        return f;
    }
    return d;
}

static void set_num(lvalue_t lv, double d) {
    *lv.n = typed_value(lv.type, d);
}

static lvalue_t lvalue(void) {
    char name[NAME_MAX + 2];
    if (!read_name(name))
        error(E_SYNTAX);
    skip_spaces();
    if (*p == '(') {
        ++p;
        bool is_str;
        void *slot = array_element(name, &is_str);
        return is_str ? (lvalue_t){.is_str = true, .s = slot}
                      : (lvalue_t){.is_str = false, .type = type_of(name), .n = slot};
    }
    var_t *v = find_var(name);
    return v->is_str ? (lvalue_t){.is_str = true, .s = &v->s}
                     : (lvalue_t){.is_str = false, .type = type_of(name), .n = &v->n};
}

static void assign(lvalue_t lv, value_t v) {
    if (lv.is_str != v.is_str)
        error(E_TYPE_MISMATCH);
    if (lv.is_str)
        set_str(lv.s, v.s);
    else
        set_num(lv, v.n);
}

// ---- Statement helpers ----------------------------------------------------

/// Skips to the end of the current statement (respecting strings).
static void skip_statement(void) {
    bool in_str = false;
    while (*p) {
        if (*p == '"')
            in_str = !in_str;
        else if (!in_str && *p == ':')
            return;
        ++p;
    }
}

static void skip_line(void) {
    p += strlen(p);
}

/// Moves to the next statement start, crossing into following lines.
/// Returns false at the end of the program.
static bool next_statement(void) {
    skip_statement();
    if (*p == ':') {
        ++p;
        return true;
    }
    if (cur_line < 0 || cur_line + 1 >= line_count)
        return false;
    ++cur_line;
    p = lines[cur_line].text;
    return true;
}

/// Finds the matching NEXT (or WEND) for a FOR (WHILE) whose body is skipped.
static void skip_block(const char *open, const char *close, int err) {
    int depth = 1;
    for (;;) {
        if (!next_statement())
            error(err);
        skip_spaces();
        if (!strncmp(p, open, strlen(open)) &&
            !(open[0] == 'F' && !strncmp(p, "FN", 2))) {
            ++depth;
        } else if (!strncmp(p, close, strlen(close))) {
            if (--depth == 0) {
                p += strlen(close);
                if (close[0] == 'N') {
                    // NEXT may name the variable; skip it (and its siblings).
                    skip_statement();
                }
                return;
            }
        }
    }
}

static void print_error(int code) {
    out_flush();
    if (out_col)
        out_newline();
    if (code == E_BREAK) {
        out_str("Break");
    } else {
        const char *text = "Unprintable error";
        for (size_t i = 0; i < sizeof error_texts / sizeof *error_texts; ++i)
            if (error_texts[i].code == code)
                text = error_texts[i].text;
        out_str(text);
    }
    int ln = running ? current_line_number() : -1;
    if (ln >= 0)
        last_line_num = ln;
    if (ln >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, " in %d", ln);
        out_str(buf);
    }
    out_newline();
}

static void check_break(void) {
    read_keys(running);
    if (break_pending) {
        break_pending = false;
        error(E_BREAK);
    }
}

// ---- File helpers ---------------------------------------------------------

static void file_name_arg(char *out, int size, bool add_bas) {
    const char *s = str_expr();
    if (!*s)
        error(E_BAD_FILE_NAME);
    snprintf(out, size, "%s", s);
    if (add_bas && !strchr(out, '.') && strlen(out) + 4 < (size_t)size)
        strcat(out, ".BAS");
}

static bool load_plain_line(char *line);
static bool load_tokenised(const uint8_t *buf, size_t len);

/// Reads plain-text programs a chunk at a time, so loading needs no more
/// memory than the program itself. GW-BASIC tokenised files (first byte 0xFF,
/// at most 64 KB) are read whole and converted.
enum { LOAD_MERGE = 1, LOAD_KEEP_VARS = 2 };

/// Loads a program (LOAD_MERGE: adds a plain-text one's lines to the program
/// in memory; LOAD_KEEP_VARS: leaves the variables, for CHAIN).
static void load_file(const char *name, int flags) {
    FIL f;  // (on the stack: kept across the longjmp below, as it's in memory)
    FRESULT r = f_open(&f, name, FA_READ);
    if (r != FR_OK)
        error(fresult_error(r));
    if (!(flags & LOAD_MERGE))
        clear_program();
    if (!(flags & LOAD_KEEP_VARS))
        clear_vars();
    reset_run_state();
    static char *whole;
    whole = NULL;
    int err = 0;
    jmp_buf saved;
    memcpy(saved, error_jump, sizeof saved);
    if ((err = setjmp(error_jump)) == 0) {
        char chunk[512];
        UINT got;
        r = f_read(&f, chunk, sizeof chunk, &got);
        if (r != FR_OK)
            error(fresult_error(r));
        if (got && (uint8_t)chunk[0] == 0xFE)
            error(E_PROTECTED);
        if (got && (uint8_t)chunk[0] == 0xFF) {
            if (flags & LOAD_MERGE)
                error(E_BAD_FILE_MODE);  // as GW-BASIC: MERGE takes ASCII files only
            size_t size = f_size(&f);
            whole = checked_malloc(size + 1);
            memcpy(whole, chunk, got);
            UINT more = 0;
            if (size > got && (r = f_read(&f, whole + got, size - got, &more)) != FR_OK)
                error(fresult_error(r));
            if (!load_tokenised((const uint8_t *)whole, got + more))
                error(E_BAD_FILE_MODE);
        } else {
            char line[LINE_MAX_LEN + 8];
            int n = 0;
            bool done = false;
            while (got && !done) {
                for (UINT i = 0; i < got && !done; ++i) {
                    char c = chunk[i];
                    if (c == 0x1a) {
                        done = true;
                    } else if (c == '\n' || c == '\r') {
                        line[n] = 0;
                        if (!load_plain_line(line))
                            error(E_BAD_FILE_MODE);
                        n = 0;
                    } else if (n < (int)sizeof line - 1) {
                        line[n++] = c;
                    }
                }
                if (!done && (r = f_read(&f, chunk, sizeof chunk, &got)) != FR_OK)
                    error(fresult_error(r));
            }
            line[n] = 0;
            if (!load_plain_line(line))
                error(E_BAD_FILE_MODE);
        }
    }
    memcpy(error_jump, saved, sizeof saved);
    free(whole);
    f_close(&f);
    if (err) {
        if (!(flags & LOAD_MERGE))
            clear_program();  // don't leave half a program behind
        error(err);
    }
    if (!(flags & LOAD_MERGE))
        snprintf(program_name, sizeof program_name, "%s", name);
    cont_pos.line = -2;
}

/// One line of a plain-text program; false if it has no line number.
static bool load_plain_line(char *line) {
    char *q = line;
    while (*q == ' ')
        ++q;
    if (!*q)
        return true;
    if (!isdigit((unsigned char)*q))
        return false;  // "Direct statement in file"
    long num = strtol(q, &q, 10);
    if (num > 65529)
        return false;
    if (*q == ' ')
        ++q;
    if (strlen(q) > LINE_MAX_LEN)
        q[LINE_MAX_LEN] = 0;
    if (*q)
        store_line(num, q);
    return true;
}

/// GW-BASIC tokenised format: 0xFF, then per line a 2-byte link, a 2-byte line
/// number, tokens, and a 0 byte; a 0 link ends the program.
static const char *const tok1[] = {
    [0x81] = "END", "FOR", "NEXT", "DATA", "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF",
    "RESTORE", "GOSUB", "RETURN", "REM", "STOP", "PRINT", "CLEAR", "LIST", "NEW", "ON", "WAIT",
    "DEF", "POKE", "CONT", NULL, NULL, "OUT", "LPRINT", "LLIST", NULL, "WIDTH", "ELSE", "TRON",
    "TROFF", "SWAP", "ERASE", "EDIT", "ERROR", "RESUME", "DELETE", "AUTO", "RENUM", "DEFSTR",
    "DEFINT", "DEFSNG", "DEFDBL", "LINE", "WHILE", "WEND", "CALL", NULL, NULL, NULL, "WRITE",
    "OPTION", "RANDOMIZE", "OPEN", "CLOSE", "LOAD", "MERGE", "SAVE", "COLOR", "CLS", "MOTOR",
    "BSAVE", "BLOAD", "SOUND", "BEEP", "PSET", "PRESET", "SCREEN", "KEY", "LOCATE", NULL, "TO",
    "THEN", "TAB(", "STEP", "USR", "FN", "SPC(", "NOT", "ERL", "ERR", "STRING$", "USING",
    "INSTR", "'", "VARPTR", "CSRLIN", "POINT", "OFF", "INKEY$", NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, ">", "=", "<", "+", "-", "*", "/", "^", "AND", "OR", "XOR", "EQV", "IMP", "MOD",
    "\\",
};
static const char *const tok_fe[] = {
    [0x81] = "FILES", "FIELD", "SYSTEM", "NAME", "LSET", "RSET", "KILL", "PUT", "GET", "RESET",
    "COMMON", "CHAIN", "DATE$", "TIME$", "PAINT", "COM", "CIRCLE", "DRAW", "PLAY", "TIMER",
    "ERDEV", "IOCTL", "CHDIR", "MKDIR", "RMDIR", "SHELL", "ENVIRON", "VIEW", "WINDOW", "PMAP",
    "PALETTE", "LCOPY", "CALLS",
};
static const char *const tok_ff[] = {
    [0x81] = "LEFT$", "RIGHT$", "MID$", "SGN", "INT", "ABS", "SQR", "RND", "SIN", "LOG", "EXP",
    "COS", "TAN", "ATN", "FRE", "INP", "POS", "LEN", "STR$", "VAL", "ASC", "CHR$", "PEEK",
    "SPACE$", "OCT$", "HEX$", "LPOS", "CINT", "CSNG", "CDBL", "FIX", "PEN", "STICK", "STRIG",
    "EOF", "LOC", "LOF",
};
static const char *const tok_fd[] = {
    [0x81] = "CVI", "CVS", "CVD", "MKI$", "MKS$", "MKD$",
};

static double mbf_to_double(const uint8_t *b, int bytes) {
    int exp = b[bytes - 1];
    if (!exp)
        return 0;
    bool neg = b[bytes - 2] & 0x80;
    double m = (b[bytes - 2] | 0x80);
    for (int i = bytes - 3; i >= 0; --i)
        m = m * 256 + b[i];
    // The mantissa is 0.1xxx in binary with (bytes - 1) * 8 bits.
    double v = ldexp(m, exp - 128 - (bytes - 1) * 8);
    return neg ? -v : v;
}

static bool load_tokenised(const uint8_t *b, size_t len) {
    size_t i = 1;
    char text[LINE_MAX_LEN * 2];
    while (i + 4 <= len) {
        unsigned link = b[i] | b[i + 1] << 8;
        if (!link)
            break;
        unsigned num = b[i + 2] | b[i + 3] << 8;
        i += 4;
        int n = 0;
        bool in_str = false;
        while (i < len && b[i]) {
            uint8_t c = b[i++];
            char tmp[32];
            const char *add = NULL;
            if (in_str || (c >= 0x20 && c < 0x7f)) {
                if (c == '"')
                    in_str = !in_str;
                if (c == ':' && !in_str && i < len && (b[i] == 0xA1 || (b[i] == 0x8F && i + 1 < len && b[i + 1] == 0xD9)))
                    continue;  // ":ELSE" and ":REM'" are stored with a leading colon
                tmp[0] = c;
                tmp[1] = 0;
                add = tmp;
            } else if (c >= 0x11 && c <= 0x1B) {
                snprintf(tmp, sizeof tmp, "%d", c - 0x11);
                add = tmp;
            } else if (c == 0x0F && i < len) {
                snprintf(tmp, sizeof tmp, "%d", b[i++]);
                add = tmp;
            } else if ((c == 0x1C || c == 0x0E || c == 0x0D || c == 0x0B || c == 0x0C) && i + 1 < len) {
                unsigned v = b[i] | b[i + 1] << 8;
                i += 2;
                if (c == 0x1C)
                    snprintf(tmp, sizeof tmp, "%d", (int16_t)v);
                else if (c == 0x0B)
                    snprintf(tmp, sizeof tmp, "&O%o", v);
                else if (c == 0x0C)
                    snprintf(tmp, sizeof tmp, "&H%X", v);
                else
                    snprintf(tmp, sizeof tmp, "%u", v);
                add = tmp;
            } else if (c == 0x1D && i + 3 < len) {
                snprintf(tmp, sizeof tmp, "%.7G", mbf_to_double(b + i, 4));
                i += 4;
                add = tmp;
            } else if (c == 0x1F && i + 7 < len) {
                snprintf(tmp, sizeof tmp, "%.16G#", mbf_to_double(b + i, 8));
                i += 8;
                add = tmp;
            } else if ((c == 0xFD || c == 0xFE || c == 0xFF) && i < len) {
                uint8_t t = b[i++];
                const char *const *table = c == 0xFD ? tok_fd : c == 0xFE ? tok_fe : tok_ff;
                size_t count = c == 0xFD ? sizeof tok_fd / sizeof *tok_fd
                             : c == 0xFE ? sizeof tok_fe / sizeof *tok_fe
                                         : sizeof tok_ff / sizeof *tok_ff;
                add = t < count ? table[t] : NULL;
                if (!add)
                    return false;
            } else if (c >= 0x81 && c < sizeof tok1 / sizeof *tok1 && tok1[c]) {
                add = tok1[c];
                if (!strcmp(add, "WHILE") && i < len && b[i] == 0xE9)
                    ++i;  // WHILE is stored with a + token after it
                if (c == 0x8F && i < len && b[i] == 0xD9) {  // REM + ' is how ' is stored
                    add = "'";
                    ++i;
                }
                if (c == 0x8F || c == 0x84) {  // REM / DATA: the rest is literal
                    int l = strlen(add);
                    if (n + l < (int)sizeof text - 1) {
                        memcpy(text + n, add, l);
                        n += l;
                    }
                    while (i < len && b[i] && n < (int)sizeof text - 1) {
                        if (c == 0x84 && b[i] == ':')
                            break;
                        text[n++] = b[i++];
                    }
                    continue;
                }
            } else {
                return false;
            }
            int l = strlen(add);
            if (n + l < (int)sizeof text - 1) {
                memcpy(text + n, add, l);
                n += l;
            }
        }
        ++i;  // the 0 at the end of the line
        text[n] = 0;
        if (n > LINE_MAX_LEN)
            text[LINE_MAX_LEN] = 0;
        if (num > 65529)
            return false;
        if (text[0])
            store_line(num, text);
    }
    return true;
}

static void save_file(const char *name) {
    FIL f;
    FRESULT r = f_open(&f, name, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK)
        error(fresult_error(r));
    for (int i = 0; i < line_count && r == FR_OK; ++i) {
        char buf[LINE_MAX_LEN + 16];
        int n = snprintf(buf, sizeof buf, "%u %s\r\n", lines[i].num, lines[i].text);
        UINT put;
        r = f_write(&f, buf, n, &put);
        if (r == FR_OK && put != (UINT)n)
            r = FR_DENIED;
    }
    FRESULT rc = f_close(&f);
    if (r == FR_OK)
        r = rc;
    if (r != FR_OK)
        error(r == FR_DENIED ? E_DISK_FULL : fresult_error(r));
    snprintf(program_name, sizeof program_name, "%s", name);
}

// ---- Statements -----------------------------------------------------------

static bfile_t *writing_file(void);
static void begin_file_output(bfile_t *b);
static void end_file_output(bfile_t *b);

static void print_items(void);

/// PRINT [#n,] items
static void print_using(void);

static void do_print(void) {
    skip_spaces();
    if (*p != '#') {
        if (accept_word("USING"))
            print_using();
        else
            print_items();
        return;
    }
    bfile_t *b = writing_file();
    expect(",");
    begin_file_output(b);
    if (accept_word("USING"))
        print_using();
    else
        print_items();
    end_file_output(b);
}

// ---- PRINT USING --------------------------------------------------------------

/// A numeric field of a PRINT USING format.
typedef struct {
    int digits;       ///< positions before the point, including ** and $$ ones
    int decimals;     ///< digits after the point
    bool point, comma, star, dollar, plus_lead, plus_trail, minus_trail;
    int exp_digits;   ///< 0, or 2/3 for ^^^^ / ^^^^^
} num_field_t;

/// Parses a numeric field at f[*i] (moving *i past it). False if there isn't
/// one there.
static bool parse_num_field(const char *f, int n, int *i, num_field_t *nf) {
    int k = *i;
    memset(nf, 0, sizeof *nf);
    if (k < n && f[k] == '+' && k + 1 < n &&
        (f[k + 1] == '#' || f[k + 1] == '.' || f[k + 1] == '*' || f[k + 1] == '$')) {
        nf->plus_lead = true;
        ++k;
    }
    bool any = false;
    if (k + 2 < n && f[k] == '*' && f[k + 1] == '*' && f[k + 2] == '$') {
        nf->star = nf->dollar = true;
        nf->digits = 3;
        k += 3;
        any = true;
    } else if (k + 1 < n && f[k] == '*' && f[k + 1] == '*') {
        nf->star = true;
        nf->digits = 2;
        k += 2;
        any = true;
    } else if (k + 1 < n && f[k] == '$' && f[k + 1] == '$') {
        nf->dollar = true;
        nf->digits = 2;
        k += 2;
        any = true;
    }
    while (k < n && (f[k] == '#' || (f[k] == ',' && (any || nf->digits)))) {
        if (f[k] == ',')
            nf->comma = true;
        ++nf->digits;
        ++k;
        any = true;
    }
    if (k < n && f[k] == '.' && (any || (k + 1 < n && f[k + 1] == '#'))) {
        nf->point = true;
        ++k;
        while (k < n && f[k] == '#') {
            ++nf->decimals;
            ++k;
        }
        if (nf->decimals > 20)
            nf->decimals = 20;
        any = true;
    }
    if (!any)
        return false;
    if (k + 3 < n && !strncmp(f + k, "^^^^", 4)) {
        nf->exp_digits = 2;
        k += 4;
        if (k < n && f[k] == '^') {
            nf->exp_digits = 3;
            ++k;
        }
    }
    if (!nf->plus_lead && k < n && (f[k] == '+' || f[k] == '-')) {
        if (f[k] == '+')
            nf->plus_trail = true;
        else
            nf->minus_trail = true;
        ++k;
    }
    *i = k;
    return true;
}

/// "\  \": a string field n + 2 characters wide.
static bool backslash_field(const char *f, int n, int i) {
    if (i >= n || f[i] != '\\')
        return false;
    int k = i + 1;
    while (k < n && f[k] == ' ')
        ++k;
    return k < n && f[k] == '\\';
}

static void format_using_number(double v, const num_field_t *nf, char *out, size_t size) {
    bool neg = v < 0;
    double a = fabs(v);
    bool sign_elsewhere = nf->plus_lead || nf->plus_trail || nf->minus_trail;
    char body[192];
    int width = nf->digits;  // the room before the point
    if (width > 60)
        width = 60;  // (keeps the buffers below big enough)
    if (nf->exp_digits) {
        // Exponential: as many digits before the point as there are #s,
        // less one for the sign unless the sign has its own place.
        int k = nf->digits - (sign_elsewhere ? 0 : 1);
        if (k < 0)
            k = 0;
        int e = 0;
        double m = a;
        if (a != 0) {
            e = (int)floor(log10(a)) - (k ? k - 1 : -1);
            m = a / pow(10, e);
            double r = pow(10, nf->decimals);
            m = round(m * r) / r;
            if (m >= pow(10, k ? k : 0) && k) {
                m /= 10;
                ++e;
            } else if (!k && m >= 1) {
                m /= 10;
                ++e;
            }
        }
        char mant[64];
        snprintf(mant, sizeof mant, "%.*f", nf->decimals, m);
        if (!k && mant[0] == '0')
            memmove(mant, mant + 1, strlen(mant));  // ".23", no digit before the point
        if (!nf->point)
            mant[strcspn(mant, ".")] = 0;
        snprintf(body, sizeof body, "%s%sE%c%0*d", (neg && !sign_elsewhere) ? "-" : "", mant,
                 e < 0 ? '-' : '+', nf->exp_digits, abs(e));
        int pad = width + (nf->point ? 1 + nf->decimals : 0) + nf->exp_digits + 2 - (int)strlen(body);
        if (pad > 60)
            pad = 60;
        char padded[260];
        snprintf(padded, sizeof padded, "%*s%s", pad > 0 ? pad : 0, "", body);
        snprintf(body, sizeof body, "%.190s", padded);
    } else {
        char num[64];
        snprintf(num, sizeof num, "%.*f", nf->decimals, a);
        if (nf->decimals == 0 && nf->point)
            strcat(num, ".");
        char *dot = strchr(num, '.');
        char ip[64], fp[32] = "";
        snprintf(ip, sizeof ip, "%.*s", dot ? (int)(dot - num) : (int)strlen(num), num);
        if (dot)
            snprintf(fp, sizeof fp, "%s", dot);
        // A zero before the point only if there's room for it.
        if (!strcmp(ip, "0") && nf->point && width <= (nf->dollar ? 1 : 0) + (neg && !sign_elsewhere))
            ip[0] = 0;
        char grouped[96];
        int g = 0, len = strlen(ip);
        for (int i = 0; i < len; ++i) {
            grouped[g++] = ip[i];
            if (nf->comma && (len - i - 1) % 3 == 0 && i < len - 1)
                grouped[g++] = ',';
        }
        grouped[g] = 0;
        // A leading + field puts the sign right against the number.
        const char *sign = nf->plus_lead ? (neg ? "-" : "+") : (neg && !sign_elsewhere) ? "-" : "";
        if (nf->plus_lead)
            ++width;
        char left[100];
        snprintf(left, sizeof left, "%s%s%s", sign, nf->dollar ? "$" : "", grouped);
        int l = strlen(left);
        char padded[260];
        if (l > width) {
            snprintf(padded, sizeof padded, "%%%s", left);  // doesn't fit
        } else {
            memset(padded, nf->star ? '*' : ' ', width - l);
            snprintf(padded + width - l, sizeof padded - (width - l), "%s", left);
        }
        snprintf(body, sizeof body, "%.160s%.30s", padded, nf->point ? fp : "");
    }
    snprintf(out, size, "%s%s%s", nf->plus_lead && nf->exp_digits ? (neg ? "-" : "+") : "", body,
             nf->plus_trail ? (neg ? "-" : "+") : nf->minus_trail ? (neg ? "-" : " ") : "");
}

/// PRINT USING fmt$; values [;|,]: the format string is used again from the
/// start if there are more values than fields.
static void print_using(void) {
    const char *f = str_expr();
    int n = slen(f);
    char fmt[256];
    memcpy(fmt, f, n);  // the temporary may be reused by the values
    f = fmt;
    if (!accept(";"))
        expect(",");
    bool newline = true, has_field = false;
    for (int k = 0; k < n && !has_field; ++k) {
        num_field_t nf;
        int j = k;
        if (f[k] == '_') {
            ++k;
            continue;
        }
        has_field = f[k] == '!' || f[k] == '&' || backslash_field(f, n, k) ||
                    parse_num_field(f, n, &j, &nf);
    }
    if (!has_field)
        error(E_ILLEGAL_CALL);
    int i = 0;
    for (;;) {
        // Literal text up to the next field.
        for (;;) {
            if (i >= n) {
                i = 0;  // more values than fields: start the format again
                continue;
            }
            num_field_t nf;
            int j = i;
            if (f[i] == '_' && i + 1 < n) {
                out_char(f[i + 1]);
                i += 2;
                continue;
            }
            if (f[i] == '!' || f[i] == '&' || backslash_field(f, n, i) ||
                parse_num_field(f, n, &j, &nf))
                break;
            out_char(f[i++]);
        }
        if (at_end_of_statement())
            break;
        // The field and its value.
        value_t v = expr();
        num_field_t nf;
        int j = i;
        if (f[i] == '!' || f[i] == '&' || backslash_field(f, n, i)) {
            if (!v.is_str)
                error(E_TYPE_MISMATCH);
            int w;
            if (f[i] == '!') {
                w = 1;
                ++i;
            } else if (f[i] == '&') {
                w = -1;
                ++i;
            } else {
                int k = i + 1;
                while (f[k] == ' ')
                    ++k;
                w = k - i + 1;
                i = k + 1;
            }
            int len = slen(v.s);
            if (w < 0) {
                out_bytes(v.s, len);
            } else {
                out_bytes(v.s, len < w ? len : w);
                for (int k = len; k < w; ++k)
                    out_char(' ');
            }
        } else {
            parse_num_field(f, n, &j, &nf);
            i = j;
            if (v.is_str)
                error(E_TYPE_MISMATCH);
            char buf[256];
            format_using_number(v.n, &nf, buf, sizeof buf);
            out_str(buf);
        }
        newline = true;
        if (accept(";") || accept(",")) {
            newline = false;
            if (at_end_of_statement())
                break;
        } else if (!at_end_of_statement()) {
            error(E_SYNTAX);
        }
        if (at_end_of_statement())
            break;
    }
    // After the last value, the literal text up to the next field.
    if (i > 0)
        while (i < n) {
            num_field_t nf;
            int j = i;
            if (f[i] == '!' || f[i] == '&' || backslash_field(f, n, i) ||
                parse_num_field(f, n, &j, &nf))
                break;
            if (f[i] == '_' && i + 1 < n)
                ++i;
            out_char(f[i++]);
        }
    if (newline)
        out_newline();
    out_flush();
}

static void print_items(void) {
    int width = out_dev ? 255 : SCREEN_COLS;  // files don't wrap at 80
    bool newline = true;
    for (;;) {
        if (at_end_of_statement())
            break;
        newline = true;
        if (accept(";")) {
            newline = false;
            continue;
        }
        if (accept_word("USING")) {  // PRINT "x"; USING "##"; n (the rest is formatted)
            print_using();
            return;
        }
        if (accept(",")) {
            newline = false;
            int next = (out_col / 14 + 1) * 14;
            if (next >= width - 10) {
                out_newline();
            } else {
                while (out_col < next)
                    out_char(' ');
            }
            continue;
        }
        if (accept("TAB(")) {
            int n = arg_int();
            expect(")");
            if (n < 1)
                n = 1;
            n = (n - 1) % width;
            if (out_col > n)
                out_newline();
            while (out_col < n)
                out_char(' ');
            newline = false;
            continue;
        }
        if (accept("SPC(")) {
            int n = arg_int();
            expect(")");
            for (int i = 0; i < n; ++i)
                out_char(' ');
            newline = false;
            continue;
        }
        value_t v = expr();
        if (v.is_str) {
            out_bytes(v.s, slen(v.s));
        } else {
            char buf[40];
            format_number(v.n, buf, sizeof buf);
            if (out_col + (int)strlen(buf) >= width && out_col)
                out_newline();
            out_str(buf);
            out_char(' ');
        }
        newline = true;
        skip_spaces();
        if (*p != ';' && *p != ',' && !at_end_of_statement())
            continue;  // "PRINT A B" prints both
    }
    if (newline)
        out_newline();
    out_flush();
}

static void do_input(bool line_input) {
    skip_spaces();
    bool question = true;
    char prompt[LINE_MAX_LEN + 1] = "";
    if (*p == '"') {
        const char *s = ++p;
        while (*p && *p != '"')
            ++p;
        snprintf(prompt, sizeof prompt, "%.*s", (int)(p - s), s);
        if (*p == '"')
            ++p;
        if (accept(","))
            question = false;
        else
            expect(";");
    }
    const char *targets = p;
    for (;;) {
        p = targets;
        out_str(prompt);
        if (question && !line_input)
            out_str("? ");
        char buf[LINE_MAX_LEN + 1];
        if (!read_line(buf, sizeof buf, NULL))
            error(E_BREAK);
        if (line_input) {
            assign(lvalue(), str(buf, strlen(buf)));
            return;
        }
        const char *in = buf;
        bool redo = false;
        do {
            lvalue_t lv = lvalue();
            while (*in == ' ')
                ++in;
            char item[LINE_MAX_LEN + 1];
            int n = 0;
            if (*in == '"') {
                ++in;
                while (*in && *in != '"')
                    item[n++] = *in++;
                if (*in == '"')
                    ++in;
                while (*in == ' ')
                    ++in;
            } else {
                while (*in && *in != ',')
                    item[n++] = *in++;
                while (n && item[n - 1] == ' ')
                    --n;
            }
            item[n] = 0;
            if (lv.is_str) {
                set_strn(lv.s, item, strlen(item));
            } else {
                char *end;
                double d = strtod(item, &end);
                while (*end == ' ')
                    ++end;
                if (*end || (!n))
                    if (*end) {
                        redo = true;
                        break;
                    }
                set_num(lv, n ? d : 0);
            }
            if (accept(",")) {
                if (*in != ',') {
                    redo = true;  // too few items
                    break;
                }
                ++in;
            } else if (*in) {
                redo = true;  // "?Extra ignored" in GW-BASIC; redo is stricter
                break;
            }
        } while (!at_end_of_statement() || false);
        if (!redo)
            return;
        out_str("?Redo from start");
        out_newline();
    }
}

static void do_for(void) {
    char name[NAME_MAX + 2];
    if (!read_name(name))
        error(E_SYNTAX);
    var_t *v = find_var(name);
    if (v->is_str)
        error(E_TYPE_MISMATCH);
    expect("=");
    v->n = typed_value(type_of(name), num_expr());
    expect("TO");
    double limit = num_expr();
    double step = 1;
    if (accept("STEP"))
        step = num_expr();
    // A FOR on a variable already looping replaces that loop.
    for (int i = for_depth - 1; i >= for_base(); --i) {
        if (for_stack[i].var == v) {
            for_depth = i;
            break;
        }
    }
    if ((step >= 0 && v->n > limit) || (step < 0 && v->n < limit)) {
        skip_block("FOR", "NEXT", E_FOR_WITHOUT_NEXT);
        return;
    }
    if (for_depth == STACK_MAX)
        error(E_OUT_OF_MEMORY);
    for_stack[for_depth++] = (for_frame_t){v, limit, step, {cur_line, p}};
}

static void do_next(void) {
    for (;;) {
        var_t *v = NULL;
        char name[NAME_MAX + 2];
        if (read_name(name))
            v = find_var(name);
        int i = for_depth - 1, base = for_base();
        if (v)
            while (i >= base && for_stack[i].var != v)
                --i;
        if (i < base)
            error(E_NEXT_WITHOUT_FOR);
        for_depth = i + 1;
        for_frame_t *f = &for_stack[i];
        f->var->n = typed_value(type_of(f->var->name), f->var->n + f->step);
        if ((f->step >= 0 && f->var->n <= f->limit) || (f->step < 0 && f->var->n >= f->limit)) {
            cur_line = f->body.line;
            p = f->body.p;
            jumped = true;
            return;
        }
        --for_depth;
        if (!accept(","))
            return;
    }
}

static void do_goto_or_gosub(bool gosub) {
    int n = read_line_number();
    if (gosub) {
        push_gosub();
    }
    jump_to_line(n);
}

static void push_gosub(void) {
    if (gosub_depth == STACK_MAX)
        error(E_OUT_OF_MEMORY);
    skip_statement();
    gosub_stack[gosub_depth++] = (gosub_frame_t){{cur_line, p}, for_depth, while_depth, -1};
}

// ---- Event trapping -------------------------------------------------------------
//
// ON KEY(n), ON TIMER(s), ON PLAY(n) and ON COM(1) GOSUB, switched with
// KEY(n)/TIMER/PLAY/COM(1) ON, OFF or STOP. Events are checked between
// statements; one happening while STOPped (or while its handler runs) is
// handled once it's back ON.

enum { EV_KEY = 0, EV_TIMER = 21, EV_PLAY, EV_COM, EV_COUNT };  ///< KEY(1)-KEY(20) are 1-20
enum { EV_OFF, EV_ON, EV_STOP };

typedef struct {
    int line;     ///< the handler, 0 none
    int state;
    bool pending, in_handler;
} event_t;
static event_t (*events_p)[EV_COUNT];  ///< (basic_ram, while BASIC runs)
#define events (*events_p)
static double timer_interval, timer_next;
static int play_threshold, play_last;

static bool events_enabled;  ///< some event isn't OFF: check_events has work to do

static void reset_events(void) {
    memset(events, 0, sizeof events);
    events_enabled = false;
}

/// KEY(n) / TIMER / PLAY / COM(n) ON|OFF|STOP
static void set_event_state(int ev) {
    if (accept_word("ON"))
        events[ev].state = EV_ON;
    else if (accept_word("OFF"))
        events[ev].state = EV_OFF, events[ev].pending = false;
    else if (accept_word("STOP"))
        events[ev].state = EV_STOP;
    else
        error(E_SYNTAX);
    if (ev == EV_TIMER && events[ev].state == EV_ON)
        timer_next = port_seconds() + timer_interval;
    events_enabled = false;
    for (int i = 1; i < EV_COUNT; ++i)
        events_enabled |= events[i].state != EV_OFF;
}

/// The event number of KEY(n) etc. after ON or before ON/OFF/STOP.
static int event_number(const char *what) {
    if (!strcmp(what, "TIMER") || !strcmp(what, "PLAY"))
        return what[0] == 'T' ? EV_TIMER : EV_PLAY;
    int n = arg_int();
    expect(")");
    if (what[0] == 'K') {
        if (n < 1 || n > 20)
            error(E_ILLEGAL_CALL);
        return EV_KEY + n;
    }
    if (n != 1)
        error(E_ILLEGAL_CALL);  // COM1 is the only port
    return EV_COM;
}

/// Function keys and arrows waiting in the key queue, for ON KEY: takes the
/// first trapped one out and returns its KEY number (1-10 F1-F10, 11 up,
/// 12 left, 13 right, 14 down), or 0.
static int take_trapped_key(void) {
    char q[sizeof key_queue];
    int n = 0;
    while (key_tail != key_head) {
        q[n++] = key_queue[key_tail];
        key_tail = (key_tail + 1) % (int)sizeof key_queue;
    }
    int found = 0, cut_from = 0, cut_len = 0;
    for (int i = 0; i < n && !found; ++i) {
        if (q[i] != 0x1b || i + 2 >= n)
            continue;
        int k = 0, len = 0;
        if (q[i + 1] == 'O' && q[i + 2] >= 'P' && q[i + 2] <= 'S') {
            k = q[i + 2] - 'P' + 1;
            len = 3;
        } else if (q[i + 1] == '[' && q[i + 2] >= 'A' && q[i + 2] <= 'D') {
            static const int arrow[4] = {11, 14, 13, 12};  // A up, B down, C right, D left
            k = arrow[q[i + 2] - 'A'];
            len = 3;
        } else if (q[i + 1] == '[') {
            int param = 0, j = i + 2;
            while (j < n && isdigit((unsigned char)q[j]))
                param = param * 10 + q[j++] - '0';
            if (j < n && q[j] == '~') {
                static const int fk[] = {[15] = 5, [17] = 6, [18] = 7, [19] = 8, [20] = 9, [21] = 10};
                k = param < 22 ? fk[param] : 0;
                len = j - i + 1;
            }
        }
        if (k && events[EV_KEY + k].state != EV_OFF) {
            found = k;
            cut_from = i;
            cut_len = len;
        }
    }
    for (int i = 0; i < n; ++i)  // put back the rest
        if (i < cut_from || i >= cut_from + cut_len) {
            key_queue[key_head] = q[i];
            key_head = (key_head + 1) % (int)sizeof key_queue;
        }
    return found;
}

/// True if COM1 is open and something has arrived on it.
static bool files_com_waiting(void) {
    for (int i = 1; i <= MAX_FILES; ++i)
        if (files[i] && files[i]->mode == FM_COM)
            return port_serial_waiting() > 0 || files[i]->peek >= 0;
    return false;
}

/// Between statements of a running program: notes events and runs the
/// handler of one that is ON.
static void check_events(void) {
    if (!running || cur_line < 0 || !events_enabled)
        return;
    if (events[EV_TIMER].state != EV_OFF && timer_interval > 0 && port_seconds() >= timer_next) {
        events[EV_TIMER].pending = true;
        timer_next = port_seconds() + timer_interval;
    }
    if (events[EV_PLAY].state != EV_OFF) {
        int left = port_play_pending();
        if (left < play_threshold && play_last >= play_threshold)
            events[EV_PLAY].pending = true;
        play_last = left;
    }
    if (events[EV_COM].state != EV_OFF && files_com_waiting())
        events[EV_COM].pending = true;
    read_keys(true);
    int k = take_trapped_key();
    if (k)
        events[EV_KEY + k].pending = true;
    for (int i = 1; i < EV_COUNT; ++i) {
        if (!events[i].pending || events[i].state != EV_ON || events[i].in_handler || !events[i].line)
            continue;
        events[i].pending = false;
        events[i].in_handler = true;
        if (gosub_depth == STACK_MAX)
            error(E_OUT_OF_MEMORY);
        gosub_stack[gosub_depth++] = (gosub_frame_t){{cur_line, p}, for_depth, while_depth, i};
        jump_to_line(events[i].line);
        return;
    }
}

/// ON KEY(n) / TIMER(s) / PLAY(n) / COM(n) GOSUB line (0: no handler).
static void do_on_event(const char *what) {
    expect("(");
    int ev;
    if (!strcmp(what, "TIMER")) {
        double s = num_expr();
        expect(")");
        if (s < 1 || s > 86400)
            error(E_ILLEGAL_CALL);
        timer_interval = s;
        timer_next = port_seconds() + s;
        ev = EV_TIMER;
    } else if (!strcmp(what, "PLAY")) {
        int n = arg_int();
        expect(")");
        if (n < 1 || n > 32)
            error(E_ILLEGAL_CALL);
        play_threshold = n;
        play_last = port_play_pending();
        ev = EV_PLAY;
    } else {
        ev = event_number(what);
    }
    if (!accept("GOSUB"))
        error(E_SYNTAX);
    int line = read_line_number();
    if (line) {
        bool exact;
        find_line(line, &exact);
        if (!exact)
            error(E_UNDEFINED_LINE);
    }
    events[ev].line = line;
}


static void do_if(void) {
    jumped = true;  // execution carries on from wherever p ends up
    value_t c = expr();
    bool truth = c.is_str ? c.s[0] != 0 : c.n != 0;
    bool then = accept("THEN");
    if (!then) {
        skip_spaces();
        if (strncmp(p, "GOTO", 4))
            error(E_SYNTAX);
    }
    if (truth) {
        skip_spaces();
        if (isdigit((unsigned char)*p)) {
            jump_to_line(read_line_number());
            return;
        }
        return;  // the statements after THEN run next; ELSE then skips the rest
    }
    // Find this IF's ELSE (nested IFs on the line take their own ELSEs).
    int depth = 0;
    bool in_str = false;
    for (; *p; ++p) {
        if (*p == '"') {
            in_str = !in_str;
            continue;
        }
        if (in_str)
            continue;
        if (!strncmp(p, "IF", 2) && (p[-1] == ' ' || p[-1] == ':' || p[-1] == 'N')) {
            ++depth;
        } else if (!strncmp(p, "ELSE", 4)) {
            if (depth == 0) {
                p += 4;
                skip_spaces();
                if (isdigit((unsigned char)*p))
                    jump_to_line(read_line_number());
                return;
            }
            --depth;
        }
    }
}

/// ON ERROR GOTO line (0 turns it off; inside a handler that reports the
/// error as if there were no handler, as GW-BASIC does).
static void do_on_error(void) {
    if (!accept("GOTO"))
        error(E_SYNTAX);
    int n = read_line_number();
    if (n == 0) {
        on_error_line = -1;
        if (in_error_handler) {
            in_error_handler = false;
            error(err_code);
        }
        return;
    }
    bool exact;
    find_line(n, &exact);
    if (!exact)
        error(E_UNDEFINED_LINE);
    on_error_line = n;
}

/// RESUME [0 | NEXT | line]
static void do_resume(void) {
    if (!in_error_handler)
        error(E_RESUME_WITHOUT_ERROR);
    skip_spaces();
    int n = 0;
    bool next = accept_word("NEXT");
    if (!next && isdigit((unsigned char)*p))
        n = read_line_number();
    in_error_handler = false;
    if (n) {
        jump_to_line(n);
        return;
    }
    cur_line = err_pos.line;
    p = err_pos.p;
    if (next)
        skip_statement();
    jumped = true;
}

/// Sends an error to the program's ON ERROR handler, if it has one and isn't
/// in it already. False if the error should stop the program instead.
static bool trap_error(int code) {
    if (!running || cur_line < 0 || on_error_line < 0 || in_error_handler || code > 255)
        return false;
    err_code = code;
    err_pos = stmt_pos;
    err_line = err_pos.line >= 0 ? lines[err_pos.line].num : 65535;
    in_error_handler = true;
    jump_to_line(on_error_line);
    return true;
}

static void do_on(void) {
    if (accept_word("ERROR")) {
        do_on_error();
        return;
    }
    static const char *const kinds[] = {"KEY", "TIMER", "PLAY", "COM"};
    for (int i = 0; i < 4; ++i) {
        skip_spaces();
        if (!strncmp(p, kinds[i], strlen(kinds[i]))) {
            const char *q = p + strlen(kinds[i]);
            while (*q == ' ')
                ++q;
            if (*q == '(') {
                p = q;
                do_on_event(kinds[i]);
                return;
            }
        }
    }
    if (accept_word("STRIG") || accept_word("PEN")) {
        skip_statement();  // no joystick or light pen: never happens
        return;
    }
    int n = (int)lround(num_expr());
    bool gosub;
    if (accept("GOTO"))
        gosub = false;
    else if (accept("GOSUB"))
        gosub = true;
    else
        error(E_SYNTAX);
    if (n < 0 || n > 255)
        error(E_ILLEGAL_CALL);
    for (int i = 1;; ++i) {
        int target = read_line_number();
        if (i == n) {
            if (gosub)
                push_gosub();
            jump_to_line(target);
            return;
        }
        if (!accept(","))
            return;  // out of range: carry on with the next statement
    }
}

/// MID$(v$, start[, n]) = x$: overwrites part of v$ (never changing its
/// length).
static void do_mid_statement(void) {
    lvalue_t lv = lvalue();
    if (!lv.is_str)
        error(E_TYPE_MISMATCH);
    expect(",");
    int start = arg_int(), n = 255;
    if (accept(","))
        n = arg_int();
    expect(")");
    expect("=");
    const char *src = str_expr();
    const char *dst = *lv.s ? *lv.s : EMPTY;
    int dlen = slen(dst), slen_ = slen(src);
    if (start < 1 || start > 255 || n < 0 || n > 255 || start > dlen)
        error(E_ILLEGAL_CALL);
    int count = n;
    if (count > slen_)
        count = slen_;
    if (count > dlen - start + 1)
        count = dlen - start + 1;
    char buf[STR_MAX + 1];
    memcpy(buf, dst, dlen);
    memcpy(buf + start - 1, src, count);
    set_strn(lv.s, buf, dlen);
}

/// ERASE a, b$: frees whole arrays, so they can be dimensioned again.
static void do_erase(void) {
    do {
        char name[NAME_MAX + 2];
        if (!read_name(name))
            error(E_SYNTAX);
        unsigned h = hash(name);
        array_t **ap = &arrays[h];
        while (*ap && strcmp((*ap)->name, name))
            ap = &(*ap)->next;
        if (!*ap)
            error(E_ILLEGAL_CALL);
        array_t *a = *ap;
        *ap = a->next;
        if (a->s)
            for (int i = 0; i < a->count; ++i)
                sfree(a->s[i]);
        free(a->s);
        free(a->n);
        free(a);
    } while (accept(","));
}

/// DATE$ = "mm-dd-yy[yy]" and TIME$ = "hh[:mm[:ss]]" set the clock (UTC).
static void set_date_or_time(const char *v, bool date) {
    char text[32];
    snprintf(text, sizeof text, "%.*s", (int)slen(v), v);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int a = 0, b = 0, c = 0;
    int got = sscanf(text, "%d%*[-/:]%d%*[-/:]%d", &a, &b, &c);
    if (date) {
        if (got != 3 || a < 1 || a > 12 || b < 1 || b > 31 || c < 0 || (c > 99 && (c < 1980 || c > 2099)))
            error(E_ILLEGAL_CALL);
        tm.tm_mon = a - 1;
        tm.tm_mday = b;
        tm.tm_year = (c < 80 ? c + 2000 : c < 100 ? c + 1900 : c) - 1900;
    } else {
        if (got < 1 || a < 0 || a > 23 || b < 0 || b > 59 || c < 0 || c > 59)
            error(E_ILLEGAL_CALL);
        tm.tm_hour = a;
        tm.tm_min = got > 1 ? b : 0;
        tm.tm_sec = got > 2 ? c : 0;
    }
    // timegm, done by hand (newlib may not have it).
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    long days = 365L * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + tm.tm_mday - 719469;
    struct timeval tv = {(time_t)days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec, 0};
    settimeofday(&tv, NULL);
}

/// ENVIRON "NAME=value" and ENVIRON$: a small table of strings (no DOS).
#define MAX_ENVIRON 16
static char *environ_table[MAX_ENVIRON];

static void set_environ(const char *v) {
    char text[256];
    snprintf(text, sizeof text, "%.*s", (int)slen(v), v);
    char *eq = strchr(text, '=');
    if (!eq || eq == text)
        error(E_ILLEGAL_CALL);
    for (char *c = text; c < eq; ++c)
        *c = toupper((unsigned char)*c);
    size_t nlen = eq - text + 1;
    int slot = -1;
    for (int i = 0; i < MAX_ENVIRON; ++i) {
        if (environ_table[i] && !strncmp(environ_table[i], text, nlen)) {
            free(environ_table[i]);
            environ_table[i] = NULL;
            slot = i;
        } else if (!environ_table[i] && slot < 0) {
            slot = i;
        }
    }
    if (!eq[1])
        return;  // "NAME=" removes it
    if (slot < 0)
        error(E_OUT_OF_MEMORY);
    environ_table[slot] = checked_malloc(strlen(text) + 1);
    strcpy(environ_table[slot], text);
}

static value_t environ_value(value_t arg) {
    if (!arg.is_str) {
        int n = (int)lround(arg.n), k = 0;
        if (n < 1 || n > 255)
            error(E_ILLEGAL_CALL);
        for (int i = 0; i < MAX_ENVIRON; ++i)
            if (environ_table[i] && ++k == n)
                return str(environ_table[i], strlen(environ_table[i]));
        return str("", 0);
    }
    char name[64];
    snprintf(name, sizeof name, "%.*s", (int)slen(arg.s), arg.s);
    for (char *c = name; *c; ++c)
        *c = toupper((unsigned char)*c);
    size_t n = strlen(name);
    for (int i = 0; i < MAX_ENVIRON; ++i)
        if (environ_table[i] && !strncmp(environ_table[i], name, n) && environ_table[i][n] == '=')
            return str(environ_table[i] + n + 1, strlen(environ_table[i] + n + 1));
    return str("", 0);
}

/// INKEY$: one key, or as on a PC CHR$(0) and the scan code for function
/// and cursor keys. A key whose ON KEY trap is set goes to the trap instead.
static value_t inkey(void) {
    int c = key_now();
    if (c < 0)
        return str("", 0);
    if (c != 0x1b) {
        char ch = c;
        return str(&ch, 1);
    }
    int k = read_escape();
    if (k == -1)
        return str("\x1b", 1);  // the Esc key itself
    int code = 0, trap = 0;
    if (k >= KEY_F(1) && k <= KEY_F(10)) {
        code = 59 + k - KEY_F(1);
        trap = k - KEY_F(0);
    } else {
        switch (k) {
        case 'A': code = 72; trap = 11; break;
        case 'D': code = 75; trap = 12; break;
        case 'C': code = 77; trap = 13; break;
        case 'B': code = 80; trap = 14; break;
        case 'H': code = 71; break;
        case 'F': code = 79; break;
        case 'U': code = 73; break;
        case 'V': code = 81; break;
        case 'I': code = 82; break;
        case 'X': code = 83; break;
        default: return str("", 0);
        }
    }
    if (trap && running && events[EV_KEY + trap].state != EV_OFF) {
        events[EV_KEY + trap].pending = true;
        return str("", 0);
    }
    char two[2] = {0, (char)code};
    return str(two, 2);
}

// KEY ON: the soft keys' labels on text row 25, as on a PC (the rows above
// then scroll on their own).

static void draw_key_line(void) {
    out_flush();
    printf("\x1b" "7\x1b[25;1H");
    if (key_line_on) {
        for (int i = 0; i < 10; ++i) {
            char label[7];
            int n = 0;
            for (const char *t = soft_keys[i]; *t && n < 6; ++t)
                label[n++] = *t == '\r' ? 27 : (unsigned char)*t < 0x20 ? ' ' : *t;
            while (n < 6)
                label[n++] = ' ';
            label[6] = 0;
            printf("%d\x1b[7m%s\x1b[27m%s", (i + 1) % 10, label, i < 9 ? " " : "");
        }
        printf("\x1b[1;24r");
    } else {
        printf("\x1b[2K\x1b[r");
    }
    printf("\x1b" "8");
    fflush(stdout);
}

static void set_key_line(bool on) {
    if (on == key_line_on)
        return;
    key_line_on = on;
    draw_key_line();
}

/// KEY ON | OFF | LIST, KEY n, x$ (a soft key), KEY(n) ON | OFF | STOP.
static void do_key(void) {
    if (accept("(")) {
        set_event_state(event_number("KEY"));
        return;
    }
    if (accept_word("ON")) {
        set_key_line(true);
        return;
    }
    if (accept_word("OFF")) {
        set_key_line(false);
        return;
    }
    if (accept_word("LIST")) {
        for (int i = 0; i < 10; ++i) {
            char line[40];
            int n = snprintf(line, sizeof line, "F%d ", i + 1);
            for (const char *t = soft_keys[i]; *t && n < (int)sizeof line - 1; ++t)
                line[n++] = *t == '\r' ? ' ' : *t;
            line[n] = 0;
            out_str(line);
            out_newline();
        }
        return;
    }
    int n = arg_int();
    expect(",");
    const char *t = str_expr();
    if (n >= 15 && n <= 20)
        return;  // a user trap key (shift flags + scan code): not detected here
    if (n < 1 || n > 10)
        error(E_ILLEGAL_CALL);
    int len = slen(t);
    if (len > 15)
        len = 15;
    memcpy(soft_keys[n - 1], t, len);
    soft_keys[n - 1][len] = 0;
    if (key_line_on)
        draw_key_line();
}

static void do_dim(void) {
    do {
        char name[NAME_MAX + 2];
        if (!read_name(name))
            error(E_SYNTAX);
        expect("(");
        int bounds[8], n = 0;
        do {
            if (n == 8)
                error(E_SUBSCRIPT);
            bounds[n++] = arg_int();
        } while (accept(","));
        expect(")");
        make_array(name, n, bounds);
    } while (accept(","));
}

/// Finds the next DATA item. data_pos.p is NULL to scan from the start of
/// data_pos.line, or points just after the last item read.
static bool find_data(void) {
    if (data_pos.p) {
        const char *q = data_pos.p;
        while (*q == ' ')
            ++q;
        if (*q == ',') {
            data_pos.p = q + 1;
            return true;
        }
        data_pos.p = q;
    } else {
        if (data_pos.line >= line_count)
            return false;
        data_pos.p = lines[data_pos.line].text;
    }
    for (;;) {
        bool in_str = false;
        for (const char *q = data_pos.p; *q; ++q) {
            if (*q == '"') {
                in_str = !in_str;
            } else if (!in_str && !strncmp(q, "DATA", 4)) {
                data_pos.p = q + 4;
                return true;
            } else if (!in_str && (!strncmp(q, "REM", 3) || *q == '\'')) {
                break;
            }
        }
        if (++data_pos.line >= line_count)
            return false;
        data_pos.p = lines[data_pos.line].text;
    }
}

static void do_read(void) {
    do {
        lvalue_t lv = lvalue();
        if (!find_data())
            error(E_OUT_OF_DATA);
        const char *q = data_pos.p;
        while (*q == ' ')
            ++q;
        char item[LINE_MAX_LEN + 1];
        int n = 0;
        if (*q == '"') {
            ++q;
            while (*q && *q != '"')
                item[n++] = *q++;
            if (*q == '"')
                ++q;
            while (*q == ' ')
                ++q;
        } else {
            while (*q && *q != ',' && *q != ':')
                item[n++] = *q++;
            while (n && item[n - 1] == ' ')
                --n;
        }
        item[n] = 0;
        data_pos.p = q;
        if (lv.is_str) {
            set_strn(lv.s, item, strlen(item));
        } else {
            char *end;
            double d = strtod(item, &end);
            while (*end == ' ')
                ++end;
            if (*end) {
                cur_line = data_pos.line;  // GW-BASIC reports the DATA line
                error(E_SYNTAX);
            }
            set_num(lv, d);
        }
    } while (accept(","));
}

static void do_restore(void) {
    data_pos.line = 0;
    data_pos.p = NULL;
    skip_spaces();
    if (isdigit((unsigned char)*p)) {
        bool exact;
        int i = find_line(read_line_number(), &exact);
        if (!exact)
            error(E_UNDEFINED_LINE);
        data_pos.line = i;
    }
}

/// DEFINT A-Z, X (and DEFSNG, DEFDBL, DEFSTR): the type of names without a
/// suffix, by first letter.
static void do_deftype(char type) {
    do {
        skip_spaces();
        int a = toupper((unsigned char)*p), b = a;
        if (a < 'A' || a > 'Z')
            error(E_SYNTAX);
        ++p;
        if (accept("-")) {
            skip_spaces();
            b = toupper((unsigned char)*p);
            if (b < a || b > 'Z')
                error(E_SYNTAX);
            ++p;
        }
        for (int c = a; c <= b; ++c)
            def_type[c - 'A'] = type;
    } while (accept(","));
}

static void do_def_seg(void);

static void do_def(void) {
    if (accept_word("SEG")) {
        do_def_seg();
        return;
    }
    if (accept("USR"))
        error(E_ILLEGAL_CALL);  // no machine code
    if (!accept("FN"))
        error(E_SYNTAX);
    if (cur_line < 0)
        error(E_ILLEGAL_DIRECT);
    fn_t f = {0};
    if (!read_name(f.name))
        error(E_SYNTAX);
    if (accept("(")) {
        do {
            if (f.param_count == 4 || !read_name(f.params[f.param_count++]))
                error(E_SYNTAX);
        } while (accept(","));
        expect(")");
    }
    expect("=");
    f.line = cur_line;
    f.body = p;
    skip_statement();
    for (fn_t *g = fns; g; g = g->next) {
        if (!strcmp(g->name, f.name)) {
            fn_t *next = g->next;
            *g = f;
            g->next = next;
            return;
        }
    }
    fn_t *g = checked_malloc(sizeof *g);
    *g = f;
    g->next = fns;
    fns = g;
}

static void do_swap(void) {
    lvalue_t a = lvalue();
    expect(",");
    lvalue_t b = lvalue();
    if (a.is_str != b.is_str)
        error(E_TYPE_MISMATCH);
    if (a.is_str) {
        char *t = *a.s;
        *a.s = *b.s;
        *b.s = t;
    } else {
        double t = *a.n;
        *a.n = *b.n;
        *b.n = t;
    }
}

// ---- Graphics -------------------------------------------------------------
//
// SCREEN 1 (320x200, 4 colours), 2 (640x200, 2 colours), 12 (640x480, 16
// colours) and 13 (320x200, 256 colours) draw straight into the 640x480
// screen; the low-resolution modes are scaled up (2x2, or 1x2 for SCREEN 2)
// and centred vertically. Text from PRINT and LOCATE still uses the 80x30
// console on top. Colours are attributes looked up in attr_map (PALETTE
// changes it); a colour of 256 or more is a direct RGB332 value, which is
// what RGB(r,g,b) returns.

#define FB_W 640
#define FB_H 480

static int screen_mode;            ///< 0 = text
static int page_count = 1;          ///< screen pages the mode has
static int active_page, visual_page; ///< the one drawn on, the one shown
static int gw, gh, gsx, gsy, gyoff;  ///< logical size, scale, vertical offset
static int gcolours;               ///< attributes in this mode
static int gfg, gbg;               ///< drawing and background attributes
static double last_x, last_y;      ///< the "last point" for STEP and DRAW
static uint8_t (*attr_map_p)[256];  ///< (basic_ram, while BASIC runs)
#define attr_map (*attr_map_p)

/// The PC's 16 colours in RGB332.
static const uint8_t pc_rgb[16] = {
    0x00, 0x02, 0x14, 0x16, 0xa0, 0xa2, 0xa8, 0xb6,
    0x49, 0x4b, 0x5d, 0x5f, 0xe9, 0xeb, 0xfd, 0xff,
};

static uint8_t to_rgb332(double r, double g, double b) {
    int ri = (int)(r * 7 + 0.5), gi = (int)(g * 7 + 0.5), bi = (int)(b * 3 + 0.5);
    return (uint8_t)(ri << 5 | gi << 2 | bi);
}

/// The VGA mode 13 default palette, approximated: the 16 PC colours, a grey
/// ramp, then 9 blocks of 24 hues (3 brightnesses x 3 saturations).
static void default_palette(void) {
    memset(attr_map, 0, sizeof attr_map);
    if (screen_mode == 1) {
        static const uint8_t cga[4] = {0, 11, 13, 15};
        for (int i = 0; i < 4; ++i)
            attr_map[i] = pc_rgb[cga[i]];
        return;
    }
    if (screen_mode == 2 || screen_mode == 11) {
        attr_map[1] = pc_rgb[15];
        return;
    }
    if (screen_mode == 10) {  // monochrome: black, grey, white, bright white
        static const uint8_t mono[4] = {0, 8, 7, 15};
        for (int i = 0; i < 4; ++i)
            attr_map[i] = pc_rgb[mono[i]];
        return;
    }
    for (int i = 0; i < 16; ++i)
        attr_map[i] = pc_rgb[i];
    if (screen_mode != 13)
        return;
    for (int i = 0; i < 16; ++i) {
        double v = i / 15.0;
        attr_map[16 + i] = to_rgb332(v, v, v);
    }
    static const double value[3] = {1.0, 0.45, 0.25}, sat[3] = {1.0, 0.55, 0.3};
    for (int i = 0; i < 216; ++i) {
        int block = i / 24, hue = i % 24;
        double v = value[block / 3], s = sat[block % 3];
        double h = hue / 4.0;  // 0..6
        double f = h - floor(h), pp = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        double r, g, b;
        switch ((int)h % 6) {
        case 0: r = v; g = t; b = pp; break;
        case 1: r = q; g = v; b = pp; break;
        case 2: r = pp; g = v; b = t; break;
        case 3: r = pp; g = q; b = v; break;
        case 4: r = t; g = pp; b = v; break;
        default: r = v; g = pp; b = q; break;
        }
        attr_map[32 + i] = to_rgb332(r, g, b);
    }
}

static void need_graphics(void) {
    if (!screen_mode)
        error(E_ILLEGAL_CALL);
}

static uint8_t colour_rgb(int c) {
    if (c >= 256 && c < 512)
        return c - 256;
    if (c < 0 || c >= gcolours)
        error(E_ILLEGAL_CALL);
    return attr_map[c];
}

/// VIEW (the viewport, in screen pixels, inclusive) and WINDOW (the world
/// coordinates mapped onto it). Drawing is clipped to the viewport.
static bool view_set, view_screen, window_set, window_screen;
static int view_x1, view_y1, view_x2, view_y2;
static double win_x1, win_y1, win_x2, win_y2;
static int clip_x1, clip_y1, clip_x2, clip_y2;

static void reset_view(void) {
    view_set = window_set = false;
    clip_x1 = clip_y1 = 0;
    clip_x2 = gw - 1;
    clip_y2 = gh - 1;
}

/// World (program) coordinates to screen pixels and back.
static double phys_x(double x) {
    int ox = view_set ? view_x1 : 0, w = view_set ? view_x2 - view_x1 : gw - 1;
    if (window_set)
        return ox + (x - win_x1) * w / (win_x2 - win_x1);
    return x + (view_set && !view_screen ? ox : 0);
}

static double phys_y(double y) {
    int oy = view_set ? view_y1 : 0, h = view_set ? view_y2 - view_y1 : gh - 1;
    if (window_set)
        return oy + (window_screen ? y - win_y1 : win_y2 - y) * h / (win_y2 - win_y1);
    return y + (view_set && !view_screen ? oy : 0);
}

static double world_x(double x) {
    int ox = view_set ? view_x1 : 0, w = view_set ? view_x2 - view_x1 : gw - 1;
    if (window_set)
        return win_x1 + (x - ox) * (win_x2 - win_x1) / (w ? w : 1);
    return x - (view_set && !view_screen ? ox : 0);
}

static double world_y(double y) {
    int oy = view_set ? view_y1 : 0, h = view_set ? view_y2 - view_y1 : gh - 1;
    if (window_set) {
        double d = (y - oy) * (win_y2 - win_y1) / (h ? h : 1);
        return window_screen ? win_y1 + d : win_y2 - d;
    }
    return y - (view_set && !view_screen ? oy : 0);
}

static int round_coord(double d);

static int px_of(double x) {
    return round_coord(phys_x(x));
}

static int py_of(double y) {
    return round_coord(phys_y(y));
}

static void plot(int x, int y, uint8_t rgb) {
    if (x < clip_x1 || y < clip_y1 || x > clip_x2 || y > clip_y2)
        return;
    port_fill_rect(x * gsx, y * gsy + gyoff, gsx, gsy, rgb);
}

static void hspan(int x1, int x2, int y, uint8_t rgb) {
    if (y < clip_y1 || y > clip_y2)
        return;
    if (x1 > x2) {
        int t = x1;
        x1 = x2;
        x2 = t;
    }
    if (x1 < clip_x1)
        x1 = clip_x1;
    if (x2 > clip_x2)
        x2 = clip_x2;
    if (x1 <= x2)
        port_fill_rect(x1 * gsx, y * gsy + gyoff, (x2 - x1 + 1) * gsx, gsy, rgb);
}

static uint8_t read_rgb(int x, int y) {
    return port_get_pixel(x * gsx, y * gsy + gyoff);
}

static int gfx_point(int x, int y) {
    need_graphics();
    if (x < 0 || y < 0 || x >= gw || y >= gh)
        return -1;
    uint8_t rgb = read_rgb(x, y);
    for (int a = 0; a < gcolours; ++a)
        if (attr_map[a] == rgb)
            return a;
    return 256 + rgb;
}

/// Bresenham, with GW-BASIC's 16-bit line style (bit 15 first).
static void draw_line(int x1, int y1, int x2, int y2, uint8_t rgb, unsigned style) {
    int dx = abs(x2 - x1), dy = -abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    unsigned bit = 0x8000;
    for (;;) {
        if (style & bit)
            plot(x1, y1, rgb);
        bit = bit >> 1 ? bit >> 1 : 0x8000;
        if (x1 == x2 && y1 == y2)
            return;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/// "(x,y)" or "STEP(x,y)" (relative to the last point).
static void read_point(double *x, double *y) {
    bool step = accept("STEP");
    expect("(");
    *x = num_expr();
    expect(",");
    *y = num_expr();
    expect(")");
    if (step) {
        *x += last_x;
        *y += last_y;
    }
}

static int round_coord(double d) {
    if (d < -1e6 || d > 1e6)
        error(E_OVERFLOW);
    return (int)floor(d + 0.5);
}

/// An optional ",value" argument; true if present (empty ",," counts as absent).
static bool optional_arg(double *v) {
    if (!accept(","))
        return false;
    skip_spaces();
    if (*p == ',' || at_end_of_statement())
        return false;
    *v = num_expr();
    return true;
}

static void set_screen_mode(int mode) {
    int pages = 1;
    switch (mode) {
    case 0: break;
    case 1: gw = 320; gh = 200; gsx = 2; gsy = 2; gcolours = 4; break;
    case 2: gw = 640; gh = 200; gsx = 1; gsy = 2; gcolours = 2; break;
    case 7: gw = 320; gh = 200; gsx = 2; gsy = 2; gcolours = 16; pages = 8; break;
    case 8: gw = 640; gh = 200; gsx = 1; gsy = 2; gcolours = 16; pages = 4; break;
    case 9: gw = 640; gh = 350; gsx = 1; gsy = 1; gcolours = 16; pages = 2; break;
    case 10: gw = 640; gh = 350; gsx = 1; gsy = 1; gcolours = 4; pages = 2; break;
    case 11: gw = 640; gh = 480; gsx = 1; gsy = 1; gcolours = 2; break;
    case 12: gw = 640; gh = 480; gsx = 1; gsy = 1; gcolours = 16; break;
    case 13: gw = 320; gh = 200; gsx = 2; gsy = 2; gcolours = 256; break;
    default: error(E_ILLEGAL_CALL);
    }
    port_reset_pages();
    sprites_reset();
    if (mode)
        port_mouse_pointer(false);
    page_count = pages;
    reset_view();
    active_page = visual_page = 0;
    screen_mode = mode;
    // 200-line modes sit at the top, so their 25 text rows (8 lines each,
    // 16 on screen) line up with the graphics as on a PC; others centre.
    gyoff = gh * gsy == 400 ? 0 : (FB_H - gh * gsy) / 2;
    default_palette();
    gfg = mode == 0 ? 7 : mode == 13 ? 15 : gcolours - 1;
    gbg = 0;
    last_x = gw / 2;
    last_y = gh / 2;
    // Clear, and hide the text cursor in graphics modes (its blink would
    // XOR the pixels under it).
    printf("\x1b[0m\x1b[2J\x1b[H%s", mode ? "\x1b[?25l" : "\x1b[?25h");
    out_row = out_col = 0;
    out_flush();
    if (key_line_on)
        draw_key_line();
}

static int mouse_coord(bool want_x) {
    int x, y, b;
    if (!port_mouse(&x, &y, &b))
        return 0;
    if (!screen_mode)
        return want_x ? x / 8 + 1 : y / 16 + 1;
    int v = want_x ? x / gsx : (y - gyoff) / gsy, most = want_x ? gw - 1 : gh - 1;
    return v < 0 ? 0 : v > most ? most : v;
}

/// MUSIC "file"[, loop] plays a MOD song in the background (loop 0: once);
/// MUSIC STOP, PAUSE, RESUME (or ON), VOL n (0-100), POS order[, row].
/// Music a program starts stops when it ends; music started from the
/// prompt plays on until MUSIC STOP, NEW, RUN or SYSTEM.
static bool music_from_program;

static void do_music(void) {
    if (accept_word("STOP")) {
        port_music_stop();
    } else if (accept_word("PAUSE")) {
        port_music_pause(true);
    } else if (accept_word("RESUME") || accept_word("ON")) {
        port_music_pause(false);
    } else if (accept_word("VOL")) {
        port_music_volume(arg_int());
    } else if (accept_word("POS")) {
        int order = arg_int(), row = 0;
        if (accept(","))
            row = arg_int();
        port_music_pos(order, row);
    } else {
        char name[96];
        file_name_arg(name, sizeof name, false);
        bool loop = true;
        if (accept(","))
            loop = num_expr() != 0;
        const char *err = port_music_play(name, loop);
        if (err) {
            if (!strcmp(err, "file not found"))
                error(E_FILE_NOT_FOUND);
            out_str(name);
            out_str(": ");
            out_str(err);
            out_newline();
            error(E_ILLEGAL_CALL);
        }
        music_from_program = running;
    }
}

/// MOUSE ON / OFF: the mouse pointer over the text screen.
static void do_mouse(void) {
    if (accept_word("ON"))
        port_mouse_pointer(screen_mode == 0);  // over graphics it would draw text cells
    else if (accept_word("OFF"))
        port_mouse_pointer(false);
    else
        error(E_SYNTAX);
}

/// SCREEN [mode] [, [colour] [, [apage] [, vpage]]]
static void do_screen(void) {
    int mode = screen_mode, apage = -1, vpage = -1;
    bool have_mode = false;
    skip_spaces();
    if (*p != ',') {
        mode = arg_int();
        have_mode = true;
    }
    if (accept(",")) {
        skip_spaces();
        if (*p != ',' && !at_end_of_statement())
            arg_int();  // colour switch: nothing to do
        if (accept(",")) {
            skip_spaces();
            if (*p != ',' && !at_end_of_statement())
                apage = arg_int();
            if (accept(","))
                vpage = arg_int();
        }
    }
    // Repeating the mode just to change pages (page flipping) keeps the
    // screen; otherwise SCREEN starts afresh.
    if (have_mode && (mode != screen_mode || (apage < 0 && vpage < 0)))
        set_screen_mode(mode);
    if (apage >= 0 || vpage >= 0) {
        if (apage < 0)
            apage = active_page;
        if (vpage < 0)
            vpage = apage;  // as GW-BASIC: the active page is shown
        if (apage >= page_count || vpage >= page_count)
            error(E_ILLEGAL_CALL);
        if (port_set_pages(apage, vpage))
            error(E_OUT_OF_MEMORY);
        active_page = apage;
        visual_page = vpage;
    }
}

static void do_pset(bool preset) {
    need_graphics();
    double x, y, c;
    read_point(&x, &y);
    int colour = preset ? gbg : gfg;
    if (optional_arg(&c))
        colour = (int)lround(c);
    plot(px_of(x), py_of(y), colour_rgb(colour));
    last_x = x;
    last_y = y;
}

static void do_gline(void) {
    need_graphics();
    double x1 = last_x, y1 = last_y, x2, y2, c;
    skip_spaces();
    if (*p == '(' || !strncmp(p, "STEP", 4))
        read_point(&x1, &y1);
    expect("-");
    last_x = x1;  // as in GW-BASIC, -STEP(dx,dy) counts from the first point
    last_y = y1;
    read_point(&x2, &y2);
    int colour = gfg;
    if (optional_arg(&c))
        colour = (int)lround(c);
    uint8_t rgb = colour_rgb(colour);
    int box = 0;  // 1 = B, 2 = BF
    unsigned style = 0xffff;
    if (accept(",")) {
        if (accept("BF"))
            box = 2;
        else if (accept("B"))
            box = 1;
        double s;
        if (optional_arg(&s))
            style = (unsigned)lround(s) & 0xffff;
    }
    int ix1 = px_of(x1), iy1 = py_of(y1), ix2 = px_of(x2), iy2 = py_of(y2);
    if (box == 2) {
        if (iy1 > iy2) {
            int t = iy1;
            iy1 = iy2;
            iy2 = t;
        }
        for (int y = iy1; y <= iy2; ++y)
            hspan(ix1, ix2, y, rgb);
    } else if (box == 1) {
        draw_line(ix1, iy1, ix2, iy1, rgb, style);
        draw_line(ix2, iy1, ix2, iy2, rgb, style);
        draw_line(ix2, iy2, ix1, iy2, rgb, style);
        draw_line(ix1, iy2, ix1, iy1, rgb, style);
    } else {
        draw_line(ix1, iy1, ix2, iy2, rgb, style);
    }
    last_x = x2;
    last_y = y2;
}

static void do_circle(void) {
    need_graphics();
    double cx, cy, r, c, start = 0, end = 2 * M_PI, aspect = (double)gsx / gsy;
    read_point(&cx, &cy);
    expect(",");
    r = num_expr();
    int colour = gfg;
    if (optional_arg(&c))
        colour = (int)lround(c);
    bool has_start = optional_arg(&start);
    bool has_end = optional_arg(&end);
    optional_arg(&aspect);
    (void)has_start;
    (void)has_end;
    uint8_t rgb = colour_rgb(colour);
    if (r < 0 || aspect <= 0)
        error(E_ILLEGAL_CALL);
    if (window_set)  // the radius is in world x units
        r = r * (view_set ? view_x2 - view_x1 : gw - 1) / fabs(win_x2 - win_x1);
    double rx = r, ry = r * aspect;
    if (aspect > 1) {
        ry = r;
        rx = r / aspect;
    }
    // Negative angles also draw a line from the centre (pie slices).
    bool spoke_start = start < 0, spoke_end = end < 0;
    start = fabs(start);
    end = fabs(end);
    if (start > 2 * M_PI + 1e-9 || end > 2 * M_PI + 1e-9)
        error(E_ILLEGAL_CALL);
    if (end < start)
        end += 2 * M_PI;
    int steps = (int)((rx + ry) * 2 * (end - start) / M_PI) + 8;
    int icx = px_of(cx), icy = py_of(cy);
    int px = icx + (int)lround(rx * cos(start)), py = icy - (int)lround(ry * sin(start));
    if (spoke_start)
        draw_line(icx, icy, px, py, rgb, 0xffff);
    for (int i = 1; i <= steps; ++i) {
        double t = start + (end - start) * i / steps;
        int x = icx + (int)lround(rx * cos(t)), y = icy - (int)lround(ry * sin(t));
        draw_line(px, py, x, y, rgb, 0xffff);
        px = x;
        py = y;
    }
    if (spoke_end)
        draw_line(icx, icy, px, py, rgb, 0xffff);
    last_x = cx;
    last_y = cy;
}

/// Scanline flood fill with an explicit stack of seed points (no recursion:
/// BASIC's stack is only 16 KB). Fills until the border colour, and treats
/// pixels already in the paint colour as filled.
static void flood_fill(int x, int y, uint8_t paint, uint8_t border) {
    if (x < clip_x1 || y < clip_y1 || x > clip_x2 || y > clip_y2)
        return;
    uint8_t here = read_rgb(x, y);
    if (here == border || here == paint)
        return;
    int cap = 256, n = 0;
    need_mem(cap * sizeof(int) * 2, HEAP_MARGIN);
    int *stack = malloc(cap * sizeof(int) * 2);
    if (!stack)
        error(E_OUT_OF_MEMORY);
    stack[n++] = x;
    stack[n++] = y;
    int rows = 0;
    while (n) {
        int sy = stack[--n], sx = stack[--n];
        uint8_t c = read_rgb(sx, sy);
        if (c == border || c == paint)
            continue;
        int l = sx, r = sx;
        while (l > clip_x1 && (c = read_rgb(l - 1, sy)) != border && c != paint)
            --l;
        while (r < clip_x2 && (c = read_rgb(r + 1, sy)) != border && c != paint)
            ++r;
        hspan(l, r, sy, paint);
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = sy + dy;
            if (ny < clip_y1 || ny > clip_y2)
                continue;
            bool in_run = false;
            for (int i = l; i <= r; ++i) {
                c = read_rgb(i, ny);
                bool fillable = c != border && c != paint;
                if (fillable && !in_run) {
                    if (n + 2 > cap) {
                        if (port_free_mem() < cap * sizeof(int) * 4 + HEAP_MARGIN) {
                            free(stack);
                            error(E_OUT_OF_MEMORY);
                        }
                        int *s2 = realloc(stack, cap * 2 * sizeof(int) * 2);
                        if (!s2) {
                            free(stack);
                            error(E_OUT_OF_MEMORY);
                        }
                        stack = s2;
                        cap *= 2;
                    }
                    stack[n++] = i;
                    stack[n++] = ny;
                }
                in_run = fillable;
            }
        }
        if (++rows % 64 == 0) {
            read_keys(running);
            if (break_pending) {
                free(stack);
                break_pending = false;
                error(E_BREAK);
            }
        }
    }
    free(stack);
}

static void do_paint(void) {
    need_graphics();
    double x, y, c;
    read_point(&x, &y);
    int paint = gfg, border;
    if (optional_arg(&c))
        paint = (int)lround(c);
    border = paint;
    if (optional_arg(&c))
        border = (int)lround(c);
    flood_fill(px_of(x), py_of(y), colour_rgb(paint), colour_rgb(border));
    last_x = x;
    last_y = y;
}

static void do_palette(void) {
    need_graphics();
    if (at_end_of_statement()) {
        default_palette();
        return;
    }
    int attr = arg_int();
    expect(",");
    int c = arg_int();
    if (attr < 0 || attr >= gcolours)
        error(E_ILLEGAL_CALL);
    if (c >= 256 && c < 512)
        attr_map[attr] = c - 256;
    else if (c >= 0 && c < 16)
        attr_map[attr] = pc_rgb[c];
    else
        error(E_ILLEGAL_CALL);
}

/// DRAW: U D L R E F G H [n], M [+|-]x,[+|-]y, B (move without drawing),
/// N (return afterwards), C colour, S scale (quarter pixels), A angle (x90),
/// TA degrees, P paint,border. Numbers may be "=variable;".
static double draw_number(const char **s, bool *present) {
    while (**s == ' ')
        ++*s;
    if (**s == '=') {
        ++*s;
        char name[NAME_MAX + 2];
        int n = 0;
        while (**s && **s != ';' && n < NAME_MAX + 1)
            name[n++] = toupper((unsigned char)*(*s)++);
        name[n] = 0;
        if (**s == ';')
            ++*s;
        *present = true;
        return named_number(name);
    }
    char *end;
    double d = strtod(*s, &end);
    *present = end != *s;
    *s = end;
    return d;
}

static void do_draw(void) {
    need_graphics();
    const char *cmd = str_expr();
    static double scale = 4, angle = 0;
    const char *s = cmd;
    while (*s) {
        while (*s == ' ' || *s == ';')
            ++s;
        if (!*s)
            break;
        bool no_draw = false, back = false;
        for (;;) {
            char c = toupper((unsigned char)*s);
            if (c == 'B')
                no_draw = true;
            else if (c == 'N')
                back = true;
            else
                break;
            ++s;
        }
        char c = toupper((unsigned char)*s++);
        bool present;
        double n;
        double dx = 0, dy = 0;
        bool move = true, relative = true;
        switch (c) {
        case 'U': dy = -1; break;
        case 'D': dy = 1; break;
        case 'L': dx = -1; break;
        case 'R': dx = 1; break;
        case 'E': dx = 1; dy = -1; break;
        case 'F': dx = 1; dy = 1; break;
        case 'G': dx = -1; dy = 1; break;
        case 'H': dx = -1; dy = -1; break;
        case 'M': {
            while (*s == ' ')
                ++s;
            relative = *s == '+' || *s == '-';
            dx = draw_number(&s, &present);
            if (!present || *s++ != ',')
                error(E_ILLEGAL_CALL);
            dy = draw_number(&s, &present);
            if (!present)
                error(E_ILLEGAL_CALL);
            break;
        }
        case 'C':
            gfg = (int)lround(draw_number(&s, &present));
            colour_rgb(gfg);
            move = false;
            break;
        case 'S':
            scale = draw_number(&s, &present);
            if (scale <= 0)
                scale = 4;
            move = false;
            break;
        case 'A':
            angle = 90 * lround(draw_number(&s, &present));
            move = false;
            break;
        case 'T':
            if (toupper((unsigned char)*s++) != 'A')
                error(E_ILLEGAL_CALL);
            angle = draw_number(&s, &present);
            move = false;
            break;
        case 'P': {
            int paint = (int)lround(draw_number(&s, &present));
            int border = paint;
            if (*s == ',') {
                ++s;
                border = (int)lround(draw_number(&s, &present));
            }
            flood_fill(round_coord(last_x), round_coord(last_y), colour_rgb(paint),
                       colour_rgb(border));
            move = false;
            break;
        }
        default:
            error(E_ILLEGAL_CALL);
        }
        if (!move)
            continue;
        if (c != 'M') {
            n = draw_number(&s, &present);
            if (!present)
                n = 1;
            dx *= n;
            dy *= n;
        }
        double tx, ty;
        if (relative) {
            dx *= scale / 4;
            dy *= scale / 4;
            double a = angle * M_PI / 180;
            double rx = dx * cos(a) + dy * sin(a), ry = -dx * sin(a) + dy * cos(a);
            tx = last_x + rx;
            ty = last_y + ry;
        } else {
            tx = dx;
            ty = dy;
        }
        if (!no_draw)
            draw_line(round_coord(last_x), round_coord(last_y), round_coord(tx), round_coord(ty),
                      colour_rgb(gfg), 0xffff);
        if (!back) {
            last_x = tx;
            last_y = ty;
        }
    }
}

/// PC colour numbers (0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta,
/// 6 brown, 7 white, +8 bright) to ANSI colour numbers.
static const int pc_to_ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};

static void do_color(void) {
    if (screen_mode) {
        double c;
        skip_spaces();
        if (*p != ',') {
            gfg = arg_int();
            colour_rgb(gfg);
        }
        if (optional_arg(&c)) {
            gbg = (int)lround(c);
            colour_rgb(gbg);
        }
        return;
    }
    skip_spaces();
    if (*p != ',') {
        int fg = arg_int();
        if (fg < 0 || fg > 31)
            error(E_ILLEGAL_CALL);
        fg &= 15;
        if (fg == 7)
            printf("\x1b[39m");  // "normal" text: the console's own colour
        else
            printf("\x1b[%dm", (fg < 8 ? 30 : 90) + pc_to_ansi[fg & 7]);
    }
    if (accept(",")) {
        skip_spaces();
        if (*p != ',' && !at_end_of_statement()) {
            int bg = arg_int();
            if (bg < 0 || bg > 15)
                error(E_ILLEGAL_CALL);
            if (bg == 0)
                printf("\x1b[49m");
            else
                printf("\x1b[%dm", (bg < 8 ? 40 : 100) + pc_to_ansi[bg & 7]);
        }
        if (accept(","))
            arg_int();  // border colour: nothing to do
    }
    out_flush();
}

static void do_locate(void) {
    int row = out_row + 1, col = out_col + 1;
    skip_spaces();
    if (*p != ',')
        row = arg_int();
    if (accept(",")) {
        skip_spaces();
        if (*p != ',' && !at_end_of_statement())
            col = arg_int();
        if (accept(",")) {
            int cursor = arg_int();
            printf(cursor ? "\x1b[?25h" : "\x1b[?25l");
        }
    }
    if (row < 1 || row > SCREEN_ROWS || col < 1 || col > SCREEN_COLS)
        error(E_ILLEGAL_CALL);
    printf("\x1b[%d;%dH", row, col);
    out_row = row - 1;
    out_col = col - 1;
    out_flush();
}

// ---- PEEK, POKE, DEF SEG, BSAVE, BLOAD -------------------------------------
//
// There's no PC memory, so these reach a few emulated places: the text
// screen at &HB800 (and &HB000), two bytes a character (the character, then
// its colours); SCREEN 13's pixels at &HA000 (one byte each, 320 to a row);
// and some BIOS data bytes (video mode, columns, the timer tick count,
// keyboard flags). Anything else is an Illegal function call.

static long def_seg = -1;  ///< -1: BASIC's own data segment (not emulated)

static void do_def_seg(void) {
    def_seg = -1;
    if (accept("=")) {
        long seg = (long)lround(num_expr());
        if (seg < -32768 || seg > 65535)
            error(E_OVERFLOW);
        def_seg = seg & 0xffff;
    }
}

static long seg_address(double offset) {
    if (def_seg < 0 || offset < -32768 || offset > 65535)
        error(E_ILLEGAL_CALL);
    return def_seg * 16 + ((long)lround(offset) & 0xffff);
}

/// SCREEN 1 and 2 keep their pixels at &HB800 as a CGA does: even lines in
/// the first 8 KB, odd ones from &H2000, 80 bytes a line, the leftmost pixel
/// in the top bits (4 two-bit pixels a byte in SCREEN 1, 8 in SCREEN 2).
static bool cga_pixels(long addr, int *x, int *y) {
    long o = addr - 0xB8000;
    if ((screen_mode != 1 && screen_mode != 2) || o < 0 || o >= 0x4000)
        return false;
    long rel = o & 0x1fff;
    if (rel >= 8000)
        return false;
    *y = rel / 80 * 2 + (o >= 0x2000);
    *x = rel % 80 * (screen_mode == 2 ? 8 : 4);
    return true;
}

static void raw_plot(int x, int y, uint8_t rgb) {
    if (x >= 0 && y >= 0 && x < gw && y < gh)
        port_fill_rect(x * gsx, y * gsy + gyoff, gsx, gsy, rgb);
}

static int peek_byte(long addr) {
    int cx, cy;
    if (cga_pixels(addr, &cx, &cy)) {
        int v = 0, bits = screen_mode == 2 ? 1 : 2;
        for (int i = 0; i < 8 / bits; ++i)
            v = v << bits | (gfx_point(cx + i, cy) & ((1 << bits) - 1));
        return v;
    }
    if ((screen_mode == 1 || screen_mode == 2) && addr >= 0xB8000 && addr < 0xBC000)
        return 0;  // the spare bytes at the end of each CGA bank
    if (addr >= 0xB8000 && addr < 0xB8000 + SCREEN_ROWS * SCREEN_COLS * 2) {
        int cell = (addr - 0xB8000) / 2;
        uint8_t ch, at;
        port_get_cell(cell / SCREEN_COLS, cell % SCREEN_COLS, &ch, &at);
        return addr & 1 ? at : ch;
    }
    if (addr >= 0xB0000 && addr < 0xB0000 + SCREEN_ROWS * SCREEN_COLS * 2)
        return peek_byte(addr - 0xB0000 + 0xB8000);
    if (addr >= 0xA0000 && addr < 0xA0000 + 64000 && screen_mode == 13) {
        int px = gfx_point((addr - 0xA0000) % 320, (addr - 0xA0000) / 320);
        return px < 0 ? 0 : px;
    }
    if (addr >= 0x400 && addr < 0x500) {  // the BIOS data area
        static const uint8_t bios_mode[14] = {3, 4, 6, 0, 0, 0, 0, 0x0d, 0x0e, 0x10, 0, 0, 0x12, 0x13};
        uint32_t ticks = (uint32_t)(fmod(port_seconds(), 86400) * 18.2065);
        switch (addr) {
        case 0x449: return screen_mode < 14 ? bios_mode[screen_mode] : 3;
        case 0x44a: return SCREEN_COLS;
        case 0x46c: return ticks & 0xff;
        case 0x46d: return (ticks >> 8) & 0xff;
        case 0x46e: return (ticks >> 16) & 0xff;
        case 0x484: return SCREEN_ROWS - 1;
        default: return 0;
        }
    }
    error(E_ILLEGAL_CALL);
    return 0;
}

static void poke_byte(long addr, int v) {
    if (v < 0 || v > 255)
        error(E_ILLEGAL_CALL);
    int cx, cy;
    if (cga_pixels(addr, &cx, &cy)) {
        int bits = screen_mode == 2 ? 1 : 2, n = 8 / bits;
        for (int i = 0; i < n; ++i)
            raw_plot(cx + i, cy, attr_map[(v >> (8 - bits * (i + 1))) & ((1 << bits) - 1)]);
        return;
    }
    if ((screen_mode == 1 || screen_mode == 2) && addr >= 0xB8000 && addr < 0xBC000)
        return;  // the spare bytes at the end of each CGA bank
    if (addr >= 0xB0000 && addr < 0xB0000 + SCREEN_ROWS * SCREEN_COLS * 2)
        addr += 0xB8000 - 0xB0000;
    if (addr >= 0xB8000 && addr < 0xB8000 + SCREEN_ROWS * SCREEN_COLS * 2) {
        int cell = (addr - 0xB8000) / 2, row = cell / SCREEN_COLS, col = cell % SCREEN_COLS;
        uint8_t ch, at;
        port_get_cell(row, col, &ch, &at);
        if (addr & 1)
            at = v;
        else
            ch = v;
        out_flush();
        port_set_cell(row, col, ch, at);
        return;
    }
    if (addr >= 0xA0000 && addr < 0xA0000 + 64000 && screen_mode == 13) {
        plot((addr - 0xA0000) % 320, (addr - 0xA0000) / 320, colour_rgb(v));
        return;
    }
    if (addr >= 0x400 && addr < 0x500)
        return;  // BIOS flags: nothing to change
    error(E_ILLEGAL_CALL);
}

/// BSAVE "file", offset, length: the bytes, after GW-BASIC's 7-byte header.
static void do_bsave(void) {
    char name[64];
    file_name_arg(name, sizeof name, false);
    expect(",");
    double off = num_expr();
    expect(",");
    double len = num_expr();
    long addr = seg_address(off);
    if (len < 0 || len > 65535)
        error(E_ILLEGAL_CALL);
    FIL f;
    FRESULT r = f_open(&f, name, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK)
        error(fresult_error(r));
    uint8_t buf[512];
    long o = (long)lround(off) & 0xffff;
    int n = (int)len;
    uint8_t head[7] = {0xFD, def_seg & 0xff, def_seg >> 8, o & 0xff, o >> 8, n & 0xff, n >> 8};
    UINT w;
    r = f_write(&f, head, 7, &w);
    for (int done = 0; r == FR_OK && done < n;) {
        int k = n - done < (int)sizeof buf ? n - done : (int)sizeof buf;
        for (int i = 0; i < k; ++i)
            buf[i] = peek_byte(addr + done + i);
        r = f_write(&f, buf, k, &w);
        done += k;
    }
    f_close(&f);
    if (r != FR_OK)
        error(fresult_error(r));
}

/// BLOAD "file"[, offset]: back where it was saved, or at offset in DEF SEG.
static void do_bload(void) {
    char name[64];
    file_name_arg(name, sizeof name, false);
    double off = -1;
    if (accept(","))
        off = num_expr();
    FIL f;
    FRESULT r = f_open(&f, name, FA_READ);
    if (r != FR_OK)
        error(fresult_error(r));
    uint8_t head[7], buf[512];
    UINT got;
    r = f_read(&f, head, 7, &got);
    if (r != FR_OK || got != 7 || head[0] != 0xFD) {
        f_close(&f);
        error(E_BAD_FILE_MODE);
    }
    long addr = off >= 0 ? seg_address(off) : (head[1] | head[2] << 8) * 16L + (head[3] | head[4] << 8);
    int n = head[5] | head[6] << 8;
    for (int done = 0; done < n;) {
        int k = n - done < (int)sizeof buf ? n - done : (int)sizeof buf;
        if (f_read(&f, buf, k, &got) != FR_OK || !got)
            break;
        for (UINT i = 0; i < got; ++i)
            poke_byte(addr + done + i, buf[i]);
        done += got;
    }
    f_close(&f);
}

/// VIEW [[SCREEN] (x1,y1)-(x2,y2) [, [fill] [, border]]], or VIEW PRINT
/// [top TO bottom] for the text lines that scroll.
static void do_view(void) {
    if (accept_word("PRINT")) {
        out_flush();
        if (at_end_of_statement()) {
            printf("\x1b[r");
        } else {
            int top = arg_int();
            if (!accept_word("TO"))
                error(E_SYNTAX);
            int bottom = arg_int();
            if (top < 1 || bottom > SCREEN_ROWS || top > bottom)
                error(E_ILLEGAL_CALL);
            printf("\x1b[%d;%dr\x1b[%d;1H", top, bottom, top);
            out_row = top - 1;
            out_col = 0;
        }
        fflush(stdout);
        return;
    }
    need_graphics();
    if (at_end_of_statement()) {
        view_set = false;  // (a WINDOW stays, now over the whole screen)
        clip_x1 = clip_y1 = 0;
        clip_x2 = gw - 1;
        clip_y2 = gh - 1;
        return;
    }
    bool screen = accept_word("SCREEN");
    expect("(");
    int x1 = arg_int();
    expect(",");
    int y1 = arg_int();
    expect(")");
    expect("-");
    expect("(");
    int x2 = arg_int();
    expect(",");
    int y2 = arg_int();
    expect(")");
    if (x1 > x2) {
        int t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y1 > y2) {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0 || y1 < 0 || x2 >= gw || y2 >= gh)
        error(E_ILLEGAL_CALL);
    double fill, border;
    bool has_fill = optional_arg(&fill);
    bool has_border = optional_arg(&border);
    view_set = true;
    view_screen = screen;
    view_x1 = x1;
    view_y1 = y1;
    view_x2 = x2;
    view_y2 = y2;
    if (has_border) {  // a frame just outside the viewport
        clip_x1 = clip_y1 = 0;
        clip_x2 = gw - 1;
        clip_y2 = gh - 1;
        uint8_t rgb = colour_rgb((int)lround(border));
        draw_line(x1 - 1, y1 - 1, x2 + 1, y1 - 1, rgb, 0xffff);
        draw_line(x2 + 1, y1 - 1, x2 + 1, y2 + 1, rgb, 0xffff);
        draw_line(x2 + 1, y2 + 1, x1 - 1, y2 + 1, rgb, 0xffff);
        draw_line(x1 - 1, y2 + 1, x1 - 1, y1 - 1, rgb, 0xffff);
    }
    clip_x1 = x1;
    clip_y1 = y1;
    clip_x2 = x2;
    clip_y2 = y2;
    if (has_fill)
        for (int y = y1; y <= y2; ++y)
            hspan(x1, x2, y, colour_rgb((int)lround(fill)));
    last_x = world_x((x1 + x2) / 2.0);
    last_y = world_y((y1 + y2) / 2.0);
}

/// PMAP(v, n): world x (n 0) or y (1) to pixels, or pixels to world x (2)
/// or y (3); pixels count from the viewport's corner.
static double pmap(double v, int n) {
    need_graphics();
    int ox = view_set && !view_screen ? view_x1 : 0, oy = view_set && !view_screen ? view_y1 : 0;
    switch (n) {
    case 0: return round(phys_x(v)) - ox;
    case 1: return round(phys_y(v)) - oy;
    case 2: return world_x(v + ox);
    case 3: return world_y(v + oy);
    }
    error(E_ILLEGAL_CALL);
    return 0;
}

/// POINT(n): the graphics cursor in pixels (n 0, 1) or world units (2, 3).
static double point_cursor(int n) {
    need_graphics();
    return n == 0 ? phys_x(last_x) : n == 1 ? phys_y(last_y) : n == 2 ? last_x : last_y;
}

/// WINDOW [[SCREEN] (x1,y1)-(x2,y2)]: world coordinates for the viewport, y
/// going up (or down, with SCREEN, as on the screen).
static void do_window(void) {
    need_graphics();
    if (at_end_of_statement()) {
        window_set = false;
        return;
    }
    bool screen = accept_word("SCREEN");
    expect("(");
    double x1 = num_expr();
    expect(",");
    double y1 = num_expr();
    expect(")");
    expect("-");
    expect("(");
    double x2 = num_expr();
    expect(",");
    double y2 = num_expr();
    expect(")");
    if (x1 == x2 || y1 == y2)
        error(E_ILLEGAL_CALL);
    window_set = true;
    window_screen = screen;
    win_x1 = fmin(x1, x2);
    win_x2 = fmax(x1, x2);
    win_y1 = fmin(y1, y2);
    win_y2 = fmax(y1, y2);
    last_x = (win_x1 + win_x2) / 2;
    last_y = (win_y1 + win_y2) / 2;
}

// ---- Graphics GET and PUT -------------------------------------------------------
//
// GET (x1,y1)-(x2,y2), a[(i)] keeps a rectangle of pixels in a numeric array
// and PUT (x,y), a[(i)][, PSET|PRESET|XOR|OR|AND] draws it back. As in
// GW-BASIC the first element holds the size and the pixels are packed at
// the mode's bits per pixel, so arrays dimensioned by GW-BASIC's formula are
// big enough: here 32 bits go in each element.

static int mode_bits(void) {
    return gcolours <= 2 ? 1 : gcolours <= 4 ? 2 : gcolours <= 16 ? 4 : 8;
}

/// The array named at p and the element to start at.
static double *graphics_array(long *room) {
    char name[NAME_MAX + 2];
    if (!read_name(name))
        error(E_SYNTAX);
    array_t *a = find_array(name);
    if (!a || a->is_str)
        error(E_ILLEGAL_CALL);
    long start = 0;
    if (accept("(")) {
        int idx[8], n = 0;
        do
            idx[n++] = arg_int();
        while (n < 8 && accept(","));
        expect(")");
        if (n != a->dims)
            error(E_SUBSCRIPT);
        for (int i = 0; i < n; ++i) {
            if (idx[i] < option_base || idx[i] >= a->size[i])
                error(E_SUBSCRIPT);
            start = start * a->size[i] + idx[i];
        }
    }
    *room = a->count - start;
    return a->n + start;
}

static void do_gget(void) {
    need_graphics();
    double x1, y1, x2, y2;
    read_point(&x1, &y1);
    expect("-");
    read_point(&x2, &y2);
    expect(",");
    long room;
    double *a = graphics_array(&room);
    int ix1 = px_of(x1), iy1 = py_of(y1), ix2 = px_of(x2), iy2 = py_of(y2);
    if (ix1 > ix2) {
        int t = ix1;
        ix1 = ix2;
        ix2 = t;
    }
    if (iy1 > iy2) {
        int t = iy1;
        iy1 = iy2;
        iy2 = t;
    }
    if (ix1 < 0 || iy1 < 0 || ix2 >= gw || iy2 >= gh)
        error(E_ILLEGAL_CALL);
    int w = ix2 - ix1 + 1, h = iy2 - iy1 + 1, bits = mode_bits();
    long need = 1 + ((long)w * h * bits + 31) / 32;
    if (need > room)
        error(E_ILLEGAL_CALL);
    a[0] = w + h * 65536.0;
    uint32_t word = 0;
    int used = 0;
    long k = 1;
    for (int y = iy1; y <= iy2; ++y)
        for (int x = ix1; x <= ix2; ++x) {
            word |= (uint32_t)(gfx_point(x, y) & ((1 << bits) - 1)) << used;
            if ((used += bits) == 32) {
                a[k++] = word;
                word = 0;
                used = 0;
            }
        }
    if (used)
        a[k] = word;
}

static void do_gput(void) {
    need_graphics();
    double x, y;
    read_point(&x, &y);
    expect(",");
    long room;
    double *a = graphics_array(&room);
    int action = 2;  // 0 PSET, 1 PRESET, 2 XOR, 3 OR, 4 AND
    if (accept(",")) {
        if (accept_word("PSET"))
            action = 0;
        else if (accept_word("PRESET"))
            action = 1;
        else if (accept_word("XOR"))
            action = 2;
        else if (accept_word("OR"))
            action = 3;
        else if (accept_word("AND"))
            action = 4;
        else
            error(E_SYNTAX);
    }
    uint32_t size = (uint32_t)a[0];
    int w = size & 0xffff, h = size >> 16, bits = mode_bits(), mask = (1 << bits) - 1;
    if (w <= 0 || h <= 0 || 1 + ((long)w * h * bits + 31) / 32 > room)
        error(E_ILLEGAL_CALL);
    int ix = px_of(x), iy = py_of(y);
    if (ix < 0 || iy < 0 || ix + w > gw || iy + h > gh)
        error(E_ILLEGAL_CALL);
    long k = 1;
    int used = 32;
    uint32_t word = 0;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            if (used == 32) {
                word = (uint32_t)a[k++];
                used = 0;
            }
            int c = (word >> used) & mask, old = gfx_point(ix + xx, iy + yy) & mask;
            used += bits;
            switch (action) {
            case 1: c = ~c & mask; break;
            case 2: c ^= old; break;
            case 3: c |= old; break;
            case 4: c &= old; break;
            }
            if (c < gcolours)
                plot(ix + xx, iy + yy, colour_rgb(c));
        }
    last_x = x;
    last_y = y;
}

// ---- Sprites (an extension) -----------------------------------------------------
//
// SPRITE SHAPE s, a[(i)] keeps a picture made by GET as shape s (colour 0 is
// see-through). SPRITE n, x, y[, s[, dx, dy]] puts sprite n at (x,y) in the
// mode's pixels, with shape s and a speed it keeps; SPRITE n OFF hides it;
// SPRITE CLEAR hides them all. Nothing is drawn until SPRITE UPDATE [f]: it
// puts back what was behind every sprite (but not over drawing the program has
// done on it since), moves each by its speed times f
// (one that moves right off the screen goes off), and draws them all again,
// higher numbers on top, keeping what is behind them. SPRITEHIT(n[, first,
// last]) is the first sprite whose pixels touch sprite n's (or -1);
// SPRITEX(n), SPRITEY(n) and SPRITEON(n) read them back.

#define SPRITES 64
#define SHAPES  64

typedef struct {
    int w, h;
    uint8_t *px;    ///< colours (RGB332), w * h
    uint8_t *mask;  ///< 1 where the shape isn't see-through
} shape_t;

typedef struct {
    bool on, drawn;
    int shape;
    double x, y, dx, dy;
    int ox, oy, oshape;  ///< where and as what it was drawn,
    int bx, by, bw, bh;  ///< the part on screen, and what was there:
    uint8_t *bg;
    int bg_cap;
} sprite_t;

/// Made when BASIC starts and freed when it ends (no app keeps SRAM while
/// it isn't running).
static shape_t *shapes;    ///< SHAPES of them
static sprite_t *sprites;  ///< SPRITES of them

/// Forgets what is behind the sprites (the screen has been cleared).
static void sprites_forget(void) {
    for (int i = 0; sprites && i < SPRITES; ++i)
        sprites[i].drawn = false;
}

static void sprites_reset(void) {
    if (!sprites)
        return;
    for (int i = 0; i < SPRITES; ++i) {
        free(sprites[i].bg);
        memset(&sprites[i], 0, sizeof sprites[i]);
    }
    for (int i = 0; i < SHAPES; ++i) {
        free(shapes[i].px);
        free(shapes[i].mask);
        memset(&shapes[i], 0, sizeof shapes[i]);
    }
}

/// Puts back what was behind a sprite's pixels, except where the program has
/// drawn over them since (that drawing stays).
static void sprite_restore(sprite_t *sp) {
    if (!sp->drawn)
        return;
    const shape_t *sh = &shapes[sp->oshape];
    const uint8_t *b = sp->bg;
    for (int y = sp->by; y < sp->by + sp->bh; ++y) {
        int k = (y - sp->oy) * sh->w + (sp->bx - sp->ox);
        for (int x = sp->bx; x < sp->bx + sp->bw; ++x, ++k, ++b)
            if (sh->mask[k] && read_rgb(x, y) == sh->px[k])
                port_fill_rect(x * gsx, y * gsy + gyoff, gsx, gsy, *b);
    }
    sp->drawn = false;
}

static void sprite_draw(sprite_t *sp) {
    const shape_t *sh = &shapes[sp->shape];
    int x0 = round_coord(sp->x), y0 = round_coord(sp->y);
    int cx1 = x0 < clip_x1 ? clip_x1 : x0, cy1 = y0 < clip_y1 ? clip_y1 : y0;
    int cx2 = x0 + sh->w - 1 > clip_x2 ? clip_x2 : x0 + sh->w - 1;
    int cy2 = y0 + sh->h - 1 > clip_y2 ? clip_y2 : y0 + sh->h - 1;
    if (cx1 > cx2 || cy1 > cy2)
        return;
    int w = cx2 - cx1 + 1, h = cy2 - cy1 + 1;
    if (w * h > sp->bg_cap) {
        uint8_t *bg = malloc(w * h);
        if (!bg)
            error(E_OUT_OF_MEMORY);
        free(sp->bg);
        sp->bg = bg;
        sp->bg_cap = w * h;
    }
    uint8_t *b = sp->bg;
    for (int y = cy1; y <= cy2; ++y)
        for (int x = cx1; x <= cx2; ++x)
            *b++ = read_rgb(x, y);
    for (int y = cy1; y <= cy2; ++y) {
        int k = (y - y0) * sh->w + (cx1 - x0);
        for (int x = cx1; x <= cx2; ++x, ++k)
            if (sh->mask[k])
                port_fill_rect(x * gsx, y * gsy + gyoff, gsx, gsy, sh->px[k]);
    }
    sp->ox = x0;
    sp->oy = y0;
    sp->oshape = sp->shape;
    sp->bx = cx1;
    sp->by = cy1;
    sp->bw = w;
    sp->bh = h;
    sp->drawn = true;
}

static void sprite_update(double f) {
    for (int i = SPRITES - 1; i >= 0; --i)
        sprite_restore(&sprites[i]);
    for (int i = 0; i < SPRITES; ++i) {
        sprite_t *sp = &sprites[i];
        if (!sp->on || (!sp->dx && !sp->dy))
            continue;
        sp->x += sp->dx * f;
        sp->y += sp->dy * f;
        const shape_t *sh = &shapes[sp->shape];
        if (sp->x + sh->w <= 0 || sp->y + sh->h <= 0 || sp->x >= gw || sp->y >= gh)
            sp->on = false;
    }
    for (int i = 0; i < SPRITES; ++i)
        if (sprites[i].on)
            sprite_draw(&sprites[i]);
}

static int sprite_number(void) {
    int n = arg_int();
    if (n < 0 || n >= SPRITES)
        error(E_ILLEGAL_CALL);
    return n;
}

static void do_sprite_shape(void) {
    int s = arg_int();
    if (s < 0 || s >= SHAPES)
        error(E_ILLEGAL_CALL);
    expect(",");
    long room;
    double *a = graphics_array(&room);
    uint32_t size = (uint32_t)a[0];
    int w = size & 0xffff, h = size >> 16, bits = mode_bits(), mask = (1 << bits) - 1;
    if (w <= 0 || h <= 0 || 1 + ((long)w * h * bits + 31) / 32 > room)
        error(E_ILLEGAL_CALL);
    uint8_t *px = malloc(w * h), *m = malloc(w * h);
    if (!px || !m) {
        free(px);
        free(m);
        error(E_OUT_OF_MEMORY);
    }
    long k = 1;
    int used = 32;
    uint32_t word = 0;
    for (int i = 0; i < w * h; ++i) {
        if (used == 32) {
            word = (uint32_t)a[k++];
            used = 0;
        }
        int c = (word >> used) & mask;
        used += bits;
        m[i] = c != 0;
        px[i] = c < gcolours ? colour_rgb(c) : 0;
    }
    shape_t *sh = &shapes[s];
    free(sh->px);
    free(sh->mask);
    sh->w = w;
    sh->h = h;
    sh->px = px;
    sh->mask = m;
}

static void do_sprite(void) {
    need_graphics();
    if (accept_word("SHAPE")) {
        do_sprite_shape();
    } else if (accept_word("UPDATE")) {
        double f = 1;
        skip_spaces();
        if (!at_end_of_statement())
            f = num_expr();
        sprite_update(f);
    } else if (accept_word("CLEAR")) {
        for (int i = SPRITES - 1; i >= 0; --i) {
            sprite_restore(&sprites[i]);
            sprites[i].on = false;
        }
    } else {
        sprite_t *sp = &sprites[sprite_number()];
        if (accept_word("OFF")) {
            sp->on = false;
            return;
        }
        expect(",");
        double x = num_expr();
        expect(",");
        double y = num_expr();
        int s = sp->shape;
        bool has_shape = sp->on || shapes[s].px;
        if (accept(",")) {
            s = arg_int();
            if (s < 0 || s >= SHAPES)
                error(E_ILLEGAL_CALL);
            has_shape = true;
            if (accept(",")) {
                sp->dx = num_expr();
                expect(",");
                sp->dy = num_expr();
            }
        }
        if (!has_shape || !shapes[s].px)
            error(E_ILLEGAL_CALL);
        sp->x = x;
        sp->y = y;
        sp->shape = s;
        sp->on = true;
    }
}

/// SPRITEHIT(n[, first, last]): the lowest-numbered sprite from first to last
/// (all of them) that is on and has a pixel where sprite n has one, or -1.
static int sprite_hit(int n, int first, int last) {
    const sprite_t *a = &sprites[n];
    if (!a->on)
        return -1;
    const shape_t *sa = &shapes[a->shape];
    int ax = round_coord(a->x), ay = round_coord(a->y);
    for (int i = first; i <= last; ++i) {
        const sprite_t *b = &sprites[i];
        if (i == n || !b->on)
            continue;
        const shape_t *sb = &shapes[b->shape];
        int bx = round_coord(b->x), by = round_coord(b->y);
        int x1 = ax > bx ? ax : bx, y1 = ay > by ? ay : by;
        int x2 = ax + sa->w < bx + sb->w ? ax + sa->w : bx + sb->w;
        int y2 = ay + sa->h < by + sb->h ? ay + sa->h : by + sb->h;
        for (int y = y1; y < y2; ++y)
            for (int x = x1; x < x2; ++x)
                if (sa->mask[(y - ay) * sa->w + x - ax] && sb->mask[(y - by) * sb->w + x - bx])
                    return i;
    }
    return -1;
}

static value_t sprite_function(int what) {
    int n = sprite_number();
    if (what == 0) {
        int first = 0, last = SPRITES - 1;
        if (accept(",")) {
            first = sprite_number();
            expect(",");
            last = sprite_number();
        }
        expect(")");
        return num(sprite_hit(n, first, last));
    }
    expect(")");
    const sprite_t *sp = &sprites[n];
    return num(what == 1 ? sp->x : what == 2 ? sp->y : sp->on ? -1 : 0);
}

/// CLS [n]: 1 clears just the graphics (on the active page); a hidden active
/// page is cleared without touching the screen.
static void do_cls(void) {
    int n = -1;
    skip_spaces();
    if (!at_end_of_statement() && ((n = arg_int()) < 0 || n > 2))
        error(E_ILLEGAL_CALL);
    if (screen_mode && view_set && (n == 1 || n == -1)) {  // just the viewport
        for (int y = view_y1; y <= view_y2; ++y)
            hspan(view_x1, view_x2, y, colour_rgb(gbg));
        last_x = world_x((view_x1 + view_x2) / 2.0);
        last_y = world_y((view_y1 + view_y2) / 2.0);
        return;
    }
    if (screen_mode && (n == 1 || active_page != visual_page)) {
        sprites_forget();
        if (n == 1)
            port_fill_rect(0, gyoff, FB_W, gh * gsy, colour_rgb(gbg));
        else
            port_fill_rect(0, 0, FB_W, FB_H, colour_rgb(gbg));
        last_x = gw / 2;
        last_y = gh / 2;
        return;
    }
    sprites_forget();
    printf("\x1b[2J\x1b[H");
    if (screen_mode && gbg)
        port_fill_rect(0, 0, FB_W, FB_H, colour_rgb(gbg));
    out_row = out_col = 0;
    out_flush();
    if (key_line_on)
        draw_key_line();
}

// ---- GPIO -----------------------------------------------------------------

/// PINMODE pin, INPUT | OUTPUT | PULLUP | PULLDOWN (or 0-3)
static void do_pinmode(void) {
    int pin = pin_arg();
    expect(",");
    int mode;
    if (accept("INPUT"))
        mode = PORT_PIN_INPUT;
    else if (accept("OUTPUT"))
        mode = PORT_PIN_OUTPUT;
    else if (accept("PULLUP"))
        mode = PORT_PIN_PULLUP;
    else if (accept("PULLDOWN"))
        mode = PORT_PIN_PULLDOWN;
    else if ((mode = arg_int()) < 0 || mode > 3)
        error(E_ILLEGAL_CALL);
    port_pin_mode(pin, mode);
}

/// PIN pin, level
static void do_pin(void) {
    int pin = pin_arg();
    expect(",");
    port_pin_write(pin, arg_int());
}

/// PWM pin, freq [, duty %]   (freq 0 turns it off)
static void do_pwm(void) {
    int pin = pin_arg();
    expect(",");
    double freq = num_expr();
    double duty = 50;
    if (accept(","))
        duty = num_expr();
    if (freq < 0 || freq > 10e6 || (freq > 0 && freq < 8) || duty < 0 || duty > 100)
        error(E_ILLEGAL_CALL);
    out_flush();  // in case it prints a warning
    port_pwm(pin, freq, duty);
}


// ---- I2C and SPI (Pico extensions) ------------------------------------------

#define I2C_SDA_DEFAULT 2   ///< GP2/GP3: I2C1, clear of COM1 (GP4/5), HDMI and the WiFi pins
#define SPI_SCK_DEFAULT 10  ///< GP10 SCK, GP11 MOSI, GP8 MISO: SPI1 (GP9 is free for CS)

/// Bytes from a list of numbers (0-255) and strings, into buf; NOSTOP (at the
/// end) sets *nostop. The count.
static int byte_items(uint8_t *buf, int max, bool *nostop) {
    int n = 0;
    for (;;) {
        if (nostop && accept_word("NOSTOP")) {
            *nostop = true;
            break;
        }
        value_t v = expr();
        if (v.is_str) {
            int len = slen(v.s);
            if (n + len > max)
                error(E_ILLEGAL_CALL);
            memcpy(buf + n, v.s, len);
            n += len;
        } else {
            if (v.n < 0 || v.n > 255 || n >= max)
                error(E_ILLEGAL_CALL);
            buf[n++] = (uint8_t)v.n;
        }
        if (!accept(","))
            break;
    }
    return n;
}

static void i2c_check(int r) {
    if (r == -2)
        error(E_DEVICE_UNAVAILABLE);  // I2C OPEN first
    if (r < 0)
        error(E_DEVICE_IO);           // no device answered at that address
}

static int i2c_addr_arg(void) {
    int a = arg_int();
    if (a < 0 || a > 127)
        error(E_ILLEGAL_CALL);
    return a;
}

static void open_failed(const char *why) {
    if (!why)
        return;
    out_flush();
    printf("%s\n", why);
    error(E_ILLEGAL_CALL);
}

/// I2C OPEN [sda, scl[, khz]] | I2C CLOSE | I2C SCAN
/// | I2C WRITE addr, byte or string[, ...][, NOSTOP]
static void do_i2c(void) {
    if (accept_word("OPEN")) {
        int sda = I2C_SDA_DEFAULT, scl = I2C_SDA_DEFAULT + 1, khz = 100;
        skip_spaces();
        if (!at_end_of_statement()) {
            sda = arg_int();
            expect(",");
            scl = arg_int();
            if (accept(","))
                khz = arg_int();
        }
        open_failed(port_i2c_open(sda, scl, khz));
    } else if (accept_word("CLOSE")) {
        port_i2c_close();
    } else if (accept_word("WRITE")) {
        int addr = i2c_addr_arg();
        expect(",");
        uint8_t buf[256];
        bool nostop = false;
        int n = byte_items(buf, sizeof buf, &nostop);
        i2c_check(port_i2c_write(addr, buf, n, nostop));
    } else if (accept_word("SCAN")) {
        out_flush();
        int found = 0;
        for (int a = 8; a < 120; ++a) {
            uint8_t b;
            int r = port_i2c_read(a, &b, 1);
            i2c_check(r == -2 ? -2 : 0);
            if (r > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%s&H%02X", found++ ? " " : "", a);
                out_str(buf);
            }
        }
        out_str(found ? "" : "No I2C devices answered");
        out_newline();
    } else {
        error(E_SYNTAX);
    }
}

/// I2CREAD$(addr, n[, reg]): n bytes from the device (after writing the
/// register number reg, without a stop, if given). I2CREAD(addr[, reg]): one.
static value_t i2c_read_function(bool string) {
    int addr = i2c_addr_arg(), n = 1, reg = -1;
    if (string) {
        expect(",");
        n = arg_int();
    }
    if (accept(","))
        reg = arg_int();
    expect(")");
    if (n < 1 || n > 255 || reg > 255)
        error(E_ILLEGAL_CALL);
    uint8_t buf[256];
    if (reg >= 0) {
        uint8_t r = (uint8_t)reg;
        i2c_check(port_i2c_write(addr, &r, 1, true));
    }
    i2c_check(port_i2c_read(addr, buf, n));
    return string ? str((const char *)buf, n) : num(buf[0]);
}

/// SPI OPEN [sck, mosi, miso[, khz[, mode]]] | SPI CLOSE
/// | SPI WRITE byte or string[, ...]
static void do_spi(void) {
    if (accept_word("OPEN")) {
        int sck = SPI_SCK_DEFAULT, mosi = SPI_SCK_DEFAULT + 1, miso = SPI_SCK_DEFAULT - 2, khz = 1000, mode = 0;
        skip_spaces();
        if (!at_end_of_statement()) {
            sck = arg_int();
            expect(",");
            mosi = arg_int();
            expect(",");
            miso = arg_int();
            if (accept(",")) {
                khz = arg_int();
                if (accept(","))
                    mode = arg_int();
            }
        }
        open_failed(port_spi_open(sck, mosi, miso, khz, mode));
    } else if (accept_word("CLOSE")) {
        port_spi_close();
    } else if (accept_word("WRITE")) {
        uint8_t buf[256];
        int n = byte_items(buf, sizeof buf, NULL);
        if (!port_spi_transfer(buf, NULL, n))
            error(E_DEVICE_UNAVAILABLE);
    } else {
        error(E_SYNTAX);
    }
}

/// SPI$(out$): sends out$ and gives back the bytes received meanwhile.
static value_t spi_function(void) {
    const char *out = str_expr();
    expect(")");
    int n = slen(out);
    uint8_t in[256];
    if (n && !port_spi_transfer((const uint8_t *)out, in, n))
        error(E_DEVICE_UNAVAILABLE);
    return str((const char *)in, n);
}

// ---- Font sheets: FONT and GPRINT (an extension) ----------------------------
//
// A font sheet is a picture (BMP, PNG or JPEG) of characters in a grid of
// cells, 8x8 to 32x32 pixels (not always square), usually starting at the
// space, CHR$(32). FONT LOAD keeps up to 8 of them; GPRINT draws text with
// one at any place on a graphics screen, in the sheet's own colours (its
// background colour, the top-left pixel's unless FONT KEY says otherwise,
// is see-through) or in one colour. Font 0 is the console's 8x16 font.
// The glyphs live in PSRAM on the board with a checksum each; a glyph
// found damaged has its sheet read again.

#define MAX_FONTS 9

typedef struct {
    bool loaded, prop;
    char path[64];
    int cw, ch, first, count, perrow, gap;
    int key;           ///< the see-through colour (RGB332), -1 none, -2 the top-left pixel's
    uint8_t *pix;      ///< count glyphs of cw x ch: RGB332
    uint8_t *solid;    ///< 1 where drawn
    uint8_t *left, *width;  ///< each glyph's drawn columns (for proportional spacing)
    uint16_t *sum;     ///< each glyph's checksum
} font_t;

static font_t (*fonts_p)[MAX_FONTS];  ///< (basic_ram, while BASIC runs)
#define fonts (*fonts_p)
static int cur_font;
static double gp_x, gp_y;  ///< where GPRINT goes on (world coordinates)

static void font_free(font_t *f) {
    free(f->pix);
    free(f->solid);
    free(f->left);
    free(f->width);
    free(f->sum);
    memset(f, 0, sizeof *f);
}

static void fonts_reset(void) {
    for (int i = 1; i < MAX_FONTS; ++i)
        font_free(&fonts[i]);
    cur_font = 0;
    gp_x = gp_y = 0;
}

static uint16_t glyph_sum(const font_t *f, int g) {
    size_t n = (size_t)f->cw * f->ch, at = (size_t)g * n;
    uint16_t h = 1;
    for (size_t i = 0; i < n; ++i)
        h = (uint16_t)(h * 31 + f->pix[at + i] + (f->solid[at + i] << 8));
    return h;
}

/// The cell size and glyphs a row of a sheet w x h pixels: the layout that
/// gives a usual number of characters (96, 64, 60...), square cells first.
static bool font_guess(int w, int h, int *cw, int *ch, int *perrow) {
    static const int per[] = {16, 20, 32, 10, 8, 12, 24, 40, 64, 15, 18};
    static const int usual[] = {96, 95, 64, 60, 59, 80, 128, 224, 256, 48, 40};
    int best = -1;
    for (unsigned i = 0; i < sizeof per / sizeof *per; ++i) {
        if (w % per[i])
            continue;
        int c = w / per[i];
        if (c < 8 || c > 32)
            continue;
        for (int r = 8; r <= 32; ++r) {
            if (h % r > r / 4 || h < r)
                continue;  // (a few spare rows of pixels at the bottom are allowed)
            int count = per[i] * (h / r), score = 0;
            if (count > 256)
                continue;
            for (unsigned k = 0; k < sizeof usual / sizeof *usual; ++k)
                if (count == usual[k])
                    score = 100 - (int)k;
            score += r == c ? 20 : r == 2 * c ? 10 : 0;
            score -= h % r ? 5 : 0;
            score += (int)(sizeof per / sizeof *per - i);
            if (score > best) {
                best = score;
                *cw = c;
                *ch = r;
                *perrow = per[i];
            }
        }
    }
    return best >= 0;
}

static uint8_t rgb_to_332(const uint8_t *p) {
    return (uint8_t)(((p[0] * 7 + 127) / 255) << 5 | ((p[1] * 7 + 127) / 255) << 2 | (p[2] * 3 + 127) / 255);
}

/// Reads the sheet into f (its path, sizes, first and key already set; cw = 0:
/// guessed). NULL if fine, else why not.
/// A size in the file's name, as in "16X16-F2.PNG" or "font8x16.bmp".
static bool font_size_in_name(const char *path, int *cw, int *ch) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char *q = base; *q; ++q) {
        if (!isdigit((unsigned char)*q) || (q > base && isdigit((unsigned char)q[-1])))
            continue;
        char *end;
        long w = strtol(q, &end, 10);
        if ((*end != 'x' && *end != 'X') || !isdigit((unsigned char)end[1]))
            continue;
        long h = strtol(end + 1, NULL, 10);
        if (w >= 4 && w <= 64 && h >= 4 && h <= 64) {
            *cw = (int)w;
            *ch = (int)h;
            return true;
        }
    }
    return false;
}

static const char *font_read(font_t *f) {
    image_t img;
    const char *err;
    if (!image_load(f->path, 0, 0, &img, &err))
        return err;
    if (!f->cw && font_size_in_name(f->path, &f->cw, &f->ch) && (img.w < f->cw || img.h < f->ch))
        f->cw = f->ch = 0;  // the name's numbers weren't the cell size
    if (!f->cw && !font_guess(img.w, img.h, &f->cw, &f->ch, &f->perrow)) {
        image_free(&img);
        return "can't tell the cell size: give it (FONT LOAD n, file, width, height)";
    }
    if (!f->perrow)
        f->perrow = img.w / f->cw;
    int rows = img.h / f->ch;
    f->count = f->perrow * rows;
    if (f->first + f->count > 256)
        f->count = 256 - f->first;
    if (f->count < 1 || f->perrow * f->cw > img.w) {
        image_free(&img);
        return "the cells don't fit the picture";
    }
    size_t n = (size_t)f->count * f->cw * f->ch;
    free(f->pix);
    free(f->solid);
    free(f->left);
    free(f->width);
    free(f->sum);
    f->pix = malloc(n);
    f->solid = malloc(n);
    f->left = malloc(f->count);
    f->width = malloc(f->count);
    f->sum = malloc(f->count * sizeof *f->sum);
    if (!f->pix || !f->solid || !f->left || !f->width || !f->sum) {
        image_free(&img);
        return "not enough memory";
    }
    int key = f->key == -2 ? rgb_to_332(img.rgb) : f->key;
    for (int g = 0; g < f->count; ++g) {
        int gx = g % f->perrow * f->cw, gy = g / f->perrow * f->ch, lo = f->cw, hi = -1;
        uint8_t *pix = f->pix + (size_t)g * f->cw * f->ch, *solid = f->solid + (size_t)g * f->cw * f->ch;
        for (int y = 0; y < f->ch; ++y)
            for (int x = 0; x < f->cw; ++x) {
                uint8_t v = rgb_to_332(img.rgb + ((size_t)(gy + y) * img.w + gx + x) * 3);
                pix[y * f->cw + x] = v;
                solid[y * f->cw + x] = v != key;
                if (v != key) {
                    lo = x < lo ? x : lo;
                    hi = x > hi ? x : hi;
                }
            }
        f->left[g] = hi < 0 ? 0 : lo;
        f->width[g] = hi < 0 ? 0 : hi - lo + 1;
        f->sum[g] = glyph_sum(f, g);
    }
    image_free(&img);
    f->loaded = true;
    return NULL;
}

static int font_slot(int n, bool loaded) {
    if (n < 0 || n >= MAX_FONTS || (loaded && n && !fonts[n].loaded))
        error(E_ILLEGAL_CALL);
    return n;
}

/// FONTW(n), FONTH(n): the cell size, 0 if nothing is loaded there.
static int font_size(int n, bool width) {
    if (!n)
        return width ? 8 : 16;
    return !fonts[n].loaded ? 0 : width ? fonts[n].cw : fonts[n].ch;
}

/// The glyph for character c in font f, or -1 (lower case falls back on upper
/// case when the sheet has none).
static int font_glyph(const font_t *f, int c) {
    int g = c - f->first;
    bool have = g >= 0 && g < f->count && (f->width[g] || c == ' ');
    if (!have && c >= 'a' && c <= 'z')
        return font_glyph(f, c - 32);
    return have ? g : -1;
}

/// How far character c moves the cursor, in the font's pixels.
static int font_advance(int n, int c) {
    if (!n)
        return 8;
    const font_t *f = &fonts[n];
    int g = font_glyph(f, c);
    if (!f->prop)
        return f->cw + f->gap;
    if (g < 0 || !f->width[g])
        return f->cw / 2 + f->gap;
    return f->width[g] + f->gap;
}

static int text_width(int n, const char *s, int len) {
    int w = 0;
    for (int i = 0; i < len; ++i)
        w += font_advance(n, (unsigned char)s[i]);
    return w;
}

/// A box of the mode's pixels, clipped to the viewport.
static void fill_box(int x, int y, int w, int h, uint8_t rgb) {
    if (x < clip_x1)
        w -= clip_x1 - x, x = clip_x1;
    if (y < clip_y1)
        h -= clip_y1 - y, y = clip_y1;
    if (x + w - 1 > clip_x2)
        w = clip_x2 - x + 1;
    if (y + h - 1 > clip_y2)
        h = clip_y2 - y + 1;
    if (w > 0 && h > 0)
        port_fill_rect(x * gsx, y * gsy + gyoff, w * gsx, h * gsy, rgb);
}

/// In modes of 16 colours or fewer, the sheet's colours become the nearest
/// in the palette (so POINT and PALETTE work on them); SCREEN 13 shows them
/// as they are.
static uint8_t palette_colour(uint8_t v) {
    static uint8_t map[256], valid[256 / 8], for_attr[16];
    static int for_mode = -1;
    if (gcolours > 16)
        return v;
    if (for_mode != screen_mode || memcmp(for_attr, attr_map, gcolours)) {
        memset(valid, 0, sizeof valid);
        memcpy(for_attr, attr_map, gcolours);
        for_mode = screen_mode;
    }
    if (!(valid[v >> 3] & (1 << (v & 7)))) {
        int r = (v >> 5) * 255 / 7, g = (v >> 2 & 7) * 255 / 7, b = (v & 3) * 85, best = 1 << 30;
        for (int i = 0; i < gcolours; ++i) {
            int a = attr_map[i], dr = r - (a >> 5) * 255 / 7, dg = g - (a >> 2 & 7) * 255 / 7, db = b - (a & 3) * 85;
            int d = dr * dr + dg * dg + db * db;
            if (d < best)
                best = d, map[v] = (uint8_t)a;
        }
        valid[v >> 3] |= 1 << (v & 7);
    }
    return map[v];
}

/// Draws character c at (x, y) (the mode's pixels, top left), each font pixel
/// `scale` pixels across, in its own colours or `rgb` (>= 0). Returns the
/// advance in the mode's pixels.
static int draw_char(int n, int c, int x, int y, int scale, int rgb) {
    if (!n) {
        const uint8_t *bits = font8x16[c & 255];
        for (int r = 0; r < 16; ++r)
            for (int k = 0; k < 8; ++k) {
                if (!(bits[r] & (0x80 >> k)))
                    continue;
                int run = 1;
                while (k + run < 8 && (bits[r] & (0x80 >> (k + run))))
                    ++run;
                fill_box(x + k * scale, y + r * scale, run * scale, scale, (uint8_t)rgb);
                k += run - 1;
            }
        return 8 * scale;
    }
    font_t *f = &fonts[n];
    int g = font_glyph(f, c);
    if (g >= 0 && f->sum[g] != glyph_sum(f, g)) {
        // Damaged in memory: read the sheet again.
        const char *why = font_read(f);
        if (why) {
            font_free(f);
            error(E_ILLEGAL_CALL);
        }
    }
    int adv = font_advance(n, c) * scale;
    if (g < 0 || !f->width[g])
        return adv;
    int x0 = f->prop ? f->left[g] : 0, x1 = f->prop ? f->left[g] + f->width[g] : f->cw;
    const uint8_t *pix = f->pix + (size_t)g * f->cw * f->ch, *solid = f->solid + (size_t)g * f->cw * f->ch;
    for (int r = 0; r < f->ch; ++r) {
        const uint8_t *pr = pix + r * f->cw, *sr = solid + r * f->cw;
        for (int k = x0; k < x1; ++k) {
            if (!sr[k])
                continue;
            uint8_t v = rgb >= 0 ? (uint8_t)rgb : rgb == -2 ? pr[k] : palette_colour(pr[k]);
            int run = 1;
            while (k + run < x1 && sr[k + run] && (rgb >= 0 || pr[k + run] == pr[k]))
                ++run;
            fill_box(x + (k - x0) * scale, y + r * scale, run * scale, scale, v);
            k += run - 1;
        }
    }
    return adv;
}

/// FONT n | FONT LOAD n, "file" [, w, h [, first [, per row]]] | FONT KEY n, c
/// | FONT PROP n, ON|OFF [, gap] | FONT CLEAR [n]
static void do_font(void) {
    if (accept_word("LOAD")) {
        int n = arg_int();
        if (n < 1 || n >= MAX_FONTS)
            error(E_ILLEGAL_CALL);
        expect(",");
        char name[64];
        file_name_arg(name, sizeof name, false);
        int cw = 0, ch = 0, first = 32, perrow = 0;
        if (accept(",")) {
            cw = arg_int();
            expect(",");
            ch = arg_int();
            if (cw < 4 || cw > 64 || ch < 4 || ch > 64)
                error(E_ILLEGAL_CALL);
            if (accept(",")) {
                first = arg_int();
                if (accept(","))
                    perrow = arg_int();
            }
        }
        if (first < 0 || first > 255 || perrow < 0)
            error(E_ILLEGAL_CALL);
        out_flush();
        font_t *f = &fonts[n];
        font_free(f);
        snprintf(f->path, sizeof f->path, "%s", name);
        f->cw = cw;
        f->ch = ch;
        f->first = first;
        f->perrow = perrow;
        f->key = -2;
        const char *why = font_read(f);
        if (why) {
            font_free(f);
            printf("%s: %s\n", name, why);
            error(!strcmp(why, "file not found") ? E_FILE_NOT_FOUND : E_ILLEGAL_CALL);
        }
        if (!running) {
            char buf[96];
            snprintf(buf, sizeof buf, "Font %d: %dx%d cells, %d a row, CHR$(%d) to CHR$(%d)", n, f->cw,
                     f->ch, f->perrow, f->first, f->first + f->count - 1);
            out_str(buf);
            out_newline();
        }
        return;
    }
    if (accept_word("KEY")) {
        int n = font_slot(arg_int(), true);
        expect(",");
        int c = arg_int();
        if (!n)
            error(E_ILLEGAL_CALL);
        fonts[n].key = c < 0 ? -1 : colour_rgb(c);
        out_flush();
        if (font_read(&fonts[n])) {
            font_free(&fonts[n]);
            error(E_ILLEGAL_CALL);
        }
        return;
    }
    if (accept_word("PROP")) {
        int n = font_slot(arg_int(), true);
        expect(",");
        bool on;
        if (accept_word("ON"))
            on = true;
        else if (accept_word("OFF"))
            on = false;
        else
            on = num_expr() != 0;
        int gap = on ? 1 : 0;
        if (accept(","))
            gap = arg_int();
        if (!n || gap < -8 || gap > 32)
            error(E_ILLEGAL_CALL);
        fonts[n].prop = on;
        fonts[n].gap = gap;
        return;
    }
    if (accept_word("CLEAR")) {
        skip_spaces();
        if (at_end_of_statement()) {
            fonts_reset();
        } else {
            int n = font_slot(arg_int(), false);
            font_free(&fonts[n]);
            if (cur_font == n)
                cur_font = 0;
        }
        return;
    }
    cur_font = font_slot(arg_int(), true);
}

/// GPRINT [x, y,] item [; item ...] [, scale [, colour]]: text in the
/// current FONT at (x, y), or where the last GPRINT ended.
static void do_gprint(void) {
    need_graphics();
    value_t v = expr();
    double x = gp_x, y = gp_y;
    if (!v.is_str && accept(",")) {
        x = v.n;
        y = num_expr();
        expect(",");
        v = expr();
    }
    char text[256];
    int len = 0;
    for (;;) {
        char num_buf[40];
        const char *s;
        int n;
        if (v.is_str) {
            s = v.s;
            n = slen(s);
        } else {
            format_number(v.n, num_buf, sizeof num_buf);
            s = num_buf[0] == ' ' ? num_buf + 1 : num_buf;
            n = (int)strlen(s);
        }
        if (len + n > (int)sizeof text)
            n = (int)sizeof text - len;
        memcpy(text + len, s, n);
        len += n;
        if (!accept(";"))
            break;
        skip_spaces();
        if (at_end_of_statement() || *p == ',')
            break;
        v = expr();
    }
    int scale = 1, rgb = cur_font ? -1 : colour_rgb(gfg);
    if (accept(",")) {
        scale = arg_int();
        if (accept(",")) {
            int c = arg_int();
            rgb = c == -2 && cur_font ? -2 : c < 0 ? (cur_font ? -1 : colour_rgb(gfg)) : colour_rgb(c);
        }
    }
    if (scale < 1 || scale > 16)
        error(E_ILLEGAL_CALL);
    int px = px_of(x), py = py_of(y);
    for (int i = 0; i < len; ++i)
        px += draw_char(cur_font, (unsigned char)text[i], px, py, scale, rgb);
    gp_x = world_x(px);
    gp_y = y;
}

/// IMAGE "file" [, x, y [, w, h [, fit]]]: a BMP, PNG or JPEG picture in
/// the current screen's coordinates. With no position it fills the screen
/// (keeping its shape); with x, y only it is drawn at its own size (one of
/// its pixels per screen pixel of the mode); with w, h it is stretched to that
/// box, or with fit <> 0 made as large as fits in it without stretching.
static void do_image(void) {
    char name[64];
    file_name_arg(name, sizeof name, false);
    // Text mode: plain screen pixels.
    int sw = screen_mode ? gw : FB_W, sh = screen_mode ? gh : FB_H;
    int sx = screen_mode ? gsx : 1, sy = screen_mode ? gsy : 1, yoff = screen_mode ? gyoff : 0;
    int x = 0, y = 0, w = sw, h = sh;
    bool fit = true;
    if (accept(",")) {
        x = arg_int();
        expect(",");
        y = arg_int();
        w = h = 0;  // own size
        fit = false;
        if (accept(",")) {
            w = arg_int();
            expect(",");
            h = arg_int();
            if (w <= 0 || h <= 0)
                error(E_ILLEGAL_CALL);
            if (accept(","))
                fit = num_expr() != 0;
        }
    }
    out_flush();
    const char *why = port_image(name, x * sx, y * sy + yoff, w * sx, h * sy, sx, sy, fit, 0,
                                 yoff, sw * sx, yoff + sh * sy);
    if (why) {
        printf("%s: %s\n", name, why);
        error(!strcmp(why, "file not found") ? E_FILE_NOT_FOUND : E_ILLEGAL_CALL);
    }
}

/// SAVEIMAGE "file": the whole screen as an 8-bit BMP (.BMP added if the name
/// has no extension).
static void do_saveimage(void) {
    char name[64];
    file_name_arg(name, sizeof name, false);
    if (!strchr(name, '.') && strlen(name) + 4 < sizeof name)
        strcat(name, ".BMP");
    const char *why = port_save_image(name);
    if (why) {
        out_flush();
        printf("%s: %s\n", name, why);
        error(E_DEVICE_IO);
    }
}

/// PAUSE ms   (Ctrl-C still stops the program)
static void do_pause(void) {
    double ms = num_expr();
    if (ms < 0)
        error(E_ILLEGAL_CALL);
    out_flush();
    double end = port_seconds() + ms / 1000;
    while (port_seconds() < end) {
        check_break();
        double left = (end - port_seconds()) * 1000;
        port_sleep_ms(left > 10 ? 10 : left > 0 ? (unsigned)left : 0);
    }
}

/// SERVO pin, angle   (0-180 degrees: a 0.5-2.5 ms pulse at 50 Hz)
static void do_servo(void) {
    int pin = pin_arg();
    expect(",");
    double angle = num_expr();
    if (angle < 0 || angle > 180)
        error(E_ILLEGAL_CALL);
    out_flush();
    port_pwm(pin, 50, (0.5 + angle / 90) / 20 * 100);
}


// ---- PLAY ----------------------------------------------------------------------
//
// GW-BASIC's music macro language. The settings last from one PLAY to the
// next (RUN resets them).

static int play_octave = 4, play_length = 4, play_tempo = 120;
static int play_on = 7;  ///< eighths of each note that sound: MN 7, ML 8, MS 6
static bool play_background;

static void reset_play(void) {
    play_octave = 4;
    play_length = 4;
    play_tempo = 120;
    play_on = 7;
    play_background = false;
}

static void queue_sound(unsigned freq, unsigned ms) {
    while (!port_play(freq, ms)) {  // the queue is full: wait for room
        check_break();
        port_sleep_ms(5);
    }
}

/// Note n (0 = the C of octave 0; middle C is 36) for the given length.
static void play_tone(int n, double ms) {
    if (n < 0 || n > 83)
        error(E_ILLEGAL_CALL);
    unsigned freq = (unsigned)lround(261.6256 * pow(2, (n - 36) / 12.0));
    unsigned on = (unsigned)(ms * play_on / 8);
    queue_sound(freq, on);
    if ((unsigned)ms > on)
        queue_sound(0, (unsigned)ms - on);
}

/// A number in the macro string at *i ("=variable;" too), or -1 if there
/// isn't one.
static int play_number(const char *m, int len, int *i) {
    if (*i < len && m[*i] == '=') {
        char name[NAME_MAX + 2];
        int n = 0;
        for (++*i; *i < len && m[*i] != ';' && n < NAME_MAX + 1; ++*i)
            name[n++] = m[*i];
        name[n] = 0;
        if (*i < len)
            ++*i;
        double v = named_number(name);
        if (v < 0 || v > 32767)
            error(E_ILLEGAL_CALL);
        return (int)lround(v);
    }
    if (*i >= len || !isdigit((unsigned char)m[*i]))
        return -1;
    int n = 0;
    while (*i < len && isdigit((unsigned char)m[*i]))
        n = n * 10 + (m[(*i)++] - '0');
    return n;
}

/// A length (1-64, or the L value) and any dots, as milliseconds.
static double play_duration(const char *m, int len, int *i) {
    int l = play_number(m, len, i);
    if (l == -1)
        l = play_length;
    if (l < 1 || l > 64)
        error(E_ILLEGAL_CALL);
    double ms = 240000.0 / (play_tempo * l), add = ms;
    while (*i < len && m[*i] == '.') {
        add /= 2;
        ms += add;
        ++*i;
    }
    return ms;
}

/// SOUND freq, ticks (18.2 a second). As in GW-BASIC: after PLAY "MB" it goes
/// to the background music queue; freq 32767 is silence; ticks 0 stops the
/// sound playing.
static void do_sound(void) {
    int freq = arg_int();
    expect(",");
    double ticks = num_expr();
    if (freq < 37 || freq > 32767 || ticks < 0 || ticks > 65535)
        error(E_ILLEGAL_CALL);
    if (ticks == 0) {
        port_play_stop();
        port_beep(0, 0);
        return;
    }
    unsigned ms = (unsigned)(ticks * 1000 / 18.2);
    unsigned f = freq == 32767 ? 0 : freq;
    if (!ms)
        return;
    if (play_background) {
        queue_sound(f, ms);
        return;
    }
    if (f)
        port_beep(f, ms);
    for (unsigned done = 0; done < ms; done += 10) {  // Ctrl-C works during a long one
        port_sleep_ms(ms - done < 10 ? ms - done : 10);
        check_break();
    }
}

static void do_play(void) {
    const char *str = str_expr();
    int len = slen(str);
    char m[256];
    memcpy(m, str, len);
    static const int note_index[7] = {9, 11, 0, 2, 4, 5, 7};  // A-G
    for (int i = 0; i < len;) {
        char c = toupper((unsigned char)m[i++]);
        int n;
        switch (c) {
        case ' ':
        case ';':
            break;
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G':
            n = play_octave * 12 + note_index[c - 'A'];
            if (i < len && (m[i] == '#' || m[i] == '+')) {
                ++n;
                ++i;
            } else if (i < len && m[i] == '-') {
                --n;
                ++i;
            }
            play_tone(n, play_duration(m, len, &i));
            break;
        case 'N':
            if ((n = play_number(m, len, &i)) < 0 || n > 84)
                error(E_ILLEGAL_CALL);
            if (n == 0)
                queue_sound(0, (unsigned)(240000.0 / (play_tempo * play_length)));
            else
                play_tone(n - 1, 240000.0 / (play_tempo * play_length));
            break;
        case 'O':
            if ((n = play_number(m, len, &i)) < 0 || n > 6)
                error(E_ILLEGAL_CALL);
            play_octave = n;
            break;
        case '>':
            if (play_octave < 6)
                ++play_octave;
            break;
        case '<':
            if (play_octave > 0)
                --play_octave;
            break;
        case 'L':
            if ((n = play_number(m, len, &i)) < 1 || n > 64)
                error(E_ILLEGAL_CALL);
            play_length = n;
            break;
        case 'T':
            if ((n = play_number(m, len, &i)) < 32 || n > 255)
                error(E_ILLEGAL_CALL);
            play_tempo = n;
            break;
        case 'P':
            queue_sound(0, (unsigned)play_duration(m, len, &i));
            break;
        case 'M':
            c = i < len ? toupper((unsigned char)m[i++]) : 0;
            if (c == 'F' || c == 'B')
                play_background = c == 'B';
            else if (c == 'N' || c == 'L' || c == 'S')
                play_on = c == 'N' ? 7 : c == 'L' ? 8 : 6;
            else
                error(E_ILLEGAL_CALL);
            break;
        default:  // includes X (it needs VARPTR$)
            error(E_ILLEGAL_CALL);
        }
    }
    // Music foreground (the default) waits for the notes to finish.
    while (!play_background && port_play_pending()) {
        if (break_pending || (read_keys(running), break_pending))
            port_play_stop();
        check_break();
        port_sleep_ms(5);
    }
}

static void do_randomize(void) {
    double seed;
    if (at_end_of_statement())
        seed = port_seconds() * 1000;
    else
        seed = num_expr();
    rnd_state = (uint32_t)(int64_t)(seed * 7919) ^ 0x9e3779b9u;
    if (!rnd_state)
        rnd_state = 1;
}

// ---- Program-level commands -----------------------------------------------

/// Parses "[from][-[to]]" and returns the first and last line index (last < first if none).
static void line_range(int *first, int *last) {
    int lo = 0, hi = 65535;
    skip_spaces();
    if (isdigit((unsigned char)*p))
        lo = hi = read_line_number();
    else if (*p == '.')
        error(E_SYNTAX);
    if (accept("-")) {
        hi = 65535;
        skip_spaces();
        if (isdigit((unsigned char)*p))
            hi = read_line_number();
    }
    *first = find_line(lo, NULL);
    *last = find_line(hi + 1, NULL) - 1;
}

static bool more(void) {
    out_str("-- more --");
    out_flush();
    int c = key_wait();
    out_str("\r          \r");
    return c != 3 && c != 'q' && c != 'Q' && c != 0x1b;
}

static void do_list(void) {
    int first, last;
    line_range(&first, &last);
    int shown = 0;
    for (int i = first; i <= last; ++i) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u ", lines[i].num);
        out_str(buf);
        out_str(lines[i].text);
        out_newline();
        poll_keys();
        if (break_pending) {
            break_pending = false;
            break;
        }
        if (++shown == PAGE_LINES && i < last) {
            shown = 0;
            if (!more())
                break;
        }
    }
    out_flush();
}

static void do_delete(void) {
    skip_spaces();
    if (at_end_of_statement())
        error(E_ILLEGAL_CALL);
    int first, last;
    line_range(&first, &last);
    if (last < first)
        error(E_ILLEGAL_CALL);
    for (int i = last; i >= first; --i)
        delete_line_at(i);
    cont_pos.line = -2;
}

/// RENUM [new][,[old][,step]]: renumbers and fixes GOTO, GOSUB, THEN, ELSE,
/// RESTORE, RUN and ON ... GOTO/GOSUB references.
static int renum_map(int old, int from_index, int start, int step) {
    bool exact;
    int i = find_line(old, &exact);
    if (!exact || i < from_index)
        return old;
    return start + (i - from_index) * step;
}

static void do_renum(void) {
    int start = 10, old = 0, step = 10;
    skip_spaces();
    if (isdigit((unsigned char)*p))
        start = read_line_number();
    if (accept(",")) {
        skip_spaces();
        if (isdigit((unsigned char)*p))
            old = read_line_number();
        if (accept(","))
            step = read_line_number();
    }
    if (step <= 0)
        error(E_ILLEGAL_CALL);
    int from = find_line(old, NULL);
    if (from > 0 && lines[from - 1].num >= start)
        error(E_ILLEGAL_CALL);
    if (start + (long)(line_count - from - 1) * step > 65529)
        error(E_ILLEGAL_CALL);
    // Rewrite references first, while the old numbers still resolve.
    static const char *const refs[] = {"GOTO", "GOSUB", "THEN", "ELSE", "RESTORE", "RUN"};
    for (int i = 0; i < line_count; ++i) {
        char out[LINE_MAX_LEN * 2];
        int n = 0;
        const char *s = lines[i].text;
        bool in_str = false, changed = false;
        while (*s && n < (int)sizeof out - 8) {
            if (*s == '"')
                in_str = !in_str;
            bool ref = false;
            if (!in_str) {
                if (*s == '\'' || !strncmp(s, "REM", 3)) {
                    while (*s && n < (int)sizeof out - 1)
                        out[n++] = *s++;
                    break;
                }
                for (size_t k = 0; k < sizeof refs / sizeof *refs; ++k) {
                    size_t l = strlen(refs[k]);
                    if (!strncmp(s, refs[k], l)) {
                        memcpy(out + n, s, l);
                        n += l;
                        s += l;
                        ref = true;
                        break;
                    }
                }
            }
            if (!ref) {
                out[n++] = *s++;
                continue;
            }
            // One number, or a comma list after ON ... GOTO/GOSUB.
            for (;;) {
                while (*s == ' ')
                    out[n++] = *s++;
                if (!isdigit((unsigned char)*s))
                    break;
                long v = strtol(s, (char **)&s, 10);
                n += snprintf(out + n, sizeof out - n, "%d", renum_map(v, from, start, step));
                changed = true;
                const char *t = s;
                while (*t == ' ')
                    ++t;
                if (*t != ',')
                    break;
                while (s <= t)
                    out[n++] = *s++;
            }
        }
        out[n] = 0;
        if (changed && strcmp(out, lines[i].text)) {
            if (n > LINE_MAX_LEN)
                error(E_OUT_OF_MEMORY);
            char *copy = checked_malloc(n + 1);
            memcpy(copy, out, n + 1);
            free(lines[i].text);
            lines[i].text = copy;
        }
    }
    for (int i = from; i < line_count; ++i)
        lines[i].num = start + (i - from) * step;
    cont_pos.line = -2;
}

/// '*' and '?' wildcards (both upper case).
static bool wild_match(const char *w, const char *s) {
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*w == '?' || *w == *s) {
            ++s;
            ++w;
        } else if (*w == '*') {
            star = w++;
            mark = s;
        } else if (star) {
            w = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (*w == '*')
        ++w;
    return !*w;
}

/// DIR$(pattern$[, n]): the n-th (1 first) name matching the pattern ("*.PNG",
/// "/FONTS/*.*") in that folder, folders included; "" past the last.
static value_t dir_function(void) {
    char pattern[96];
    snprintf(pattern, sizeof pattern, "%s", str_expr());
    int n = 1;
    if (accept(","))
        n = arg_int();
    expect(")");
    char folder[96] = "";
    char *slash = strrchr(pattern, '/');
    const char *wild = pattern;
    if (slash) {
        snprintf(folder, sizeof folder, "%.*s", (int)(slash - pattern), pattern);
        if (!folder[0])
            strcpy(folder, "/");
        wild = slash + 1;
    }
    char want[96];
    snprintf(want, sizeof want, "%s", *wild ? wild : "*");
    for (char *c = want; *c; ++c)
        *c = toupper((unsigned char)*c);
    DIR dir;
    FILINFO info;
    if (n < 1 || f_opendir(&dir, folder) != FR_OK)
        return str("", 0);
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & (AM_HID | AM_SYS))
            continue;
        char upper[sizeof info.fname];
        snprintf(upper, sizeof upper, "%s", info.fname);
        for (char *c = upper; *c; ++c)
            *c = toupper((unsigned char)*c);
        if (wild_match(want, upper) && --n == 0) {
            f_closedir(&dir);
            return str(info.fname, (int)strlen(info.fname));
        }
    }
    f_closedir(&dir);
    return str("", 0);
}

static void do_files(void) {
    char pattern[64] = "*";
    skip_spaces();
    if (!at_end_of_statement())
        snprintf(pattern, sizeof pattern, "%s", str_expr());
    for (char *c = pattern; *c; ++c)
        *c = toupper((unsigned char)*c);
    DIR dir;
    FILINFO info;
    FRESULT r = f_opendir(&dir, "");
    if (r != FR_OK)
        error(fresult_error(r));
    int col = 0, shown = 0;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & (AM_HID | AM_SYS))
            continue;
        // Simple wildcards: '*' and '?'.
        char upper[sizeof info.fname];
        snprintf(upper, sizeof upper, "%s", info.fname);
        for (char *c = upper; *c; ++c)
            *c = toupper((unsigned char)*c);
        if (!wild_match(pattern, upper))
            continue;
        char entry[40];
        snprintf(entry, sizeof entry, "%-.14s%s", info.fname, info.fattrib & AM_DIR ? "<DIR>" : "");
        out_str(entry);
        ++shown;
        if (++col == 4) {
            col = 0;
            out_newline();
        } else {
            for (int k = strlen(entry); k < 20; ++k)
                out_char(' ');
        }
    }
    f_closedir(&dir);
    if (col)
        out_newline();
    if (!shown) {
        out_str("File not found");
        out_newline();
    }
    out_flush();
}

// ---- Data files: OPEN, CLOSE, PRINT #, WRITE, INPUT #, FIELD, GET, PUT -----


static void close_file(int n) {
    bfile_t *b = files[n];
    if (!b)
        return;
    files[n] = NULL;
    if (b->mode == FM_COM)
        port_serial_close();
    else
        f_close(&b->f);
    free(b->buf);
    free(b);
}

static void close_all_files(void) {
    for (int n = 1; n <= MAX_FILES; ++n)
        close_file(n);
}

/// FIELD points variables into record buffers; when variables are cleared
/// those links go too.
static void forget_fields(void) {
    for (int n = 1; n <= MAX_FILES; ++n)
        if (files[n])
            files[n]->nfields = 0;
}

static int file_number(void) {
    accept("#");
    int n = arg_int();
    if (n < 1 || n > MAX_FILES)
        error(E_BAD_FILE_NUMBER);
    return n;
}

static bfile_t *open_file_arg(void) {
    int n = file_number();
    if (!files[n])
        error(E_BAD_FILE_NUMBER);
    return files[n];
}

/// The keyword `kw` as a whole word (so a variable such as NAME$ or OPENED
/// isn't taken for the statement).
static bool accept_word(const char *kw) {
    const char *save = p;
    if (!accept(kw))
        return false;
    if (is_ident_char((unsigned char)*p) || *p == '$') {
        p = save;
        return false;
    }
    return true;
}

/// OPEN "COM1:[speed][,parity][,data][,stop][,options]" AS #n opens the
/// serial port, UART1. As in GW-BASIC the defaults are 300 baud, even parity,
/// 7 data bits and 1 stop bit; options are LF (send LF after CR), RS, CS, DS,
/// CD, PE, BIN and ASC (accepted, no effect: there are no handshake lines),
/// and TXn / RXn to choose the pins (default TX GP4, RX GP5).
static void open_com(int n, const char *spec) {
    if (spec[3] != '1')
        error(E_BAD_FILE_NAME);  // COM0/UART0 is the debug console
    for (int i = 1; i <= MAX_FILES; ++i)
        if (files[i] && files[i]->mode == FM_COM)
            error(E_FILE_ALREADY_OPEN);
    unsigned baud = 300;
    int data = 7, stop = 1, tx = 4, rx = 5;
    char parity = 'E';
    bool lf = false;
    const char *q = spec + 5;
    for (int field = 0; *q || field == 0; ++field) {
        char item[16];
        int len = 0;
        while (*q == ' ')
            ++q;
        while (*q && *q != ',' && len < (int)sizeof item - 1)
            item[len++] = toupper((unsigned char)*q++);
        while (len && item[len - 1] == ' ')
            --len;
        item[len] = 0;
        if (*q == ',')
            ++q;
        else if (*q)
            error(E_BAD_FILE_NAME);
        if (!len) {
            if (!*q)
                break;
            continue;  // an empty field keeps the default
        }
        if (field == 0) {
            baud = atoi(item);
            if (baud < 75 || baud > 921600)
                error(E_BAD_FILE_NAME);
        } else if (field == 1 && len == 1 && strchr("NEO", item[0])) {
            parity = item[0];
        } else if (field == 2 && len == 1 && item[0] >= '5' && item[0] <= '8') {
            data = item[0] - '0';
        } else if (field == 3 && len == 1 && (item[0] == '1' || item[0] == '2')) {
            stop = item[0] - '0';
        } else if (!strcmp(item, "LF")) {
            lf = true;
        } else if ((item[0] == 'T' || item[0] == 'R') && item[1] == 'X' && isdigit((unsigned char)item[2])) {
            *(item[0] == 'T' ? &tx : &rx) = atoi(item + 2);
        } else if (!strncmp(item, "RS", 2) || !strncmp(item, "CS", 2) || !strncmp(item, "DS", 2) ||
                   !strncmp(item, "CD", 2) || !strcmp(item, "PE") || !strcmp(item, "BIN") ||
                   !strcmp(item, "ASC")) {
            // handshake and mode options: no such lines here
        } else {
            error(E_BAD_FILE_NAME);
        }
        if (!*q)
            break;
    }
    const char *why = port_serial_open(baud, data, parity, stop, tx, rx);
    if (why) {
        out_flush();
        printf("COM1: %s\n", why);
        error(E_DEVICE_UNAVAILABLE);
    }
    bfile_t *b = calloc(1, sizeof *b);
    if (!b) {
        port_serial_close();
        error(E_OUT_OF_MEMORY);
    }
    b->mode = FM_COM;
    b->peek = -2;
    b->com_lf = lf;
    snprintf(b->name, sizeof b->name, "COM1:");
    files[n] = b;
}

/// OPEN "name" [FOR INPUT|OUTPUT|APPEND|RANDOM] AS [#]n [LEN=reclen]
/// OPEN "I"|"O"|"A"|"R", [#]n, "name" [, reclen]
static void do_open(void) {
    char first[64], name[64];
    snprintf(first, sizeof first, "%s", str_expr());
    int mode, n, reclen = 128;
    if (accept(",")) {
        switch (toupper((unsigned char)first[0])) {
        case 'I': mode = FM_INPUT; break;
        case 'O': mode = FM_OUTPUT; break;
        case 'A': mode = FM_APPEND; break;
        case 'R': mode = FM_RANDOM; break;
        default: error(E_BAD_FILE_MODE);
        }
        n = file_number();
        expect(",");
        file_name_arg(name, sizeof name, false);
        if (accept(","))
            reclen = arg_int();
    } else {
        if (!*first)
            error(E_BAD_FILE_NAME);
        snprintf(name, sizeof name, "%s", first);
        mode = FM_RANDOM;
        if (accept("FOR")) {
            if (accept("INPUT"))
                mode = FM_INPUT;
            else if (accept("OUTPUT"))
                mode = FM_OUTPUT;
            else if (accept("APPEND"))
                mode = FM_APPEND;
            else if (accept("RANDOM"))
                mode = FM_RANDOM;
            else
                error(E_SYNTAX);
        }
        expect("AS");
        n = file_number();
        if (accept("LEN")) {
            expect("=");
            reclen = arg_int();
        }
    }
    if (files[n])
        error(E_FILE_ALREADY_OPEN);
    if (!strncasecmp(name, "COM", 3) && isdigit((unsigned char)name[3]) && name[4] == ':') {
        open_com(n, name);
        return;
    }
    if (reclen < 1 || reclen > MAX_RECLEN)
        error(E_ILLEGAL_CALL);
    // A file may be read through several numbers, but not written while
    // open elsewhere.
    for (int i = 1; i <= MAX_FILES; ++i)
        if (files[i] && !strcasecmp(files[i]->name, name) &&
            (mode != FM_INPUT || files[i]->mode != FM_INPUT))
            error(E_FILE_ALREADY_OPEN);
    bfile_t *b = checked_malloc(sizeof *b);
    memset(b, 0, sizeof *b);
    b->mode = mode;
    b->peek = -2;
    snprintf(b->name, sizeof b->name, "%s", name);
    BYTE how = mode == FM_INPUT    ? FA_READ
               : mode == FM_OUTPUT ? FA_WRITE | FA_CREATE_ALWAYS
               : mode == FM_APPEND ? FA_WRITE | FA_OPEN_APPEND
                                   : FA_READ | FA_WRITE | FA_OPEN_ALWAYS;
    if (mode == FM_RANDOM) {
        b->reclen = reclen;
        b->buf = malloc(reclen);
        if (!b->buf) {
            free(b);
            error(E_OUT_OF_MEMORY);
        }
        memset(b->buf, ' ', reclen);
    }
    FRESULT r = f_open(&b->f, name, how);
    if (r != FR_OK) {
        free(b->buf);
        free(b);
        error(fresult_error(r));
    }
    files[n] = b;
}

/// CLOSE [[#]n [, [#]n ...]]
static void do_close(void) {
    if (at_end_of_statement()) {
        close_all_files();
        return;
    }
    do
        close_file(file_number());
    while (accept(","));
}

static int file_getc(bfile_t *b) {
    if (b->peek != -2) {
        int c = b->peek;
        b->peek = -2;
        return c;
    }
    if (b->mode == FM_COM) {  // wait for a byte; Ctrl-C still stops
        int c;
        while ((c = port_serial_getc()) < 0) {
            check_break();
            port_sleep_ms(1);
        }
        return c;
    }
    uint8_t ch;
    UINT n;
    if (f_read(&b->f, &ch, 1, &n) != FR_OK)
        error(E_DEVICE_IO);
    return n && ch != 0x1a ? ch : -1;  // Ctrl-Z also ends a text file
}

/// The next character without taking it; -1 at the end (or, for COM1,
/// when nothing has arrived: a look ahead never waits for the line).
static int file_peek(bfile_t *b) {
    if (b->peek == -2) {
        if (b->mode == FM_COM) {
            int c = port_serial_getc();
            if (c < 0)
                return -1;
            b->peek = c;
        } else {
            b->peek = file_getc(b);
        }
    }
    return b->peek;
}

static bfile_t *reading_file(void) {
    bfile_t *b = open_file_arg();
    if (b->mode != FM_INPUT && b->mode != FM_COM)
        error(E_BAD_FILE_MODE);
    return b;
}

static bfile_t *writing_file(void) {
    bfile_t *b = open_file_arg();
    if (b->mode != FM_OUTPUT && b->mode != FM_APPEND && b->mode != FM_COM)
        error(E_BAD_FILE_MODE);
    return b;
}

/// Starts sending out_char's output to the file `b`.
static void begin_file_output(bfile_t *b) {
    screen_col = out_col;
    out_col = b->col;
    out_dev = b;
}

static void end_file_output(bfile_t *b) {
    b->col = out_col;
    out_col = screen_col;
    out_dev = NULL;
}

/// One item for INPUT #: a quoted string, or text up to a comma or the end
/// of the line (for numbers, also up to a space).
static void read_item(bfile_t *b, bool is_str, char *out) {
    int c;
    do
        c = file_getc(b);
    while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (c < 0)
        error(E_INPUT_PAST_END);
    int n = 0;
    if (is_str && c == '"') {
        while ((c = file_getc(b)) >= 0 && c != '"' && c != '\n')
            if (n < STR_MAX)
                out[n++] = c;
        if (c == '"')
            while ((c = file_getc(b)) >= 0 && c != ',' && c != '\n' && c != '\r')
                ;
        if (c == '\r' && file_peek(b) == '\n')
            file_getc(b);
    } else {
        while (c >= 0 && c != ',' && c != '\n' && c != '\r' && (is_str || c != ' ')) {
            if (n < STR_MAX)
                out[n++] = c;
            c = file_getc(b);
        }
        if (!is_str && c == ' ') {  // skip to the separator
            while (file_peek(b) == ' ')
                file_getc(b);
            if (file_peek(b) == ',')
                file_getc(b);
            else if (file_peek(b) == '\r')
                c = file_getc(b);
        }
        // Take the whole line ending, so EOF is true after the last item.
        if (c == '\r' && file_peek(b) == '\n')
            file_getc(b);
        while (n && out[n - 1] == ' ')
            --n;
    }
    out[n] = 0;
}

/// INPUT #n, var [, var ...]
static void do_input_file(void) {
    bfile_t *b = reading_file();
    expect(",");
    do {
        lvalue_t lv = lvalue();
        char item[STR_MAX + 1];
        read_item(b, lv.is_str, item);
        if (lv.is_str)
            set_strn(lv.s, item, strlen(item));
        else
            set_num(lv, strtod(item, NULL));
    } while (accept(","));
}

/// LINE INPUT #n, var$
static void do_line_input_file(void) {
    bfile_t *b = reading_file();
    expect(",");
    lvalue_t lv = lvalue();
    if (!lv.is_str)
        error(E_TYPE_MISMATCH);
    char line[STR_MAX + 1];
    int n = 0, c = file_getc(b);
    if (b->mode == FM_COM && c == '\n')  // the LF of a CR LF that ended the last line
        c = file_getc(b);
    if (c < 0)
        error(E_INPUT_PAST_END);
    // A line ends at LF (CR LF in files); from COM1 a CR alone ends it too.
    for (; c >= 0 && c != '\n' && !(c == '\r' && b->mode == FM_COM); c = file_getc(b))
        if (c != '\r' && n < STR_MAX)
            line[n++] = c;
    line[n] = 0;
    set_strn(lv.s, line, n);
}

/// WRITE [#n,] expr [, expr ...]: strings in quotes, items separated by
/// commas, so INPUT # reads them back exactly.
static void do_write(void) {
    bfile_t *b = NULL;
    skip_spaces();
    if (*p == '#') {
        b = writing_file();
        expect(",");
        begin_file_output(b);
    }
    bool first = true;
    while (!at_end_of_statement()) {
        if (!first)
            expect(",");
        if (!first)
            out_char(',');
        first = false;
        value_t v = expr();
        if (v.is_str) {
            out_char('"');
            out_bytes(v.s, slen(v.s));
            out_char('"');
        } else {
            char buf[40];
            format_number(v.n, buf, sizeof buf);
            out_str(buf[0] == ' ' ? buf + 1 : buf);
        }
    }
    out_newline();
    if (b)
        end_file_output(b);
    out_flush();
}

/// FIELD #n, width AS var$ [, width AS var$ ...]
static void do_field(void) {
    bfile_t *b = open_file_arg();
    if (b->mode != FM_RANDOM)
        error(E_BAD_FILE_MODE);
    b->nfields = 0;
    int off = 0;
    while (accept(",")) {
        int len = arg_int();
        expect("AS");
        lvalue_t lv = lvalue();
        if (!lv.is_str)
            error(E_TYPE_MISMATCH);
        if (len < 0 || off + len > b->reclen)
            error(E_FIELD_OVERFLOW);
        if (b->nfields == MAX_FIELDS)
            error(E_ILLEGAL_CALL);
        b->fields[b->nfields].slot = lv.s;
        b->fields[b->nfields].off = off;
        b->fields[b->nfields].len = len;
        ++b->nfields;
        set_strn(lv.s, b->buf + off, len);
        off += len;
    }
}

/// Copies the record buffer into the FIELD variables.
static void fields_from_buffer(bfile_t *b) {
    for (int i = 0; i < b->nfields; ++i)
        set_strn(b->fields[i].slot, b->buf + b->fields[i].off, b->fields[i].len);
}

/// LSET / RSET var$ = expr: pads or cuts to the variable's width (its FIELD
/// width, or else its current length), left- or right-aligned.
static void do_lrset(bool right) {
    lvalue_t lv = lvalue();
    if (!lv.is_str)
        error(E_TYPE_MISMATCH);
    expect("=");
    const char *v = str_expr();
    int vlen = slen(v);
    char *dest = NULL;
    int width = -1;
    for (int n = 1; n <= MAX_FILES && width < 0; ++n)
        for (int i = 0; files[n] && i < files[n]->nfields; ++i)
            if (files[n]->fields[i].slot == lv.s) {
                width = files[n]->fields[i].len;
                dest = files[n]->buf + files[n]->fields[i].off;
                break;
            }
    if (width < 0)
        width = *lv.s ? slen(*lv.s) : 0;
    char t[MAX_RECLEN + 1];
    int len = vlen;
    if (len > width)
        len = width;
    memset(t, ' ', width);
    memcpy(t + (right ? width - len : 0), v, len);
    t[width] = 0;
    if (dest)
        memcpy(dest, t, width);
    set_strn(lv.s, t, width);
}

/// GET / PUT #n [, record]
static void do_getput(bool put) {
    bfile_t *b = open_file_arg();
    if (b->mode != FM_RANDOM)
        error(E_BAD_FILE_MODE);
    long rec = b->record + 1;
    if (accept(",")) {
        double d = num_expr();
        if (d < 1 || d > 16777215)
            error(E_BAD_RECORD_NUMBER);
        rec = (long)d;
    }
    FRESULT r = f_lseek(&b->f, (FSIZE_t)(rec - 1) * b->reclen);
    if (r != FR_OK)
        error(fresult_error(r));
    UINT n;
    if (put) {
        r = f_write(&b->f, b->buf, b->reclen, &n);
        if (r != FR_OK)
            error(fresult_error(r));
        if (n != b->reclen)
            error(E_DISK_FULL);
    } else {
        r = f_read(&b->f, b->buf, b->reclen, &n);
        if (r != FR_OK)
            error(fresult_error(r));
        b->past_end = n == 0;
        memset(b->buf + n, ' ', b->reclen - n);
        fields_from_buffer(b);
    }
    b->record = rec;
}

/// NAME "old" AS "new"
static void do_name(void) {
    char from[64], to[64];
    file_name_arg(from, sizeof from, false);
    expect("AS");
    file_name_arg(to, sizeof to, false);
    FRESULT r = f_rename(from, to);
    if (r != FR_OK)
        error(r == FR_EXIST ? E_FILE_EXISTS : fresult_error(r));
}

static int free_file_number(void) {
    for (int n = 1; n <= MAX_FILES; ++n)
        if (!files[n])
            return n;
    error(E_TOO_MANY_FILES);
    return 0;
}

static void do_kill(void) {
    char name[64];
    file_name_arg(name, sizeof name, false);
    FRESULT r = f_unlink(name);
    if (r != FR_OK)
        error(fresult_error(r));
}

static void run_program(int start_line);

static void do_run(void) {
    port_music_stop();
    skip_spaces();
    if (*p == '"') {
        char name[64];
        file_name_arg(name, sizeof name, true);
        load_file(name, 0);
        run_program(-1);
        return;
    }
    int start = -1;
    if (isdigit((unsigned char)*p))
        start = read_line_number();
    run_program(start);
}

/// COMMON names: the variables CHAIN passes on ("A()" for an array).
#define MAX_COMMON 64
static char (*common_names)[NAME_MAX + 4];  ///< MAX_COMMON, while BASIC runs
static int common_count;
static bool chain_all;

static bool keep_for_chain(const char *name, bool array) {
    if (chain_all)
        return true;
    char want[NAME_MAX + 4];
    snprintf(want, sizeof want, "%s%s", name, array ? "()" : "");
    for (int i = 0; i < common_count; ++i)
        if (!strcmp(common_names[i], want))
            return true;
    return false;
}

static void do_common(void) {
    do {
        char name[NAME_MAX + 2];
        if (!read_name(name))
            error(E_SYNTAX);
        bool array = accept("(");
        if (array)
            expect(")");
        char entry[NAME_MAX + 4];
        snprintf(entry, sizeof entry, "%s%s", name, array ? "()" : "");
        bool have = false;
        for (int i = 0; i < common_count; ++i)
            have |= !strcmp(common_names[i], entry);
        if (!have) {
            if (common_count == MAX_COMMON)
                error(E_OUT_OF_MEMORY);
            strcpy(common_names[common_count++], entry);
        }
    } while (accept(","));
}

/// CHAIN [MERGE] "file" [, [line] [, [ALL] [, DELETE a-b]]]: runs another
/// program, passing on the COMMON variables (or ALL of them); open files
/// stay open. MERGE adds the file's lines to this program instead (after
/// deleting lines a-b).
static void do_chain(void) {
    bool merge = accept_word("MERGE");
    char name[64];
    file_name_arg(name, sizeof name, true);
    int start = -1, del_from = -1, del_to = -1;
    chain_all = false;
    if (accept(",")) {
        skip_spaces();
        if (*p != ',' && !at_end_of_statement())
            start = (int)lround(num_expr());
        if (accept(",")) {
            skip_spaces();
            if (accept_word("ALL"))
                chain_all = true;
            if (accept(",")) {
                if (!accept_word("DELETE"))
                    error(E_SYNTAX);
                line_range(&del_from, &del_to);
            }
        }
    }
    if (merge) {
        if (del_from >= 0)
            for (int i = del_to; i >= del_from; --i)
                delete_line_at(i);
        load_file(name, LOAD_MERGE | LOAD_KEEP_VARS);
    } else {
        if (chain_all)
            keep_fns_for_chain();
        prune_vars(keep_for_chain, chain_all);
        load_file(name, LOAD_KEEP_VARS);
    }
    common_count = 0;
    if (!line_count)
        error(E_ILLEGAL_CALL);
    if (start >= 0) {
        jump_to_line(start);
    } else {
        cur_line = 0;
        p = lines[0].text;
        running = true;
        jumped = true;
    }
}

static void do_edit(void) {
    // The program goes to a scratch file, the editor opens it, and whatever
    // is in the file afterwards becomes the program.
    const char *tmp = "~basic.tmp";
    save_file(tmp);
    char saved_name[sizeof program_name];
    strcpy(saved_name, program_name);
    port_edit(tmp);
    load_file(tmp, 0);
    f_unlink(tmp);
    strcpy(program_name, saved_name);
}

// ---- The statement loop ---------------------------------------------------

enum { S_CONTINUE, S_END, S_SYSTEM };

static int statement(void);

/// Runs statements from p until the end of the line (direct mode) or the end
/// of the program.
static int execute(void) {
    for (;;) {
        skip_spaces();
        if (*p == ':') {
            ++p;
            continue;
        }
        if (!*p || *p == '\'') {
            if (!running || cur_line < 0 || cur_line + 1 >= line_count) {
                if (running && cur_line >= 0 && in_error_handler)
                    error(E_NO_RESUME);  // ran off the end inside an error handler
                return S_END;
            }
            ++cur_line;
            p = lines[cur_line].text;
            if (trace) {
                char buf[16];
                snprintf(buf, sizeof buf, "[%u]", lines[cur_line].num);
                out_str(buf);
            }
            continue;
        }
        if (!strncmp(p, "ELSE", 4)) {
            skip_line();  // reached after a true IF's THEN part
            continue;
        }
        int r = statement();
        free_temps();
        if (r != S_CONTINUE)
            return r;
        // Reading the keyboard costs more than a simple statement, so the
        // loop looks every 32 statements; typed keys wait in the console's
        // buffer meanwhile (INKEY$ and INPUT read it directly).
        static uint8_t key_check;
        if (++key_check % 32 == 0 || break_pending)
            check_break();
        check_events();
    }
}

static void run_program(int start_line) {
    common_count = 0;
    reset_play();
    close_all_files();
    port_pins_reset();
    clear_vars();
    reset_run_state();
    rnd_state = 0x12345678u;
    if (!line_count)
        return;
    if (start_line >= 0) {
        jump_to_line(start_line);
    } else {
        cur_line = 0;
        p = lines[0].text;
    }
    running = true;
    jumped = true;
}

static int statement(void) {
    skip_spaces();
    jumped = false;
    const char *start = p;
    stmt_pos = (pos_t){cur_line, start};
    if (!may_be_keyword(&statement_set, p)) {
        // Implicit LET (the else at the end), without trying every keyword.
        lvalue_t lv = lvalue();
        expect("=");
        assign(lv, expr());
    } else if (accept("PRINT") || accept("?") || accept("LPRINT")) {  // no printer: LPRINT is PRINT
        do_print();
    } else if (accept("LET")) {
        lvalue_t lv = lvalue();
        expect("=");
        assign(lv, expr());
    } else if (accept("IF")) {
        do_if();
    } else if (accept("GOTO")) {
        do_goto_or_gosub(false);
    } else if (accept("GOSUB")) {
        do_goto_or_gosub(true);
    } else if (accept_word("RESUME")) {
        do_resume();
    } else if (accept_word("ERROR")) {
        int n = arg_int();
        if (n < 1 || n > 255)
            error(E_ILLEGAL_CALL);
        error(n);
    } else if (accept("RETURN")) {
        if (!gosub_depth)
            error(E_RETURN_WITHOUT_GOSUB);
        gosub_frame_t *g = &gosub_stack[--gosub_depth];
        if (g->event >= 0)
            events[g->event].in_handler = false;
        for_depth = g->for_base;
        while_depth = g->while_base;
        cur_line = g->ret.line;
        p = g->ret.p;
        jumped = true;
    } else if (accept("FOR")) {
        do_for();
    } else if (accept_word("EXIT")) {
        // EXIT FOR (from QuickBASIC): leaves the innermost FOR loop.
        if (!accept_word("FOR"))
            error(E_SYNTAX);
        if (for_depth == for_base())
            error(E_NEXT_WITHOUT_FOR);
        --for_depth;
        skip_block("FOR", "NEXT", E_FOR_WITHOUT_NEXT);
    } else if (accept("NEXT")) {
        do_next();
    } else if (accept("WHILE")) {
        const char *stmt = start;
        value_t c = expr();
        bool truth = c.is_str ? c.s[0] != 0 : c.n != 0;
        if (truth) {
            if (while_depth == STACK_MAX)
                error(E_OUT_OF_MEMORY);
            while_stack[while_depth++] = (pos_t){cur_line, stmt};
        } else {
            skip_block("WHILE", "WEND", E_WHILE_WITHOUT_WEND);
        }
    } else if (accept("WEND")) {
        if (!while_depth)
            error(E_WEND_WITHOUT_WHILE);
        pos_t w = while_stack[--while_depth];
        cur_line = w.line;
        p = w.p;
        jumped = true;
    } else if (accept("ON")) {
        do_on();
    } else if (accept("INPUT")) {
        skip_spaces();
        if (*p == '#')
            do_input_file();
        else
            do_input(false);
    } else if (accept("LINE")) {
        if (accept("INPUT")) {
            skip_spaces();
            if (*p == '#')
                do_line_input_file();
            else
                do_input(true);
        } else {
            do_gline();
        }
    } else if (accept("PSET")) {
        do_pset(false);
    } else if (accept("PRESET")) {
        do_pset(true);
    } else if (accept("CIRCLE")) {
        do_circle();
    } else if (accept("PAINT")) {
        do_paint();
    } else if (accept("DRAW")) {
        do_draw();
    } else if (accept("PALETTE")) {
        do_palette();
    } else if (accept("SCREEN")) {
        do_screen();
    } else if (accept("DIM")) {
        do_dim();
    } else if (accept("READ")) {
        do_read();
    } else if (accept("DATA") || accept("REM") || accept("'")) {
        if (start[0] == 'D')
            skip_statement();
        else
            skip_line();
    } else if (accept("RESTORE")) {
        do_restore();
    } else if (accept_word("DEFINT")) {
        do_deftype('%');
    } else if (accept_word("DEFSNG")) {
        do_deftype(0);
    } else if (accept_word("DEFDBL")) {
        do_deftype('#');
    } else if (accept_word("DEFSTR")) {
        do_deftype('$');
    } else if (accept("DEF")) {
        do_def();
    } else if (accept("RANDOMIZE")) {
        do_randomize();
    } else if (accept("SWAP")) {
        do_swap();
    } else if (accept("CLS")) {
        do_cls();
    } else if (accept("LOCATE")) {
        do_locate();
    } else if (accept("COLOR")) {
        do_color();
    } else if (accept_word("OPEN")) {
        do_open();
    } else if (accept_word("CLOSE")) {
        do_close();
    } else if (accept_word("WRITE")) {
        do_write();
    } else if (accept_word("FIELD")) {
        do_field();
    } else if (accept_word("LSET")) {
        do_lrset(false);
    } else if (accept_word("RSET")) {
        do_lrset(true);
    } else if (accept_word("GET")) {
        skip_spaces();
        if (*p == '(' || !strncmp(p, "STEP", 4))
            do_gget();
        else
            do_getput(false);
    } else if (accept_word("PUT")) {
        skip_spaces();
        if (*p == '(' || !strncmp(p, "STEP", 4))
            do_gput();
        else
            do_getput(true);
    } else if (accept_word("SPRITE")) {
        do_sprite();
    } else if (accept_word("I2C")) {
        do_i2c();
    } else if (accept_word("SPI")) {
        do_spi();
    } else if (accept_word("FONT")) {
        do_font();
    } else if (accept_word("GPRINT")) {
        do_gprint();
    } else if (accept_word("VIEW")) {
        do_view();
    } else if (accept_word("WINDOW")) {
        do_window();
    } else if (accept_word("PCOPY")) {
        int from = arg_int();
        expect(",");
        int to = arg_int();
        if (from < 0 || to < 0 || from >= page_count || to >= page_count)
            error(E_ILLEGAL_CALL);
        out_flush();
        if (port_copy_page(from, to))
            error(E_OUT_OF_MEMORY);
    } else if (accept_word("NAME")) {
        do_name();
    } else if (accept("PINMODE")) {
        do_pinmode();
    } else if (accept("PINS")) {
        out_flush();
        port_pins_list();
    } else if (accept("PIN")) {
        do_pin();
    } else if (accept("PWM")) {
        do_pwm();
    } else if (accept("SERVO")) {
        do_servo();
    } else if (accept_word("IMAGE")) {
        do_image();
    } else if (accept_word("SAVEIMAGE")) {
        do_saveimage();
    } else if (accept("PAUSE")) {
        do_pause();
    } else if (accept("BEEP")) {
        port_beep(800, 250);
        port_sleep_ms(250);
    } else if (accept("SOUND")) {
        do_sound();
    } else if (accept("END")) {
        close_all_files();
        out_flush();
        cont_pos.line = -2;
        return S_END;
    } else if (accept("STOP")) {
        skip_statement();
        cont_pos = (pos_t){cur_line, p};
        error(E_BREAK);
    } else if (accept("CONT")) {
        if (cont_pos.line < -1 || cur_line >= 0)
            error(E_CANT_CONTINUE);
        cur_line = cont_pos.line;
        p = cont_pos.p;
        running = true;
        jumped = true;
    } else if (accept("RUN")) {
        do_run();
    } else if (accept("LIST") || accept("LLIST")) {  // no printer: LLIST lists on screen
        do_list();
    } else if (accept("NEW")) {
        port_music_stop();
        close_all_files();
        port_pins_reset();
        clear_program();
        clear_vars();
        reset_run_state();
        program_name[0] = 0;
        cont_pos.line = -2;
        return S_END;
    } else if (accept("CLEAR")) {
        close_all_files();
        while (!at_end_of_statement())
            ++p;  // CLEAR ,n arguments are accepted and ignored
        clear_vars();
        reset_run_state();
    } else if (accept("LOAD")) {
        char name[64];
        file_name_arg(name, sizeof name, true);
        bool run = false;
        if (accept(",")) {
            if (!accept("R"))
                error(E_SYNTAX);
            run = true;
        }
        load_file(name, 0);
        if (run)
            run_program(-1);
        else
            return S_END;
    } else if (accept_word("PLAY")) {
        skip_spaces();
        if (!strncmp(p, "ON", 2) || !strncmp(p, "OFF", 3) || !strncmp(p, "STOP", 4))
            set_event_state(EV_PLAY);
        else
            do_play();
    } else if (accept_word("MERGE")) {
        char name[64];
        file_name_arg(name, sizeof name, true);
        load_file(name, LOAD_MERGE | LOAD_KEEP_VARS);
        cont_pos.line = -2;
        return S_END;
    } else if (accept_word("CHAIN")) {
        do_chain();
    } else if (accept_word("COMMON")) {
        do_common();
    } else if (accept("SAVE")) {
        char name[64];
        file_name_arg(name, sizeof name, true);
        if (accept(","))
            ++p;  // ,A (ASCII) is the only format anyway
        save_file(name);
    } else if (accept("FILES")) {
        do_files();
    } else if (accept("KILL")) {
        do_kill();
    } else if (accept("DELETE")) {
        do_delete();
        return S_END;
    } else if (accept("RENUM")) {
        do_renum();
        return S_END;
    } else if (accept("TRON")) {
        trace = true;
    } else if (accept("TROFF")) {
        trace = false;
    } else if (accept("EDIT")) {
        if (cur_line >= 0)
            error(E_ILLEGAL_DIRECT);
        do_edit();
        return S_END;
    } else if (accept("MID$(")) {
        do_mid_statement();
    } else if (accept_word("OPTION")) {
        if (!accept_word("BASE"))
            error(E_SYNTAX);
        int b = arg_int();
        if (b != 0 && b != 1)
            error(E_SYNTAX);
        for (int i = 0; i < BUCKETS; ++i)
            if (arrays[i] && b != option_base)
                error(E_DUPLICATE_DEF);  // as GW-BASIC: before any array exists
        option_base = b;
    } else if (accept_word("ERASE")) {
        do_erase();
    } else if (accept("DATE$")) {
        expect("=");
        set_date_or_time(str_expr(), true);
    } else if (accept("TIME$")) {
        expect("=");
        set_date_or_time(str_expr(), false);
    } else if (accept_word("CHDIR") || accept_word("MKDIR") || accept_word("RMDIR")) {
        char which = start[0];
        char path[128];
        const char *ps = str_expr();
        snprintf(path, sizeof path, "%.*s", (int)slen(ps), ps);
        FRESULT r = which == 'C' ? f_chdir(path) : which == 'M' ? f_mkdir(path) : f_unlink(path);
        if (r != FR_OK)
            error(r == FR_EXIST ? E_PATH_ACCESS : r == FR_DENIED ? E_PATH_ACCESS : r == FR_NO_PATH || r == FR_NO_FILE ? E_PATH_NOT_FOUND : fresult_error(r));
    } else if (accept_word("ENVIRON")) {
        set_environ(str_expr());
    } else if (accept_word("SHELL")) {
        if (at_end_of_statement())
            error(E_ILLEGAL_CALL);  // there's no DOS to start
        const char *cmd = str_expr();
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)slen(cmd), cmd);
        out_flush();
        if (!port_shell(line))
            error(E_ILLEGAL_CALL);
        out_col = 0;
    } else if (accept_word("POKE")) {
        double off = num_expr();
        expect(",");
        int v = arg_int();
        poke_byte(seg_address(off), v);
    } else if (accept_word("BSAVE")) {
        do_bsave();
    } else if (accept_word("BLOAD")) {
        do_bload();
    } else if (accept_word("RESET")) {
        close_all_files();
    } else if (accept_word("LOCK") || accept_word("UNLOCK") || accept_word("MOTOR") ||
               accept_word("STRIG") || accept_word("PEN")) {
        skip_statement();  // single-user, no cassette, joystick or light pen
    } else if (accept_word("OUT") || accept_word("WAIT") || accept_word("CALL") ||
               accept_word("IOCTL")) {
        error(E_ILLEGAL_CALL);  // no PC I/O ports or machine code
    } else if (accept("SYSTEM") || accept("BYE")) {
        return S_SYSTEM;
    } else if (accept_word("KEY")) {
        do_key();
    } else if (accept_word("TIMER")) {
        set_event_state(EV_TIMER);
    } else if (accept_word("MOUSE")) {
        do_mouse();
    } else if (accept_word("MUSIC")) {
        do_music();
    } else if (accept("COM(")) {
        set_event_state(event_number("COM"));
    } else if (accept_word("WIDTH")) {
        // As GW-BASIC: WIDTH 80 in SCREEN 1 switches to SCREEN 2 and WIDTH 40
        // in SCREEN 2 back. Text otherwise stays 80 columns.
        skip_spaces();
        if (isdigit((unsigned char)*p)) {
            int w = arg_int();
            if (w == 80 && screen_mode == 1)
                set_screen_mode(2);
            else if (w == 40 && screen_mode == 2)
                set_screen_mode(1);
        }
        skip_statement();
    } else {
        // Implicit LET.
        lvalue_t lv = lvalue();
        expect("=");
        assign(lv, expr());
    }
    if (!jumped && !at_end_of_statement())
        error(E_SYNTAX);
    return S_CONTINUE;
}

/// Runs one direct-mode line (and any program it starts). Returns false on
/// SYSTEM.
static bool direct(const char *text) {
    snprintf(direct_line, sizeof direct_line, "%s", text);
    normalise(direct_line);
    cur_line = -1;
    p = direct_line;
    running = false;
    int code = setjmp(error_jump);
    if (code)
        free_temps();
    if (code && !trap_error(code)) {
        if (code == E_BREAK && running && cur_line >= 0 && cont_pos.line < -1)
            cont_pos = (pos_t){cur_line, p};  // Ctrl-C: CONT resumes here
        else if (code != E_BREAK)
            cont_pos.line = -2;
        print_error(code);
        if (running && code != E_BREAK)
            close_all_files();  // as GW-BASIC does when a program stops on an error
        if (running && music_from_program)
            port_music_stop();  // the program's music ends with it
        running = false;
        in_error_handler = false;
        printf("\x1b[0m");  // a program may have left colours set
        return true;
    }
    int r = execute();
    free_temps();
    in_error_handler = false;
    bool was_running = running;
    if (was_running)
        close_all_files();  // the program has ended
    if (was_running && music_from_program)
        port_music_stop();
    running = false;
    if (was_running && r != S_SYSTEM)
        cont_pos.line = -2;
    return r != S_SYSTEM;
}

/// Stores a typed program line; reports an error and returns false if it
/// can't. (setjmp lives here so no caller's locals are live across it.)
static bool try_store_line(int n, const char *text) {
    int code = setjmp(error_jump);
    if (code) {
        free_temps();
        print_error(code);
        return false;
    }
    if (n > 65529)
        error(E_SYNTAX);
    store_line(n, text);
    clear_vars();  // as GW-BASIC does when the program changes
    reset_run_state();
    cont_pos.line = -2;
    return true;
}

static void do_auto(int start, int step) {
    for (int n = start; n <= 65529; n += step) {
        char prefix[16], buf[LINE_MAX_LEN + 1];
        bool exact;
        find_line(n, &exact);
        snprintf(prefix, sizeof prefix, "%d%c", n, exact ? '*' : ' ');
        out_str(prefix);
        if (!read_line(buf, sizeof buf, NULL))
            return;
        const char *t = buf;
        while (*t == ' ')
            ++t;
        if (!*t) {
            if (exact)
                continue;  // keep the existing line
            return;
        }
        if (!try_store_line(n, t))
            return;
    }
}

static char (*edit_initial_p)[LINE_MAX_LEN + 8];  ///< EDIT n: the next line's starting text
#define edit_initial (*edit_initial_p)

/// BASIC's own variables, in SRAM (they're used all the time) but only while
/// it runs: the shell keeps none of BASIC's SRAM.
typedef struct {
    jmp_buf r_error_jump;
    var_t *r_vars[BUCKETS];
    array_t *r_arrays[BUCKETS];
    char r_direct_line[LINE_MAX_LEN + 1];
    event_t r_events[EV_COUNT];
    uint8_t r_attr_map[256];
    font_t r_fonts[MAX_FONTS];
    char r_edit_initial[LINE_MAX_LEN + 8];
} basic_ram_t;
static basic_ram_t *basic_ram;

static bool basic_ram_alloc(void) {
    basic_ram = (malloc)(sizeof *basic_ram);
    if (!basic_ram)
        return false;
    memset(basic_ram, 0, sizeof *basic_ram);
    error_jump_p = &basic_ram->r_error_jump;
    vars_p = &basic_ram->r_vars;
    arrays_p = &basic_ram->r_arrays;
    direct_line_p = &basic_ram->r_direct_line;
    events_p = &basic_ram->r_events;
    attr_map_p = &basic_ram->r_attr_map;
    fonts_p = &basic_ram->r_fonts;
    edit_initial_p = &basic_ram->r_edit_initial;
    return true;
}

static void basic_ram_free(void) {
    (free)(basic_ram);
    basic_ram = NULL;
    error_jump_p = NULL;
    vars_p = NULL;
    arrays_p = NULL;
    direct_line_p = NULL;
    events_p = NULL;
    attr_map_p = NULL;
    fonts_p = NULL;
    edit_initial_p = NULL;
}

void basic_main(const char *file) {
    if (!basic_ram_alloc()) {
        printf("basic: not enough memory\n");
        return;
    }
    // The stacks and COMMON's names only while BASIC runs; the line history
    // stays for next time. (Blocks this big go in PSRAM.)
    if (!history && (history = malloc(HISTORY * sizeof *history)))
        memset(history, 0, HISTORY * sizeof *history);
    // The control stacks in SRAM ((malloc), not port_alloc, which puts
    // blocks this big in PSRAM): every NEXT, RETURN and WEND uses them.
    for_stack = (malloc)(STACK_MAX * sizeof *for_stack);
    gosub_stack = (malloc)(STACK_MAX * sizeof *gosub_stack);
    while_stack = (malloc)(STACK_MAX * sizeof *while_stack);
    common_names = malloc(MAX_COMMON * sizeof *common_names);
    sprites = calloc(SPRITES, sizeof *sprites);
    shapes = calloc(SHAPES, sizeof *shapes);
    if (!history || !for_stack || !gosub_stack || !while_stack || !common_names || !sprites || !shapes) {
        printf("basic: not enough memory\n");
        goto out;
    }
    out_col = out_row = 0;
    screen_mode = 0;
    page_count = 1;
    active_page = visual_page = 0;
    key_head = key_tail = 0;
    break_pending = false;
    cont_pos.line = -2;
    trace = false;
    program_name[0] = 0;
    out_str("Pico BASIC (GW-BASIC style). SYSTEM returns to the shell.");
    out_newline();
    {
        char buf[48];
        size_t free_mem = port_free_mem();
        snprintf(buf, sizeof buf, "%u Bytes free",
                 (unsigned)(free_mem > HEAP_MARGIN ? free_mem - HEAP_MARGIN : 0));
        out_str(buf);
        out_newline();
    }
    if (file && *file) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "RUN \"%s\"", file);
        if (!direct(cmd))
            goto done;
    }
    bool ok = true;  // "Ok" is shown after direct commands, not program lines
    for (;;) {
        if (ok) {
            out_str("Ok");
            out_newline();
        }
        ok = true;
        char buf[LINE_MAX_LEN + 1];
        if (!read_line_ex(buf, sizeof buf, edit_initial[0] ? edit_initial : NULL, true))
            continue;
        edit_initial[0] = 0;
        const char *t = buf;
        while (*t == ' ')
            ++t;
        if (!*t) {
            ok = false;
            continue;
        }
        // EDIT n (or EDIT .): the line goes into the line editor.
        if (!strncasecmp(t, "EDIT", 4)) {
            const char *q = t + 4;
            while (*q == ' ')
                ++q;
            if (isdigit((unsigned char)*q) || *q == '.') {
                int n = *q == '.' ? last_line_num : (int)strtol(q, NULL, 10);
                bool exact;
                int i = find_line(n, &exact);
                if (!exact) {
                    out_str("Undefined line number");
                    out_newline();
                    continue;
                }
                snprintf(edit_initial, sizeof edit_initial, "%u %s", lines[i].num, lines[i].text);
                ok = false;
                continue;
            }
        }
        if (isdigit((unsigned char)*t)) {
            long n = strtol(t, (char **)&t, 10);
            ok = !try_store_line(n, t);
            continue;
        }
        if (!strncasecmp(t, "AUTO", 4)) {
            int start = 10, step = 10;
            const char *q = t + 4;
            while (*q == ' ')
                ++q;
            if (isdigit((unsigned char)*q))
                start = strtol(q, (char **)&q, 10);
            while (*q == ' ')
                ++q;
            if (*q == ',')
                step = strtol(q + 1, NULL, 10);
            do_auto(start, step > 0 ? step : 10);
            continue;
        }
        if (!direct(t))
            break;
    }
done:
    close_all_files();
    port_music_stop();
    port_mouse_pointer(false);
    port_pins_reset();
    port_reset_pages();
    set_key_line(false);
    if (screen_mode) {
        screen_mode = 0;
        printf("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    }
    clear_program();
    clear_vars();
    free(lines);
    lines = NULL;
    line_cap = 0;
    free(temps);
    temps = NULL;
    temp_cap = 0;
    printf("\x1b[0m");
    out_flush();
out:
    free(sprites);
    free(shapes);
    sprites = NULL;
    shapes = NULL;
    (free)(for_stack);
    (free)(gosub_stack);
    (free)(while_stack);
    free(common_names);
    for_stack = NULL;
    gosub_stack = NULL;
    while_stack = NULL;
    common_names = NULL;
    basic_ram_free();
}
