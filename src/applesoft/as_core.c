/**
 * @file as_core.c
 * @brief The Applesoft interpreter proper: program entry and listing, the statement loop, expressions, variables, arrays and strings.
 *
 * It keeps the program, variables and strings in the emulated 64 KB exactly
 * as the ROM lays them out, and keeps the ROM's pointers in zero page, so
 * PEEK, POKE, FRE and machine code all see what they would on an Apple.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_overlay.h"
#include "applesoft.h"
#include "as_internal.h"
#include "as_port.h"

uint8_t *M;
uint16_t tp;
as_fp fac;
bool fac_string;
uint16_t fac_desc;
void (*applesoft_on_exit)(const uint8_t *ram);

static uint8_t intflg;   ///< $12: the variable just found is an integer
static uint8_t dimflg;   ///< $10: PTRGET for DIM
static uint8_t dim_request;  ///< the next PTRGET is DIM's
static uint8_t subflg;   ///< $14: no arrays or % (FOR, DEF FN), or arrays only

static jmp_buf restart_jmp APP_OV_DATA(apple2);  ///< back to the ] prompt
static jmp_buf newstt_jmp APP_OV_DATA(apple2);  ///< carry on at TXTPTR (ONERR, RESUME)
static jmp_buf quit_jmp APP_OV_DATA(apple2);    ///< BYE
static bool newstt_jmp_valid;

// ---- Tokens ----------------------------------------------------------------------

enum {
    T_END = 0x80, T_FOR, T_NEXT, T_DATA, T_INPUT, T_DEL, T_DIM, T_READ, T_GR, T_TEXT,
    T_PRNUM, T_INNUM, T_CALL, T_PLOT, T_HLIN, T_VLIN, T_HGR2, T_HGR, T_HCOLOR, T_HPLOT,
    T_DRAW, T_XDRAW, T_HTAB, T_HOME, T_ROT, T_SCALE, T_SHLOAD, T_TRACE, T_NOTRACE,
    T_NORMAL, T_INVERSE, T_FLASH, T_COLOR, T_POP, T_VTAB, T_HIMEM, T_LOMEM, T_ONERR,
    T_RESUME, T_RECALL, T_STORE, T_SPEED, T_LET, T_GOTO, T_RUN, T_IF, T_RESTORE, T_AMPER,
    T_GOSUB, T_RETURN, T_REM, T_STOP, T_ON, T_WAIT, T_LOAD, T_SAVE, T_DEF, T_POKE,
    T_PRINT, T_CONT, T_LIST, T_CLEAR, T_GET, T_NEW, T_TAB, T_TO, T_FN, T_SPC, T_THEN,
    T_AT, T_NOT, T_STEP, T_PLUS, T_MINUS, T_MUL, T_DIV, T_POW, T_AND, T_OR, T_GREATER,
    T_EQUAL, T_LESS, T_SGN, T_INT, T_ABS, T_USR, T_FRE, T_SCRN, T_PDL, T_POS, T_SQR,
    T_RND, T_LOG, T_EXP, T_COS, T_SIN, T_TAN, T_ATN, T_PEEK, T_LEN, T_STRS, T_VAL,
    T_ASC, T_CHRS, T_LEFTS, T_RIGHTS, T_MIDS,
    T_LAST = T_MIDS,
};

static const char *const keywords[] = {
    "END", "FOR", "NEXT", "DATA", "INPUT", "DEL", "DIM", "READ", "GR", "TEXT", "PR#",
    "IN#", "CALL", "PLOT", "HLIN", "VLIN", "HGR2", "HGR", "HCOLOR=", "HPLOT", "DRAW",
    "XDRAW", "HTAB", "HOME", "ROT=", "SCALE=", "SHLOAD", "TRACE", "NOTRACE", "NORMAL",
    "INVERSE", "FLASH", "COLOR=", "POP", "VTAB", "HIMEM:", "LOMEM:", "ONERR", "RESUME",
    "RECALL", "STORE", "SPEED=", "LET", "GOTO", "RUN", "IF", "RESTORE", "&", "GOSUB",
    "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "DEF", "POKE", "PRINT", "CONT",
    "LIST", "CLEAR", "GET", "NEW", "TAB(", "TO", "FN", "SPC(", "THEN", "AT", "NOT", "STEP",
    "+", "-", "*", "/", "^", "AND", "OR", ">", "=", "<", "SGN", "INT", "ABS", "USR", "FRE",
    "SCRN(", "PDL", "POS", "SQR", "RND", "LOG", "EXP", "COS", "SIN", "TAN", "ATN", "PEEK",
    "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$",
};

static const char *const error_names[] = {
    [0] = "NEXT WITHOUT FOR", [16] = "SYNTAX", [22] = "RETURN WITHOUT GOSUB",
    [42] = "OUT OF DATA", [53] = "ILLEGAL QUANTITY", [69] = "OVERFLOW",
    [77] = "OUT OF MEMORY", [90] = "UNDEF'D STATEMENT", [107] = "BAD SUBSCRIPT",
    [120] = "REDIM'D ARRAY", [133] = "DIVISION BY ZERO", [149] = "ILLEGAL DIRECT",
    [163] = "TYPE MISMATCH", [176] = "STRING TOO LONG", [191] = "FORMULA TOO COMPLEX",
    [210] = "CAN'T CONTINUE", [224] = "UNDEF'D FUNCTION",
};

// ---- The 6502 stack, as far as Applesoft's limits go -----------------------------
// FOR and GOSUB records live on the Apple's 256-byte stack, which limits how
// deep they nest (and an ONERR handler entered from inside an expression
// leaves junk on it). We keep the records in C but count the bytes the ROM
// would use, so the same programs run out of memory.

#define STACK_TOP  0xf8
#define FOR_BYTES  18
#define GOSUB_BYTES 7

enum { FR_FOR = 0x81, FR_GOSUB = 0xb0, FR_JUNK = 0 };

typedef struct {
    ///@{
    uint8_t type, bytes;
    ///@}
    /// where to carry on
    ///@{
    uint16_t line, text;
    ///@}
    uint16_t var;          ///< FOR: the variable's value
    as_packed limit;       ///< FOR: the limit, packed
    as_fp step;            ///< FOR: the step (unrounded copy as pushed)
    int8_t step_sign;
} frame_t;

#define MAX_FRAMES 80
static frame_t frames[MAX_FRAMES] APP_OV_DATA(apple2);  ///< (the apps' SRAM area, while it runs)
static int nframes;
static uint8_t vsp = STACK_TOP;  ///< the S register at the start of a statement
static int vs = STACK_TOP;       ///< S as it would be now, inside a statement

/// The bytes the ROM would have on the stack (return addresses, operands)
/// while evaluating; stack_check is its test before each level.
static inline void vpush(int n) {
    vs -= n;
}

static inline void vpop(int n) {
    vs += n;
}

static void stack_reset(void) {
    nframes = 0;
    vsp = STACK_TOP;
}

static frame_t *push_frame(uint8_t type, uint8_t bytes) {
    if (nframes == MAX_FRAMES || bytes > vsp) {
        // Only an ONERR loop that never tidies up gets here: on the Apple
        // the stack pointer would wrap round. Start it afresh instead.
        stack_reset();
    }
    frame_t *f = &frames[nframes++];
    memset(f, 0, sizeof *f);
    f->type = type;
    f->bytes = bytes;
    vsp -= bytes;
    return f;
}

static void pop_frames_to(int n) {
    while (nframes > n)
        vsp += frames[--nframes].bytes;
}

// ---- Small helpers -------------------------------------------------------------

static inline bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static inline bool is_letter(uint8_t c) {
    return c >= 'A' && c <= 'Z';
}

bool is_end(uint8_t c) {
    return c == 0 || c == ':';
}

uint8_t chrgot(void) {
    for (;;) {
        uint8_t c = M[tp];
        if (c != ' ')
            return c;
        tp++;
    }
}

uint8_t chrget(void) {
    tp++;
    return chrgot();
}

void synchr(uint8_t c) {
    if (M[tp] != c)
        as_error(ERR_SYNTAX);
    chrget();
}

void chkcom(void) {
    synchr(',');
}

bool as_running(void) {
    return M[Z_CURLIN + 1] != 0xff;
}

static void err_direct(void) {
    if (!as_running())
        as_error(ERR_ILLEGAL_DIRECT);
}

void as_sync_out(void) {
    sw16(Z_TXTPTR, tp);
    M[Z_FAC] = fac.exp;
    M[Z_FAC + 1] = (uint8_t)(fac.mant >> 24);
    M[Z_FAC + 2] = (uint8_t)(fac.mant >> 16);
    M[Z_FAC + 3] = (uint8_t)(fac.mant >> 8);
    M[Z_FAC + 4] = (uint8_t)fac.mant;
    M[Z_FAC + 5] = fac.neg ? 0xff : 0;
    M[0xac] = fac.ext;
    M[0x11] = fac_string ? 0xff : 0;
    M[0x12] = intflg;
    if (fac_string)
        sw16(0xa0, fac_desc);
}

void as_sync_in(void) {
    tp = w16(Z_TXTPTR);
    fac_string = M[0x11] & 0x80;
    intflg = M[0x12];
    if (fac_string) {
        fac_desc = w16(0xa0);
    } else {
        fac.exp = M[Z_FAC];
        fac.mant = (uint32_t)M[Z_FAC + 1] << 24 | (uint32_t)M[Z_FAC + 2] << 16 |
                   (uint32_t)M[Z_FAC + 3] << 8 | M[Z_FAC + 4];
        fac.neg = M[Z_FAC + 5] & 0x80;
        fac.ext = M[0xac];
    }
}

void as_print(const char *s) {
    while (*s)
        as_outdo((uint8_t)*s++);
}

static void print_uint(unsigned v) {
    as_fp f = fp_from_int16(0);
    // LINPRT: an unsigned 16-bit number through FOUT.
    f.exp = 0x90;
    f.mant = (uint32_t)(v & 0xffff) << 16;
    f.ext = 0;
    f.neg = false;
    if (!v) {
        f.exp = 0;
    } else {
        while (!(f.mant & 0x80000000u)) {
            f.mant <<= 1;
            f.exp--;
        }
    }
    char buf[20];
    fp_format(&f, buf);
    as_print(buf);
}

// ---- Errors -----------------------------------------------------------------------

static void fp_error_hook(int code) {
    as_error(code);
}

static void onerr_handler(int code) {
    M[Z_ERRNUM] = (uint8_t)code;
    M[Z_ERRSTK] = M[Z_REMSTK];
    sw16(Z_ERRLIN, w16(Z_CURLIN));
    sw16(Z_ERRPOS, w16(Z_OLDTEXT));
    // The ROM jumps to the handler without tidying its stack: what an
    // expression had pushed stays there (until RESUME or CALL -3288).
    int junk = vsp - vs;
    if (junk > 0)
        push_frame(FR_JUNK, (uint8_t)junk);
    vs = vsp;
    tp = w16(Z_TXTPSV);
    sw16(Z_CURLIN, w16(Z_CURLSV));
    chrgot();
    // GOTO the handler line
    extern void stmt_goto(void);
    stmt_goto();
    longjmp(newstt_jmp, 1);
}

static void stkini(void) {
    M[Z_TEMPPT] = Z_TEMPST;
    M[Z_OLDTEXT + 1] = 0;  // CONT can't
    subflg = 0;
    stack_reset();
    vs = vsp;
}

static uint16_t unsupported_addr;

void as_unsupported_call(uint16_t addr) {
    unsupported_addr = addr;
    as_error(ERR_UNSUPPORTED);
}

void as_error(int code) {
    if ((M[Z_ERRFLG] & 0x80) && code != ERR_UNSUPPORTED && newstt_jmp_valid)
        onerr_handler(code >= ERR_DOS ? code - ERR_DOS : code);
    as_crdo();
    if (code == ERR_UNSUPPORTED) {
        char buf[48];
        snprintf(buf, sizeof buf, "?NO ROM ROUTINE AT $%04X", unsupported_addr);
        as_print(buf);
        as_outdo(7);
    } else if (code >= ERR_DOS) {
        extern const char *dos_error_text(int code);
        as_print(dos_error_text(code - ERR_DOS));
        as_outdo(7);
    } else {
        as_outdo('?');
        const char *name = code < (int)(sizeof error_names / sizeof error_names[0]) &&
                                   error_names[code]
                               ? error_names[code]
                               : "SYNTAX";
        as_print(name);
        as_print(" ERROR");
        as_outdo(7);
    }
    as_close_dos_io();
    stkini();
    if (as_running() && code < ERR_DOS) {
        as_print(" IN ");
        print_uint(w16(Z_CURLIN));
    }
    longjmp(restart_jmp, 1);
}

void as_dos_error(int code) {
    as_error(ERR_DOS + code);
}

void as_quit(void) {
    longjmp(quit_jmp, 1);
}

void as_quit_to_prompt(void) {
    longjmp(restart_jmp, 2);
}

static void run_from(bool first);

void as_start_run(void) {
    as_run_program();
    if (newstt_jmp_valid)
        longjmp(newstt_jmp, 1);
    run_from(false);
    for (;;)
        ;  // run_from never returns
}

/// CHAIN: DOS 3.3 needs a helper program for this in Applesoft; here it is
/// RUN of the other program.
void as_chain(const char *name) {
    dos_load_program(name);
    as_start_run();
}

// ---- Memory management --------------------------------------------------------

static void gc(void);

/// REASON: is there room for something to end at addr? Collects garbage once.
static void reason(uint16_t addr) {
    if (addr < w16(Z_FRETOP))
        return;
    gc();
    if (addr >= w16(Z_FRETOP))
        as_error(ERR_OUT_OF_MEMORY);
}

/// Moves [from, end) up or down by delta (memmove).
static void move_block(uint16_t from, uint16_t end, int delta) {
    if (end > from)
        memmove(&M[(uint16_t)(from + delta)], &M[from], end - from);
}

// ---- Program lines -------------------------------------------------------------

/// FNDLIN: the address of the first line numbered >= num, searching from
/// `from`. Returns true if that line is numbered exactly num.
static bool fndlin_from(uint16_t from, uint16_t num, uint16_t *where) {
    uint16_t p = from;
    for (;;) {
        if (M[(uint16_t)(p + 1)] == 0) {  // link high byte zero: end
            *where = p;
            return false;
        }
        uint16_t n = w16(p + 2);
        if (n >= num) {
            *where = p;
            return n == num;
        }
        p = w16(p);
    }
}

static bool fndlin(uint16_t num, uint16_t *where) {
    return fndlin_from(w16(Z_TXTTAB), num, where);
}

void as_fix_links(void) {
    uint16_t p = w16(Z_TXTTAB);
    while (M[(uint16_t)(p + 1)]) {
        uint16_t q = p + 4;
        while (M[q])
            q++;
        sw16(p, q + 1);
        p = q + 1;
    }
    // PRGEND: just past the two zero bytes
    sw16(Z_PRGEND, w16(Z_VARTAB));
}

static void clearc(void);

void linget(void) {
    uint16_t n = 0;
    uint8_t c = chrgot();
    while (is_digit(c)) {
        if (n >= 6400)
            as_error(ERR_SYNTAX);
        n = (uint16_t)(n * 10 + (c - '0'));
        c = chrget();
    }
    sw16(Z_LINNUM, n);
}

/// Stores the crunched line at $200 (length len, with its NUL) as line LINNUM.
static void insert_line(int len) {
    uint16_t num = w16(Z_LINNUM);
    sw16(Z_VARTAB, w16(Z_PRGEND));
    uint16_t at;
    if (fndlin(num, &at)) {
        // Delete the old line.
        uint16_t next = w16(at);
        uint16_t end = w16(Z_VARTAB);
        move_block(next, end, -(int)(next - at));
        sw16(Z_VARTAB, end - (next - at));
    }
    if (M[0x200]) {
        // Room for link, number and the text.
        uint16_t size = (uint16_t)(len + 4);
        uint16_t end = w16(Z_VARTAB);
        sw16(Z_FRETOP, w16(Z_MEMSIZ));
        reason((uint16_t)(end + size));
        move_block(at, end, size);
        sw16(Z_VARTAB, end + size);
        sw16(at + 2, num);
        memcpy(&M[at + 4], &M[0x200], len);
        sw16(at, 0xffff);  // non-zero link until fixed
    }
    clearc();
    as_fix_links();
}

// ---- PARSE: tokenising a line ------------------------------------------------------

static uint8_t upper(uint8_t c) {
    return c >= 0x61 ? (uint8_t)(c & 0x5f) : c;
}

/// Tries the keywords at in[x]; returns the token and sets *end, or 0.
static uint8_t match_keyword(const uint8_t *in, int x, int *end) {
    for (int t = 0; t <= T_LAST - 0x80; ++t) {
        const char *k = keywords[t];
        int i = x;
        bool ok = true;
        for (const char *p = k; *p; ++p) {
            while (in[i] == ' ')
                i++;
            if (upper(in[i]) != (uint8_t)*p) {
                ok = false;
                break;
            }
            i++;
        }
        if (!ok)
            continue;
        if (t + 0x80 == T_AT) {
            int j = i;
            while (in[j] == ' ')
                j++;
            uint8_t n = upper(in[j]);
            if (n == 'N' || n == 'O')
                continue;  // ATN, or A TO
        }
        *end = i;
        return (uint8_t)(t + 0x80);
    }
    return 0;
}

int as_crunch(void) {
    uint8_t in[256];
    memcpy(in, &M[0x200], 256);
    in[255] = 0;
    int x = 0, y = 0;
    bool data = false;
    for (;;) {
        uint8_t c = in[x];
        if (!data)
            while (c == ' ')
                c = in[++x];
        uint8_t out;
        if (c == '"') {
            M[0x200 + y++] = '"';
            x++;
            for (;;) {
                c = in[x];
                if (c == 0)
                    break;
                M[0x200 + y++] = c;
                x++;
                if (c == '"')
                    break;
            }
            if (c == 0) {
                M[0x200 + y++] = 0;
                break;
            }
            continue;
        }
        if (data) {
            out = c;
            x++;
        } else if (c == '?') {
            out = T_PRINT;
            x++;
        } else if (c >= '0' && c <= ';') {
            out = c;
            x++;
        } else {
            int end;
            uint8_t t = match_keyword(in, x, &end);
            if (t) {
                out = t;
                x = end;
            } else {
                out = upper(c);
                x++;
            }
        }
        M[0x200 + y++] = out;
        if (out == 0)
            break;
        if (out == ':')
            data = false;
        else if (out == T_DATA)
            data = true;
        else if (out == T_REM) {
            while ((c = in[x]) != 0) {
                M[0x200 + y++] = c;
                x++;
            }
            M[0x200 + y++] = 0;
            break;
        }
    }
    // Two more zeros after the line: a direct statement then ends the
    // "program".
    M[0x200 + y] = 0;
    M[0x200 + y + 1] = 0;
    return y;
}

// ---- LIST ------------------------------------------------------------------------

static void list_wrap(void) {
    extern int text_column(void);
    extern int text_list_wrap(void);
    if (text_column() >= text_list_wrap()) {
        as_crdo();
        M[Z_CH] = 5;
    }
}

static void stmt_list(void) {
    uint8_t c = chrgot();
    if (!(is_digit(c) || is_end(c) || c == T_MINUS || c == ','))
        return;  // NEWSTT reports the syntax error
    linget();
    uint16_t p;
    fndlin(w16(Z_LINNUM), &p);
    c = chrgot();
    if (!is_end(c)) {
        if (c != T_MINUS && c != ',')
            return;
        chrget();
        linget();
        if (!is_end(chrgot()))
            return;
    }
    uint16_t last = w16(Z_LINNUM);
    if (!last)
        last = 0xffff;
    for (;;) {
        if (M[(uint16_t)(p + 1)] == 0)
            break;
        if (as_kbd_poll_break()) {
            as_rdkey();
            extern void do_break(void);
            do_break();
        }
        as_crdo();
        uint16_t num = w16(p + 2);
        if (num > last)
            break;
        as_outdo(' ');
        print_uint(num);
        as_outdo(' ');
        list_wrap();
        uint16_t q = p + 4;
        uint8_t b;
        while ((b = M[q++]) != 0) {
            if (b < 0x80) {
                as_outdo(b);
            } else {
                as_outdo(' ');
                const char *k = b <= T_LAST ? keywords[b - 0x80] : "";
                as_print(k);
                as_outdo(' ');
            }
            list_wrap();
        }
        p = w16(p);
    }
    as_outdo('\r');
}

int as_detok_line(uint16_t line, char *out, int max) {
    // Spaces outside strings, REM and DATA don't matter to the tokeniser,
    // so they go where they read best: around keywords, not around
    // operators or before a parenthesis.
    int n = snprintf(out, max, "%u ", w16(line + 2));
    uint16_t q = line + 4;
    uint8_t b;
    bool quote = false;
    while ((b = M[q++]) != 0 && n < max - 12) {
        if (b < 0x80 || quote) {
            if (b == '"')
                quote = !quote;
            out[n++] = (char)(b & 0x7f);
            continue;
        }
        const char *k = b <= T_LAST ? keywords[b - 0x80] : "";
        bool op = b >= T_PLUS && b <= T_LESS && b != T_AND && b != T_OR;
        if (!op && n && out[n - 1] != ' ' && out[n - 1] != '(')
            out[n++] = ' ';
        int kl = (int)strlen(k);
        memcpy(out + n, k, kl);
        n += kl;
        if (b == T_REM || b == T_DATA) {
            // the rest is kept exactly as typed
            while ((b = M[q]) != 0 && n < max - 2) {
                out[n++] = (char)b;
                q++;
            }
            break;
        }
        uint8_t next = M[q];
        if (!op && k[kl - 1] != '(' && next && next != '(' && next != ':' && next != ',' &&
            next != ';' && next != ')' && !(next >= T_PLUS && next <= T_LESS))
            out[n++] = ' ';
    }
    out[n] = 0;
    return n;
}

// ---- Strings ------------------------------------------------------------------------

/// Descriptor at d: length, address.
static inline uint8_t desc_len(uint16_t d) {
    return M[d];
}

static inline uint16_t desc_addr(uint16_t d) {
    return w16(d + 1);
}

/// GETSPA: n bytes of string space (collecting garbage once if need be).
static uint16_t getspa(uint8_t n) {
    for (int tries = 0;; ++tries) {
        uint16_t top = (uint16_t)(w16(Z_FRETOP) - n);
        if (w16(Z_FRETOP) >= n && top >= w16(Z_STREND)) {
            sw16(Z_FRETOP, top);
            sw16(Z_FRESPC, top);
            return top;
        }
        if (tries)
            as_error(ERR_OUT_OF_MEMORY);
        gc();
    }
}

/// The garbage collector: moves every string in use to the top of string
/// space, highest first, as the ROM does. With room for a list of the
/// descriptors it does it in one sorted pass; otherwise the ROM's way, one
/// string per pass.
typedef struct {
    ///@{
    uint16_t desc, addr;
    ///@}
} gc_ref;

#define GC_MAX_REFS 22000  ///< more descriptors than 64 KB can hold
static gc_ref *gc_refs;

static int gc_cmp(const void *a, const void *b) {
    const gc_ref *x = a, *y = b;
    return (int)y->addr - (int)x->addr;
}

/// Calls fn for every string descriptor: temporaries, simple variables,
/// string arrays.
static void each_descriptor(void (*fn)(uint16_t d, void *ctx), void *ctx) {
    for (uint16_t d = Z_TEMPST; d < M[Z_TEMPPT]; d += 3)
        fn(d, ctx);
    for (uint16_t v = w16(Z_VARTAB); v < w16(Z_ARYTAB); v += 7)
        if (!(M[v] & 0x80) && (M[v + 1] & 0x80))
            fn((uint16_t)(v + 2), ctx);
    for (uint16_t a = w16(Z_ARYTAB); a < w16(Z_STREND);) {
        uint16_t size = w16(a + 2);
        if (!size)
            break;
        if (!(M[a] & 0x80) && (M[a + 1] & 0x80))
            for (uint16_t e = (uint16_t)(a + 5 + 2 * M[a + 4]); e < a + size; e += 3)
                fn(e, ctx);
        a += size;
    }
}

typedef struct {
    /// strings in [lo, hi) are in string space
    ///@{
    uint16_t lo, hi;
    ///@}
    int n;
    bool overflow;
    /// the one-at-a-time search
    ///@{
    uint16_t best, best_addr;
    ///@}
} gc_state;

static void gc_collect(uint16_t d, void *ctx) {
    gc_state *g = ctx;
    uint16_t a = desc_addr(d);
    if (!M[d] || a < g->lo || a >= g->hi)
        return;
    if (g->n < GC_MAX_REFS && gc_refs)
        gc_refs[g->n++] = (gc_ref){d, a};
    else
        g->overflow = true;
}

static void gc_highest(uint16_t d, void *ctx) {
    gc_state *g = ctx;
    uint16_t a = desc_addr(d);
    if (!M[d] || a < g->lo || a >= g->hi)
        return;
    if (!g->best || a >= g->best_addr) {
        g->best = d;
        g->best_addr = a;
    }
}

static void gc(void) {
    gc_state g = {w16(Z_STREND), w16(Z_MEMSIZ), 0, false, 0, 0};
    uint16_t top = g.hi;
    each_descriptor(gc_collect, &g);
    if (!g.overflow) {
        qsort(gc_refs, g.n, sizeof gc_refs[0], gc_cmp);
        for (int i = 0; i < g.n; ++i) {
            uint16_t d = gc_refs[i].desc;
            uint16_t from = gc_refs[i].addr;
            if (i && gc_refs[i - 1].addr == from) {
                // Two descriptors, one string: it moves once.
                sw16(d + 1, w16(gc_refs[i - 1].desc + 1));
                continue;
            }
            top -= M[d];
            memmove(&M[top], &M[from], M[d]);
            sw16(d + 1, top);
        }
    } else {
        for (;;) {
            g.best = 0;
            g.hi = top;
            each_descriptor(gc_highest, &g);
            if (!g.best)
                break;
            top -= M[g.best];
            memmove(&M[top], &M[g.best_addr], M[g.best]);
            sw16(g.best + 1, top);
        }
    }
    sw16(Z_FRETOP, top);
}

/// FRETMS: if d is the last temporary, drop it.
static bool fretms(uint16_t d) {
    if (d == M[Z_LASTPT] && M[Z_LASTPT + 1] == 0) {
        M[Z_TEMPPT] = (uint8_t)d;
        M[Z_LASTPT] = (uint8_t)(d - 3);
        return true;
    }
    return false;
}

/// FRETMP: releases a string that is being used up; if it was a temporary at
/// the bottom of string space, gives the space back. Returns its address.
static uint16_t fretmp(uint16_t d, uint8_t *len) {
    bool was_temp = fretms(d);
    *len = M[d];
    uint16_t addr = desc_addr(d);
    if (was_temp && addr == w16(Z_FRETOP))
        sw16(Z_FRETOP, addr + *len);
    return addr;
}

/// PUTNEW: a new temporary descriptor (len, addr) becomes the FAC's value.
static void putnew(uint8_t len, uint16_t addr) {
    uint8_t x = M[Z_TEMPPT];
    if (x == Z_TEMPST + 9)
        as_error(ERR_FORMULA_TOO_COMPLEX);
    M[x] = len;
    sw16(x + 1, addr);
    fac_desc = x;
    fac_string = true;
    M[Z_LASTPT] = x;
    M[Z_LASTPT + 1] = 0;
    M[Z_TEMPPT] = (uint8_t)(x + 3);
}

/// STRLIT/STRLT2: a string from addr up to t1, t2 or a NUL. Strings in zero
/// page or the input buffer are copied into string space; others are used
/// where they are. Returns the address after it (past a closing quote).
static uint16_t strlit(uint16_t addr, uint8_t t1, uint8_t t2) {
    int n = 0;
    uint8_t c;
    while ((c = M[(uint16_t)(addr + n)]) != 0 && c != t1 && c != t2)
        n++;
    uint16_t after = (uint16_t)(addr + n + (c == '"' ? 1 : 0));
    if (n > 255)
        n = 255;
    uint8_t hi = addr >> 8;
    if (hi == 0 || hi == 2) {
        uint16_t s = getspa((uint8_t)n);
        memmove(&M[s], &M[addr], n);
        addr = s;
    }
    putnew((uint8_t)n, addr);
    return after;
}

static void chkstr(void) {
    if (!fac_string)
        as_error(ERR_TYPE_MISMATCH);
}

static void chknum(void) {
    if (fac_string)
        as_error(ERR_TYPE_MISMATCH);
}

// ---- Variables and arrays ------------------------------------------------------

/// Where a variable that doesn't exist reads from: five zero bytes in the
/// ROM area (as the ROM points such variables at zeros in itself).
#define ZERO_VALUE 0xe09a
static const uint8_t zero5[5];
static bool fn_mode;  ///< PTRGET for an FN name

/// PTRGET: finds (or creates) the variable named at TXTPTR; returns the
/// address of its value (or of an array element). For a variable that
/// doesn't exist yet and is only being read, returns NULL (it reads as zero
/// or empty, and isn't created).
static uint16_t ptrget_ex(bool reading, bool *missing);

static uint16_t ptrget(void) {
    return ptrget_ex(false, NULL);
}

static uint16_t find_or_make_array(uint8_t n1, uint8_t n2, int ndims, const uint16_t *subs);

static uint16_t ptrget_ex(bool reading, bool *missing) {
    uint8_t c = chrgot();
    if (!is_letter(c))
        as_error(ERR_SYNTAX);
    uint8_t n1 = fn_mode ? (uint8_t)(c | 0x80) : c, n2 = 0;
    fn_mode = false;
    dimflg = dim_request;
    dim_request = 0;
    fac_string = false;
    intflg = 0;
    c = chrget();
    if (is_digit(c) || is_letter(c)) {
        n2 = c;
        do
            c = chrget();
        while (is_digit(c) || is_letter(c));
    }
    if (c == '$') {
        fac_string = true;
        n2 |= 0x80;
        c = chrget();
    } else if (c == '%') {
        if (subflg & 0x80)
            as_error(ERR_SYNTAX);
        intflg = 0x80;
        n1 |= 0x80;
        n2 |= 0x80;
        c = chrget();
    }
    M[Z_VARNAM] = n1;
    M[Z_VARNAM + 1] = n2;
    bool array = (c == '(' && !subflg) || (subflg & 0x40);
    if (!array || (subflg & 0x80)) {
        subflg = 0;
        uint16_t v = w16(Z_VARTAB);
        uint16_t end = w16(Z_ARYTAB);
        for (; v != end; v += 7)
            if (M[v] == n1 && M[v + 1] == n2) {
                sw16(Z_VARPNT, v + 2);
                return v + 2;
            }
        if (reading) {
            if (missing)
                *missing = true;
            return ZERO_VALUE;
        }
        // Make it: arrays move up 7 bytes.
        uint16_t strend = w16(Z_STREND);
        reason((uint16_t)(strend + 7));
        move_block(end, strend, 7);
        sw16(Z_ARYTAB, end + 7);
        sw16(Z_STREND, strend + 7);
        M[end] = n1;
        M[end + 1] = n2;
        memset(&M[end + 2], 0, 5);
        sw16(Z_VARPNT, end + 2);
        return end + 2;
    }
    // Array: subscripts in parentheses.
    uint8_t saved_dim = dimflg, saved_int = intflg;
    bool saved_str = fac_string;
    uint16_t subs[89] = {0};
    int nd = 0;
    if (subflg & 0x40) {
        subflg = 0;
        return find_or_make_array(n1, n2, -1, subs);
    }
    vpush(2 + 5);  // JSR PTRGET, then the name and flags
    do {
        chrget();
        vpush(2);
        frmnum();
        vpop(2);
        if (fac.neg)
            as_error(ERR_ILLEGAL_QUANTITY);
        int v = fp_to_int16(&fac);
        if (nd < 89)
            subs[nd++] = (uint16_t)v;
        else
            as_error(ERR_OUT_OF_MEMORY);
        vpush(2);
    } while (chrgot() == ',');
    vpop(7 + 2 * nd);
    synchr(')');
    dimflg = saved_dim;
    intflg = saved_int;
    fac_string = saved_str;
    return find_or_make_array(n1, n2, nd, subs);
}

/// Arrays: name (2), size of the whole array (2), number of dimensions (1),
/// the dimensions (2 each, high byte first, last subscript first), elements.
static uint16_t find_or_make_array(uint8_t n1, uint8_t n2, int nd, const uint16_t *subs) {
    uint16_t a = w16(Z_ARYTAB), end = w16(Z_STREND);
    int esize = (n1 & 0x80) ? 2 : (n2 & 0x80) ? 3 : 5;
    while (a != end) {
        if (M[a] == n1 && M[a + 1] == n2)
            break;
        uint16_t size = w16(a + 2);
        if (!size)
            as_error(ERR_BAD_SUBSCRIPT);
        a += size;
        if (a < w16(Z_ARYTAB))
            as_error(ERR_BAD_SUBSCRIPT);
    }
    if (a != end) {
        if (dimflg)
            as_error(ERR_REDIMD_ARRAY);
        if (nd < 0)
            return a;
        if (M[a + 4] != nd)
            as_error(ERR_BAD_SUBSCRIPT);
    } else {
        if (nd < 0)
            as_error(ERR_OUT_OF_DATA);
        uint16_t hdr = (uint16_t)(a + 5 + 2 * nd);
        reason(hdr);
        M[a] = n1;
        M[a + 1] = n2;
        M[a + 4] = (uint8_t)nd;
        uint32_t total = esize;
        for (int i = 0; i < nd; ++i) {
            uint32_t d = dimflg ? (uint32_t)subs[nd - 1 - i] + 1 : 11;
            M[a + 5 + 2 * i] = (uint8_t)(d >> 8);
            M[a + 6 + 2 * i] = (uint8_t)d;
            total *= d;
            if (total > 0xffff)
                as_error(ERR_OUT_OF_MEMORY);
        }
        uint32_t new_end = hdr + total;
        if (new_end > 0xffff)
            as_error(ERR_OUT_OF_MEMORY);
        reason((uint16_t)new_end);
        memset(&M[hdr], 0, total);
        sw16(Z_STREND, (uint16_t)new_end);
        sw16(a + 2, (uint16_t)(new_end - a));
        if (dimflg)
            return a;
    }
    // The element: last subscript most significant.
    uint32_t off = 0;
    for (int i = 0; i < nd; ++i) {
        uint16_t s = subs[nd - 1 - i];
        uint16_t d = (uint16_t)(M[a + 5 + 2 * i] << 8 | M[a + 6 + 2 * i]);
        if (s >= d)
            as_error(ERR_BAD_SUBSCRIPT);
        off = off * d + s;
        if (off > 0xffff)
            as_error(ERR_OUT_OF_MEMORY);
    }
    uint16_t e = (uint16_t)(a + 5 + 2 * nd + off * esize);
    sw16(Z_VARPNT, e);
    return e;
}

/// Reads the value at v into the FAC (a string: the FAC holds its descriptor).
static void load_value(uint16_t v, bool missing) {
    (void)missing;
    if (fac_string) {
        fac_desc = v;
        fac.ext = 0;
        return;
    }
    if (intflg & 0x80)
        fac = fp_from_int16((int16_t)(M[v] << 8 | M[(uint16_t)(v + 1)]));
    else
        fac = fp_unpack(&M[v]);
}

/// LET's store: the FAC into the variable at v.
static void let_store(uint16_t v, bool is_string, uint8_t is_int) {
    if (is_string) {
        uint16_t d = fac_desc;
        uint16_t addr = desc_addr(d);
        bool copy = addr >= w16(Z_FRETOP) && d >= w16(Z_VARTAB);
        if (copy) {
            // Another variable's string: this one gets its own copy.
            uint8_t len = M[d];
            uint16_t s = getspa(len);
            memmove(&M[s], &M[desc_addr(d)], len);
            M[0x9d] = len;
            sw16(0x9e, s);
            d = 0x9d;
        } else {
            fretms(d);
        }
        M[v] = M[d];
        M[(uint16_t)(v + 1)] = M[(uint16_t)(d + 1)];
        M[(uint16_t)(v + 2)] = M[(uint16_t)(d + 2)];
        return;
    }
    if (is_int & 0x80) {
        fp_round(&fac);
        int i = fp_to_int16(&fac);
        M[v] = (uint8_t)(i >> 8);
        M[(uint16_t)(v + 1)] = (uint8_t)i;
    } else {
        fp_pack(&fac, &M[v]);
    }
}

// ---- Expressions ----------------------------------------------------------------

enum { P_OR = 0x46, P_AND = 0x50, P_REL = 0x64, P_ADD = 0x79, P_MUL = 0x7b, P_POW = 0x7d,
       P_UNARY = 0x7f };

static void element(void);
static void eval(uint8_t min_prec);

void frmevl(void) {
    tp--;  // the ROM's FRMEVL backs up so its first CHRGET re-reads
    vpush(2);
    eval(0);
    vpop(2);
}

void frmnum(void) {
    vpush(2);
    frmevl();
    vpop(2);
    chknum();
}

/// The ROM's check that the stack has room for 2A + $36 bytes more.
static void stack_check(int a) {
    if (vs - 2 < 2 * a + 0x36)
        as_error(ERR_OUT_OF_MEMORY);
    if (asp_stack_low())
        as_error(ERR_OUT_OF_MEMORY);
}

static int string_compare(uint16_t left, uint16_t right) {
    uint8_t ll, rl;
    uint16_t ra = fretmp(right, &rl);
    uint16_t la = fretmp(left, &ll);
    int n = ll < rl ? ll : rl;
    for (int i = 0; i < n; ++i) {
        uint8_t a = M[(uint16_t)(la + i)], b = M[(uint16_t)(ra + i)];
        if (a != b)
            return a < b ? -1 : 1;
    }
    return ll == rl ? 0 : ll < rl ? -1 : 1;
}

static void eval(uint8_t min_prec) {
    vpush(1);  // the pending precedence
    stack_check(1);
    vpush(2);
    element();
    vpop(2);
    for (;;) {
        uint8_t c = chrgot();
        uint16_t op_at = tp;
        uint8_t rel = 0;
        while (c >= T_GREATER && c <= T_LESS) {
            uint8_t bit = (uint8_t)(1 << (c - T_GREATER));
            if (rel & bit)
                as_error(ERR_SYNTAX);
            rel |= bit;
            c = chrget();
        }
        uint8_t prec;
        if (rel) {
            prec = P_REL;
            c = 0;
        } else if (c >= T_PLUS && c <= T_OR) {
            if (c == T_PLUS && fac_string) {
                // Concatenation, an element at a time, whatever the precedence.
                uint16_t left = fac_desc;
                vpush(4);
                element();
                vpop(4);
                chkstr();
                uint16_t right = fac_desc;
                int total = M[left] + M[right];
                if (total > 255)
                    as_error(ERR_STRING_TOO_LONG);
                uint16_t s = getspa((uint8_t)total);
                uint8_t l1 = M[left], l2;
                memmove(&M[s], &M[desc_addr(left)], l1);
                uint16_t a2 = fretmp(right, &l2);
                memmove(&M[(uint16_t)(s + l1)], &M[a2], l2);
                fretmp(left, &l1);
                putnew((uint8_t)total, s);
                continue;
            }
            static const uint8_t precs[] = {P_ADD, P_ADD, P_MUL, P_MUL, P_POW, P_AND, P_OR};
            prec = precs[c - T_PLUS];
        } else {
            break;
        }
        if (prec <= min_prec) {
            tp = op_at;  // the caller applies its operator first, then rescans
            break;
        }
        // The left operand: a number is rounded, as the ROM pushes it.
        bool left_str = fac_string;
        if (!rel)
            chknum();
        uint16_t left_desc = fac_desc;
        as_fp left = fac;
        if (!left_str) {
            fp_round(&left);
            left.ext = 0;
        }
        if (rel)
            tp--;  // back to the last relational operator
        vpush(11);  // the left operand, its sign and the operator
        eval(prec);
        vpop(11);
        if (rel) {
            if (fac_string != left_str)
                as_error(ERR_TYPE_MISMATCH);
            int cmp;  // left against right
            if (left_str) {
                cmp = string_compare(left_desc, fac_desc);
            } else {
                as_packed m;
                m[0] = left.exp;
                m[1] = (uint8_t)((left.mant >> 24) & (left.neg ? 0xff : 0x7f));
                m[2] = (uint8_t)(left.mant >> 16);
                m[3] = (uint8_t)(left.mant >> 8);
                m[4] = (uint8_t)left.mant;
                cmp = -fp_compare(&fac, m);
            }
            uint8_t bit = cmp > 0 ? 1 : cmp == 0 ? 2 : 4;
            fac = fp_from_byte((rel & bit) ? 1 : 0);
            fac_string = false;
            continue;
        }
        chknum();
        switch (c) {
        case T_PLUS: fp_add(&fac, &left); break;
        case T_MINUS: fp_sub(&fac, &left); break;
        case T_MUL: fp_mul(&fac, &left); break;
        case T_DIV: fp_div(&fac, &left); break;
        case T_POW: fp_pow(&fac, &left); break;
        case T_AND: fac = fp_from_uint8(left.exp && fac.exp ? 1 : 0); break;
        case T_OR: fac = fp_from_uint8(left.exp || fac.exp ? 1 : 0); break;
        }
    }
    vpop(1);
}

static void paren_expr(void) {
    synchr('(');
    frmevl();
    synchr(')');
}

/// CONINT/GETBYT on the FAC: 0-255 or ILLEGAL QUANTITY.
static int conint(void) {
    chknum();
    if (fac.neg)
        as_error(ERR_ILLEGAL_QUANTITY);
    int v = fp_to_int16(&fac);
    if (v > 255 || v < 0)
        as_error(ERR_ILLEGAL_QUANTITY);
    return v;
}

int getbyt(void) {
    vpush(2);
    frmnum();
    vpop(2);
    return conint();
}

int gtbytc(void) {
    chrget();
    return getbyt();
}

uint16_t getadr(void) {
    frmnum();
    return fp_to_addr(&fac);
}

static void fn_call(void);
static void string_function(uint8_t t);

static void element(void) {
    fac_string = false;
    uint8_t c = chrget();
    for (;;) {
        if (is_digit(c) || c == '.') {
            as_fin_src src = {
                .peek = (int (*)(void *))(void *)chrgot,
                .next = (void (*)(void *))(void *)chrget,
            };
            fac = fp_parse(&src);
            return;
        }
        if (is_letter(c)) {
            bool missing = false;
            uint16_t v = ptrget_ex(true, &missing);
            load_value(v, missing);
            return;
        }
        if (c == T_PLUS) {
            c = chrget();
            continue;
        }
        break;
    }
    if (c == '"') {
        tp = strlit((uint16_t)(tp + 1), '"', '"');
        return;
    }
    if (c == T_MINUS || c == T_NOT) {
        vpush(9);  // (element's return dropped) the operator and FAC pushed
        eval(P_UNARY);
        vpop(9);
        chknum();
        if (c == T_MINUS)
            fp_neg(&fac);
        else
            fac = fp_from_uint8(fac.exp ? 0 : 1);
        return;
    }
    if (c == T_FN) {
        fn_call();
        return;
    }
    if (c >= T_SGN && c <= T_LAST) {
        chrget();
        if (c == T_SCRN) {
            int x = getbyt();
            if (x >= 48)
                as_error(ERR_ILLEGAL_QUANTITY);
            chkcom();
            int y = getbyt();
            if (y >= 48)
                as_error(ERR_ILLEGAL_QUANTITY);
            fac = fp_from_uint8((uint8_t)gfx_scrn(x, y));
            fac_string = false;
            synchr(')');
            return;
        }
        if (c >= T_LEFTS) {
            string_function(c);
            return;
        }
        // One argument in parentheses.
        vpush(3);
        paren_expr();
        vpop(3);
        switch (c) {
        case T_STRS: {
            chknum();
            char buf[20];
            as_fp f = fac;
            fp_format(&f, buf);
            int n = (int)strlen(buf);
            uint16_t s = getspa((uint8_t)n);
            memcpy(&M[s], buf, n);
            putnew((uint8_t)n, s);
            return;
        }
        case T_CHRS: {
            int v = conint();
            uint16_t s = getspa(1);
            M[s] = (uint8_t)v;
            putnew(1, s);
            return;
        }
        case T_LEN: {
            chkstr();
            uint8_t len;
            fretmp(fac_desc, &len);
            fac = fp_from_uint8(len);
            fac_string = false;
            return;
        }
        case T_ASC: {
            chkstr();
            uint8_t len;
            uint16_t a = fretmp(fac_desc, &len);
            if (!len)
                as_error(ERR_ILLEGAL_QUANTITY);
            fac = fp_from_uint8(M[a]);
            fac_string = false;
            return;
        }
        case T_VAL: {
            chkstr();
            uint8_t len;
            uint16_t a = fretmp(fac_desc, &len);
            fac_string = false;
            if (!len) {
                fac = fp_unpack(zero5);
                return;
            }
            uint16_t save = tp;
            uint16_t end = (uint16_t)(a + len);
            uint8_t keep = M[end];
            M[end] = 0;
            tp = a;
            chrgot();
            as_fin_src src = {
                .peek = (int (*)(void *))(void *)chrgot,
                .next = (void (*)(void *))(void *)chrget,
            };
            fac = fp_parse(&src);
            M[end] = keep;
            tp = save;
            return;
        }
        case T_FRE: {
            if (fac_string) {
                uint8_t len;
                fretmp(fac_desc, &len);
            }
            gc();
            int v = (int16_t)(w16(Z_FRETOP) - w16(Z_STREND));
            fac = fp_from_int16(v);
            fac_string = false;
            return;
        }
        case T_POS:
            {
                extern int text_column(void);
                fac = fp_from_uint8((uint8_t)text_column());
            }
            fac_string = false;
            return;
        default:
            break;
        }
        chknum();
        switch (c) {
        case T_SGN: fac = fp_from_byte((int8_t)fp_sign(&fac)); break;
        case T_INT: fp_int(&fac); break;
        case T_ABS: fac.neg = false; break;
        case T_USR: as_usr(); break;
        case T_PDL: fac = fp_from_uint8((uint8_t)asp_paddle(conint())); break;
        case T_SQR: fp_sqr(&fac); break;
        case T_RND: fp_rnd(&fac, &M[Z_RNDSEED]); break;
        case T_LOG: fp_log(&fac); break;
        case T_EXP: fp_exp(&fac); break;
        case T_COS: fp_cos(&fac); break;
        case T_SIN: fp_sin(&fac); break;
        case T_TAN: fp_tan(&fac); break;
        case T_ATN: fp_atn(&fac); break;
        case T_PEEK: fac = fp_from_uint8(as_peek(fp_to_addr(&fac))); break;
        default: as_error(ERR_SYNTAX);
        }
        fac_string = false;
        return;
    }
    // A parenthesised expression.
    if (c != '(')
        as_error(ERR_SYNTAX);
    paren_expr();
}

/// LEFT$, RIGHT$, MID$: (string, n [, m])
static void string_function(uint8_t t) {
    synchr('(');
    vpush(1);
    frmevl();
    chkcom();
    chkstr();
    uint16_t d = fac_desc;
    vpush(2);
    int n = getbyt();
    int m = 255;
    if (t == T_MIDS && chrgot() != ')') {
        chkcom();
        m = getbyt();
    }
    vpop(3);
    synchr(')');
    if (n == 0)
        as_error(ERR_ILLEGAL_QUANTITY);
    uint8_t len = M[d];
    int start, count;
    if (t == T_LEFTS) {
        start = 0;
        count = n < len ? n : len;
    } else if (t == T_RIGHTS) {
        count = n < len ? n : len;
        start = len - count;
    } else {
        start = n - 1;
        if (start >= len) {
            start = 0;
            count = 0;
        } else {
            count = len - start;
            if (m < count)
                count = m;
        }
    }
    uint16_t s = getspa((uint8_t)count);
    uint8_t l;
    uint16_t a = fretmp(d, &l);
    memmove(&M[s], &M[(uint16_t)(a + start)], count);
    putnew((uint8_t)count, s);
}

// ---- DEF FN ------------------------------------------------------------------------

/// FNC: FN name. FN names are variables with the top bit of the first
/// letter set. Returns the address of its value: the body's address, the
/// parameter's value address, the body's first character.
static uint16_t fn_name(void) {
    synchr(T_FN);
    subflg = 0x80;
    fn_mode = true;
    uint16_t v = ptrget();
    chknum();
    return v;
}

static uint16_t end_of_statement(bool whole_line);

static void stmt_def(void) {
    uint16_t f = fn_name();
    err_direct();
    synchr('(');
    subflg = 0x80;
    uint16_t param = ptrget();
    chknum();
    synchr(')');
    synchr(T_EQUAL);
    uint8_t first = M[tp];
    uint16_t body = tp;
    tp = end_of_statement(false);
    sw16(f, body);
    sw16(f + 2, param);
    M[(uint16_t)(f + 4)] = first;
}

static void fn_call(void) {
    uint16_t f = fn_name();
    vpush(4);
    paren_expr();
    vpop(4);
    chknum();
    uint16_t param = w16(f + 2);
    if (!(param >> 8))
        as_error(ERR_UNDEFD_FUNCTION);
    uint8_t saved[5];
    memcpy(saved, &M[param], 5);
    fp_pack(&fac, &M[param]);
    uint16_t save_tp = tp;
    tp = w16(f);
    vpush(9);
    frmnum();
    vpop(9);
    if (!is_end(chrgot()))
        as_error(ERR_SYNTAX);
    tp = save_tp;
    memcpy(&M[param], saved, 5);
}

// ---- Statements ----------------------------------------------------------------------

/// DATAN/REMN: the offset from TXTPTR to the end of the statement (':' outside
/// quotes, or the end of the line) or, for a whole line, the line's end.
static uint16_t end_of_statement(bool whole_line) {
    uint16_t p = tp;
    bool quote = false;
    for (;;) {
        uint8_t c = M[p];
        if (c == 0)
            return p;
        if (!whole_line && !quote && c == ':')
            return p;
        if (c == '"')
            quote = !quote;
        p++;
    }
}

static void stmt_rem(void) {
    tp = end_of_statement(true);
}

static void stmt_data(void) {
    tp = end_of_statement(false);
}

void stmt_goto(void) {
    linget();
    uint16_t num = w16(Z_LINNUM);
    uint16_t from;
    if ((num >> 8) > M[Z_CURLIN + 1]) {
        uint16_t e = end_of_statement(true);
        from = (uint16_t)(e + 1);
    } else {
        from = w16(Z_TXTTAB);
    }
    uint16_t at;
    if (!fndlin_from(from, num, &at))
        as_error(ERR_UNDEFD_STATEMENT);
    tp = (uint16_t)(at - 1);
}

static void stmt_gosub(void) {
    if (vsp - 4 < 2 * 3 + 0x36)
        as_error(ERR_OUT_OF_MEMORY);
    frame_t *f = push_frame(FR_GOSUB, GOSUB_BYTES);
    f->text = tp;
    f->line = w16(Z_CURLIN);
    chrgot();
    stmt_goto();
}

/// FNDFOR: the FOR record for variable var (0: the innermost), searching
/// down from the top until something that isn't a FOR. Returns its index, or
/// -1; *stop is the index of the first non-FOR record (or -1).
static int fndfor(uint16_t var, int *stop) {
    for (int i = nframes - 1; i >= 0; --i) {
        if (frames[i].type != FR_FOR) {
            if (stop)
                *stop = i;
            return -1;
        }
        if (!var || frames[i].var == var)
            return i;
    }
    if (stop)
        *stop = -1;
    return -1;
}

static void stmt_return(bool pop) {
    if (!is_end(chrgot()))
        return;
    int stop = -1;
    fndfor(0xffff, &stop);
    if (stop < 0 || frames[stop].type != FR_GOSUB)
        as_error(ERR_RETURN_WITHOUT_GOSUB);
    frame_t g = frames[stop];
    pop_frames_to(stop);
    if (pop)
        return;
    sw16(Z_CURLIN, g.line);
    tp = g.text;
    tp = end_of_statement(false);
}

static void stmt_let(void) {
    uint16_t v = ptrget();
    bool is_str = fac_string;
    uint8_t is_int = intflg;
    sw16(Z_FORPNT, v);
    synchr(T_EQUAL);
    vpush(2);
    frmevl();
    vpop(2);
    if (fac_string != is_str)
        as_error(ERR_TYPE_MISMATCH);
    let_store(v, is_str, is_int);
}

static void stmt_for(void) {
    subflg = 0x80;
    uint16_t v = ptrget();
    bool is_str = fac_string;
    sw16(Z_FORPNT, v);
    synchr(T_EQUAL);
    vpush(4);
    frmevl();
    vpop(4);
    if (fac_string != is_str)
        as_error(ERR_TYPE_MISMATCH);
    let_store(v, is_str, 0);
    // An older loop on this variable goes, with everything inside it.
    int i = fndfor(v, NULL);
    if (i >= 0)
        pop_frames_to(i);
    if (vsp - 2 < 2 * 9 + 0x36)
        as_error(ERR_OUT_OF_MEMORY);
    uint16_t end = end_of_statement(false);
    uint16_t line = w16(Z_CURLIN);
    synchr(T_TO);
    if (is_str)
        as_error(ERR_TYPE_MISMATCH);
    vs = vsp - 4;  // the statement's return dropped; text and line pushed
    frmnum();
    as_fp limit = fac;
    fp_round(&limit);
    as_packed lim;
    lim[0] = limit.exp;
    lim[1] = (uint8_t)((limit.mant >> 24) & (limit.neg ? 0xff : 0x7f));
    lim[2] = (uint8_t)(limit.mant >> 16);
    lim[3] = (uint8_t)(limit.mant >> 8);
    lim[4] = (uint8_t)limit.mant;
    static const as_packed one = {0x81, 0, 0, 0, 0};
    fac = fp_unpack(one);
    if (chrgot() == T_STEP) {
        chrget();
        vs = vsp - 11;
        frmnum();
    }
    int8_t sign = (int8_t)fp_sign(&fac);
    as_fp step = fac;
    fp_round(&step);
    frame_t *f = push_frame(FR_FOR, FOR_BYTES);
    f->var = v;
    f->line = line;
    f->text = end;
    memcpy(f->limit, lim, 5);
    f->step = step;
    f->step_sign = sign;
}

static void stmt_next(void) {
    uint16_t var = 0;
    for (;;) {
        if (!is_end(chrgot()))
            var = ptrget();
        else
            var = 0;
        int i = fndfor(var, NULL);
        if (i < 0)
            as_error(ERR_NEXT_WITHOUT_FOR);
        pop_frames_to(i + 1);
        frame_t *f = &frames[i];
        // var = var + step, rounded into the variable, then compared.
        fac = f->step;
        fac.ext = 0;
        fac.neg = f->step_sign < 0;
        as_fp a = fp_unpack(&M[f->var]);
        fp_add(&fac, &a);
        fp_pack(&fac, &M[f->var]);
        int cmp = fp_compare(&fac, f->limit);
        if (cmp - f->step_sign != 0) {
            sw16(Z_CURLIN, f->line);
            tp = f->text;
            return;
        }
        pop_frames_to(i);
        if (chrgot() != ',')
            return;
        chrget();
    }
}

static void stmt_if(void) {
    frmevl();
    uint8_t c = chrgot();
    if (c != T_GOTO)
        synchr(T_THEN);
    if (!fac.exp) {
        tp = end_of_statement(true);
        return;
    }
    c = chrgot();
    if (is_digit(c)) {
        stmt_goto();
        return;
    }
    extern void execute(uint8_t c);
    execute(c);
}

static void stmt_on(void) {
    int v = getbyt();
    uint8_t t = chrgot();
    if (t != T_GOSUB && t != T_GOTO)
        as_error(ERR_SYNTAX);
    for (;;) {
        if (--v == 0) {
            chrget();
            if (t == T_GOSUB)
                stmt_gosub();
            else
                stmt_goto();
            return;
        }
        chrget();
        linget();
        if (chrgot() != ',')
            return;
    }
}

static void stmt_restore(void) {
    sw16(Z_DATPTR, w16(Z_TXTTAB) - 1);
}

static void restore_data(void) {
    stmt_restore();
}

static void clearc(void) {
    sw16(Z_FRETOP, w16(Z_MEMSIZ));
    sw16(Z_ARYTAB, w16(Z_VARTAB));
    sw16(Z_STREND, w16(Z_VARTAB));
    restore_data();
    stkini();
    M[Z_LOCK] = M[Z_LOCK];
}

void as_clear(void) {
    clearc();
}

void as_new_program(void) {
    M[Z_LOCK] = 0;
    uint16_t t = w16(Z_TXTTAB);
    M[(uint16_t)(t - 1)] = 0;  // the program must follow a zero
    M[t] = 0;
    M[t + 1] = 0;
    sw16(Z_VARTAB, t + 2);
    sw16(Z_PRGEND, t + 2);
    tp = (uint16_t)(t - 1);
    clearc();
}

void do_break(void);

static void stop_or_end(bool stop, bool save) {
    if (save && as_running()) {
        sw16(Z_OLDTEXT, tp);
        sw16(Z_OLDLIN, w16(Z_CURLIN));
    }
    if (stop) {
        as_print("\rBREAK");
        as_outdo(7);
        if (as_running()) {
            as_print(" IN ");
            print_uint(w16(Z_CURLIN));
        }
    }
    longjmp(restart_jmp, 2);
}

void do_break(void) {
    if (M[Z_ERRFLG] & 0x80)
        onerr_handler(ERR_BREAK);
    stop_or_end(true, true);
}

static void stmt_cont(void) {
    if (!is_end(chrgot()))
        return;
    if (!M[Z_OLDTEXT + 1])
        as_error(ERR_CANT_CONTINUE);
    tp = w16(Z_OLDTEXT);
    sw16(Z_CURLIN, w16(Z_OLDLIN));
}

void as_run_program(void) {
    M[Z_CURLIN + 1]--;
    tp = (uint16_t)(w16(Z_TXTTAB) - 1);
    clearc();
}

static void stmt_run(void) {
    uint8_t c = chrgot();
    if (!is_end(c) && !is_digit(c)) {
        // RUN name: DOS
        char name[64];
        int n = 0;
        while (!is_end(M[tp]) && n < 63) {
            uint8_t b = M[tp++];
            if (b >= 0x80) {
                const char *k = b <= T_LAST ? keywords[b - 0x80] : "";
                while (*k && n < 63)
                    name[n++] = *k++;
            } else {
                name[n++] = (char)b;
            }
        }
        name[n] = 0;
        char cmd[80];
        snprintf(cmd, sizeof cmd, "RUN %s", name);
        dos_command(cmd, true);
        return;
    }
    M[Z_CURLIN + 1]--;
    if (is_end(c)) {
        tp = (uint16_t)(w16(Z_TXTTAB) - 1);
        clearc();
        return;
    }
    clearc();
    stmt_goto();
}

static void stmt_dim(void) {
    for (;;) {
        dim_request = M[tp] ? M[tp] : 1;
        ptrget();
        if (is_end(chrgot()))
            return;
        chkcom();
    }
}

static void stmt_poke(void) {
    uint16_t a = getadr();
    chkcom();
    int v = getbyt();
    as_poke(a, (uint8_t)v);
}

static void stmt_wait(void) {
    uint16_t a = getadr();
    chkcom();
    int mask = getbyt();
    int x = 0;
    if (chrgot() == ',') {
        chrget();
        x = getbyt();
    }
    while (!(((as_peek(a) ^ x) & mask))) {
        if (as_kbd_poll_break()) {
            as_rdkey();
            do_break();
        }
        video_update(false);
    }
}

static void stmt_himem(void) {
    uint16_t a = getadr();
    if (a < w16(Z_STREND))
        as_error(ERR_OUT_OF_MEMORY);
    sw16(Z_MEMSIZ, a);
    sw16(Z_FRETOP, a);
}

static void stmt_lomem(void) {
    uint16_t a = getadr();
    if (a >= w16(Z_MEMSIZ) || a < w16(Z_VARTAB))
        as_error(ERR_OUT_OF_MEMORY);
    sw16(Z_VARTAB, a);
    clearc();
}

static void stmt_onerr(void) {
    synchr(T_GOTO);
    sw16(Z_TXTPSV, tp);
    M[Z_ERRFLG] = (uint8_t)((M[Z_ERRFLG] >> 1) | 0x80);
    sw16(Z_CURLSV, w16(Z_CURLIN));
    tp = end_of_statement(true);
}

static void stmt_resume(void) {
    sw16(Z_CURLIN, w16(Z_ERRLIN));
    tp = w16(Z_ERRPOS);
    // S = ERRSTK: drop whatever is above it.
    while (nframes && vsp < M[Z_ERRSTK])
        vsp += frames[--nframes].bytes;
}

void as_stack_init(void) {
    // STKINI from machine code: the stack (FOR and GOSUB too) starts afresh.
    M[Z_TEMPPT] = Z_TEMPST;
    stack_reset();
    vs = vsp;
}

uint16_t as_ptrget(void) {
    return ptrget();
}

void as_skip_statement(bool whole_line) {
    tp = end_of_statement(whole_line);
}

void as_restore_errstk(void) {
    while (nframes && vsp < M[Z_ERRSTK])
        vsp += frames[--nframes].bytes;
}

static void stmt_del(void) {
    if (!is_digit(chrgot()))
        as_error(ERR_SYNTAX);
    sw16(Z_VARTAB, w16(Z_PRGEND));
    linget();
    uint16_t from;
    fndlin(w16(Z_LINNUM), &from);
    chkcom();
    linget();
    sw16(Z_LINNUM, w16(Z_LINNUM) + 1);
    uint16_t to;
    fndlin(w16(Z_LINNUM), &to);
    if (to < from)
        return;
    uint16_t end = w16(Z_VARTAB);
    move_block(to, end + 1, -(int)(to - from));
    sw16(Z_VARTAB, end - (to - from));
    clearc();
    as_fix_links();
    // DEL ends like a line entry: back at the prompt.
    longjmp(restart_jmp, 2);
}

// ---- PRINT, INPUT, READ, GET ---------------------------------------------------

static void strprt(uint16_t d) {
    uint8_t len;
    uint16_t a = fretmp(d, &len);
    for (int i = 0; i < len; ++i)
        as_outdo(M[(uint16_t)(a + i)]);
}

static void stmt_print(void) {
    extern int text_column(void);
    extern int text_width_for_tab(void);
    uint8_t c = chrgot();
    if (is_end(c)) {
        as_crdo();
        return;
    }
    for (;;) {
        c = chrgot();
        if (is_end(c))
            return;
        if (c == T_TAB || c == T_SPC) {
            int n = gtbytc();
            if (chrgot() != ')')
                as_error(ERR_SYNTAX);
            if (c == T_SPC) {
                for (int i = 0; i < n; ++i)
                    as_outdo(' ');
            } else {
                // TAB(n): out to column n-1 (8-bit, so TAB(0) goes to 255).
                int col = text_column();
                uint8_t target = (uint8_t)(n - 1);
                if (target >= col)
                    for (int i = col; i < target; ++i)
                        as_outdo(' ');
            }
        } else if (c == ',') {
            extern bool text_80col(void);
            int col = text_column();
            if (!text_80col() && col >= 24) {
                as_crdo();
            } else {
                uint8_t target = (uint8_t)((col + 16) & 0xf0);
                if (target >= col)
                    for (int i = col; i < target; ++i)
                        as_outdo(' ');
            }
        } else if (c == ';') {
            ;
        } else {
            frmevl();
            if (!fac_string) {
                char buf[20];
                fp_format(&fac, buf);
                as_print(buf);
            } else {
                strprt(fac_desc);
            }
            c = chrgot();
            if (is_end(c)) {
                as_crdo();
                return;
            }
            continue;
        }
        chrget();
    }
}

/// Mode of the shared INPUT/READ/GET code ($15).
enum { MODE_INPUT = 0x00, MODE_GET = 0x40, MODE_READ = 0x98 };

static void bad_input(int mode) {
    if (mode == MODE_INPUT) {
        if (M[Z_ERRFLG] & 0x80)
            as_error(ERR_REENTER);
        as_print("?REENTER\r");
        tp = w16(Z_OLDTEXT);
        longjmp(newstt_jmp, 1);
    }
    if (mode == MODE_READ)
        sw16(Z_CURLIN, w16(Z_DATLIN));
    else
        sw16(Z_CURLIN, 0xffff);
    as_error(ERR_SYNTAX);
}

static void read_items(int mode, uint16_t input_ptr) {
    sw16(Z_INPTR, input_ptr);
    for (;;) {
        uint16_t v = ptrget();
        bool is_str = fac_string;
        uint8_t is_int = intflg;
        sw16(Z_FORPNT, v);
        uint16_t prog = tp;
        tp = w16(Z_INPTR);
        uint8_t c = chrgot();
        if (is_end(c)) {
            if (mode == MODE_GET) {
                uint8_t k = as_rdkey() & 0x7f;
                M[0x200] = k;
                M[0x201] = 0;
                tp = 0x1ff;
            } else if (mode == MODE_READ) {
                // On to the next DATA statement.
                for (;;) {
                    uint16_t e = end_of_statement(false);
                    uint16_t y = (uint16_t)(e - tp + 1);
                    if (M[e] == 0) {
                        if (M[(uint16_t)(tp + y + 1)] == 0)
                            as_error(ERR_OUT_OF_DATA);
                        sw16(Z_DATLIN, w16(tp + y + 2));
                        y += 4;
                    }
                    uint8_t t = M[(uint16_t)(tp + y)];
                    tp += y;
                    if (t == T_DATA)
                        break;
                }
            } else {
                if (!dos_reading()) {  // DOS keeps the prompts off when reading a file
                    as_outdo('?');
                    as_outdo('?');
                }
                as_inlin(0x80);
                tp = 0x1ff;
            }
        }
        chrget();
        if (is_str) {
            uint16_t start;
            uint8_t t1, t2;
            if (mode == MODE_GET) {
                tp = 0x200;
                start = tp;
                t1 = t2 = 0;
            } else {
                c = M[tp];
                if (c == '"') {
                    t1 = t2 = '"';
                    start = (uint16_t)(tp + 1);
                } else {
                    t1 = ':';
                    t2 = ',';
                    start = tp;
                }
            }
            uint16_t after = strlit(start, t1, t2);
            tp = after;
            let_store(v, true, 0);
        } else {
            if (mode == MODE_INPUT && M[0x200] == 0)
                bad_input(mode);
            as_fin_src src = {
                .peek = (int (*)(void *))(void *)chrgot,
                .next = (void (*)(void *))(void *)chrget,
            };
            fac = fp_parse(&src);
            fac_string = false;
            let_store(v, false, is_int);
        }
        c = chrgot();
        if (!is_end(c) && c != ',')
            bad_input(mode);
        sw16(Z_INPTR, tp);
        tp = prog;
        if (is_end(chrgot()))
            break;
        chkcom();
    }
    if (mode == MODE_READ) {
        sw16(Z_DATPTR, w16(Z_INPTR));
    } else if (M[w16(Z_INPTR)] != 0) {
        as_print("?EXTRA IGNORED\r");
    }
}

static void stmt_input(void) {
    uint8_t c = chrgot();
    if (c == '"') {
        tp = strlit((uint16_t)(tp + 1), '"', '"');
        synchr(';');
        strprt(fac_desc);
    } else if (!dos_reading()) {
        as_outdo('?');
    }
    err_direct();
    M[0x1ff] = ',';
    as_inlin(0x80);
    if (M[0x200] == 3) {
        do_break();
    }
    read_items(MODE_INPUT, 0x1ff);
}

static void stmt_read(void) {
    read_items(MODE_READ, w16(Z_DATPTR));
}

static void stmt_get(void) {
    err_direct();
    M[0x201] = 0;
    read_items(MODE_GET, 0x201);
}

// ---- Graphics and screen statements (the work is in as_gfx/as_text) ------------

static int byte_xy(int limit) {
    int v = getbyt();
    if (v >= limit)
        as_error(ERR_ILLEGAL_QUANTITY);
    return v;
}

static void stmt_plot(void) {
    int x = byte_xy(48);
    chkcom();
    int y = byte_xy(48);
    if (x >= 40)
        as_error(ERR_ILLEGAL_QUANTITY);
    gfx_plot(x, y);
}

static void stmt_hlin_vlin(bool h) {
    int a = byte_xy(48);
    chkcom();
    int b = byte_xy(48);
    synchr(T_AT);
    int c = byte_xy(48);
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    if (h) {
        if (b >= 40)
            as_error(ERR_ILLEGAL_QUANTITY);
        gfx_hlin(a, b, c);
    } else {
        if (c >= 40)
            as_error(ERR_ILLEGAL_QUANTITY);
        gfx_vlin(a, b, c);
    }
}

static void hplot_coords(int *x, int *y) {
    *x = getadr();
    chkcom();
    *y = getbyt();
    if (*x >= 280 || *y >= 192)
        as_error(ERR_ILLEGAL_QUANTITY);
}

static void stmt_hplot(void) {
    int x, y;
    if (chrgot() != T_TO) {
        hplot_coords(&x, &y);
        gfx_hplot(x, y);
    }
    while (chrgot() == T_TO) {
        chrget();
        hplot_coords(&x, &y);
        gfx_hplot_to(x, y);
    }
}

static void stmt_draw(bool xdraw) {
    int shape = getbyt();
    bool at = false;
    int x = 0, y = 0;
    if (chrgot() == T_AT) {
        chrget();
        hplot_coords(&x, &y);
        at = true;
    }
    gfx_draw(shape, xdraw, at, x, y);
}

static void stmt_htab(void) {
    extern void text_htab(int n);
    text_htab(getbyt());
}

static void stmt_vtab(void) {
    int n = getbyt();
    if (n < 1 || n > 24)
        as_error(ERR_ILLEGAL_QUANTITY);
    M[Z_CV] = (uint8_t)(n - 1);
    as_vtab();
}

// ---- The statement loop -------------------------------------------------------------

static void sync_txtptr_and_call(void (*fn)(void)) {
    as_sync_out();
    fn();
    as_sync_in();
}

void execute(uint8_t c) {
    vs = vsp - 2;  // NEWSTT's JSR to the statement
    if (is_end(c))
        return;
    if (c < 0x80) {
        stmt_let();
        return;
    }
    if (c >= 0xc0)
        as_error(ERR_SYNTAX);
    chrget();
    switch (c) {
    case T_END: if (is_end(chrgot())) stop_or_end(false, true); return;
    case T_STOP: if (is_end(chrgot())) stop_or_end(true, true); return;
    case T_FOR: stmt_for(); return;
    case T_NEXT: stmt_next(); return;
    case T_DATA: stmt_data(); return;
    case T_INPUT: stmt_input(); return;
    case T_DEL: stmt_del(); return;
    case T_DIM: stmt_dim(); return;
    case T_READ: stmt_read(); return;
    case T_GR: gfx_gr(); return;
    case T_TEXT: as_text_mode(); return;
    case T_PRNUM: as_pr(getbyt()); return;
    case T_INNUM: as_in(getbyt()); return;
    case T_CALL: as_call(getadr()); return;
    case T_PLOT: stmt_plot(); return;
    case T_HLIN: stmt_hlin_vlin(true); return;
    case T_VLIN: stmt_hlin_vlin(false); return;
    case T_HGR2: gfx_hgr(2); return;
    case T_HGR: gfx_hgr(1); return;
    case T_HCOLOR: {
        int v = getbyt();
        if (v > 7)
            as_error(ERR_ILLEGAL_QUANTITY);
        gfx_hcolor(v);
        return;
    }
    case T_HPLOT: stmt_hplot(); return;
    case T_DRAW: stmt_draw(false); return;
    case T_XDRAW: stmt_draw(true); return;
    case T_HTAB: stmt_htab(); return;
    case T_HOME: as_home(); return;
    case T_ROT: M[Z_ROT] = (uint8_t)getbyt(); return;
    case T_SCALE: M[Z_SCALE] = (uint8_t)getbyt(); return;
    case T_SHLOAD: as_error(ERR_SYNTAX);
    case T_TRACE: M[Z_TRCFLG] = (uint8_t)((M[Z_TRCFLG] >> 1) | 0x80); return;
    case T_NOTRACE: M[Z_TRCFLG] >>= 1; return;
    case T_NORMAL: M[Z_INVFLG] = 0xff; M[Z_FLASH] = 0; return;
    case T_INVERSE: M[Z_INVFLG] = 0x3f; M[Z_FLASH] = 0; return;
    case T_FLASH: M[Z_INVFLG] = 0x7f; M[Z_FLASH] = 0x40; return;
    case T_COLOR: gfx_set_color(getbyt()); return;
    case T_POP: stmt_return(true); return;
    case T_VTAB: stmt_vtab(); return;
    case T_HIMEM: stmt_himem(); return;
    case T_LOMEM: stmt_lomem(); return;
    case T_ONERR: stmt_onerr(); return;
    case T_RESUME: stmt_resume(); return;
    case T_RECALL: case T_STORE: as_error(ERR_SYNTAX);
    case T_SPEED: M[Z_SPEED] = (uint8_t)(-getbyt()); return;
    case T_LET: stmt_let(); return;
    case T_GOTO: stmt_goto(); return;
    case T_RUN: stmt_run(); return;
    case T_IF: stmt_if(); return;
    case T_RESTORE: stmt_restore(); return;
    case T_AMPER: sync_txtptr_and_call(as_ampersand); return;
    case T_GOSUB: stmt_gosub(); return;
    case T_RETURN: stmt_return(false); return;
    case T_REM: stmt_rem(); return;
    case T_ON: stmt_on(); return;
    case T_WAIT: stmt_wait(); return;
    case T_LOAD: case T_SAVE: {
        char name[64];
        int n = 0;
        while (!is_end(M[tp]) && n < 60) {
            uint8_t b = M[tp++];
            if (b >= 0x80) {
                const char *k = b <= T_LAST ? keywords[b - 0x80] : "";
                while (*k && n < 60)
                    name[n++] = *k++;
            } else {
                name[n++] = (char)b;
            }
        }
        name[n] = 0;
        char cmd[80];
        snprintf(cmd, sizeof cmd, "%s %s", c == T_LOAD ? "LOAD" : "SAVE", name);
        dos_command(cmd, true);
        return;
    }
    case T_DEF: stmt_def(); return;
    case T_POKE: stmt_poke(); return;
    case T_PRINT: stmt_print(); return;
    case T_CONT: stmt_cont(); return;
    case T_LIST: stmt_list(); return;
    case T_CLEAR: if (is_end(chrgot())) clearc(); return;
    case T_GET: stmt_get(); return;
    case T_NEW:
        if (is_end(chrgot()))
            as_new_program();
        return;
    default:
        as_error(ERR_SYNTAX);
    }
}

void as_set_trace(bool on) {
    M[Z_TRCFLG] = on ? 0x80 : 0;
}

static unsigned poll_count;

/// NEWSTT: runs statements until the program (or direct line) ends.
static void newstt(bool first) {
    for (;;) {
        if (!first) {
            M[Z_REMSTK] = vsp;
            if (++poll_count >= 64) {
                poll_count = 0;
                if (as_kbd_poll_break()) {
                    as_rdkey();
                    do_break();
                }
                video_update(false);
            }
            if (as_running())
                sw16(Z_OLDTEXT, tp);
            uint8_t c = M[tp];
            if (c == 0) {
                if (M[(uint16_t)(tp + 2)] == 0)
                    stop_or_end(false, false);
                sw16(Z_CURLIN, w16(tp + 3));
                tp += 4;
            } else if (c != ':') {
                as_error(ERR_SYNTAX);
            }
        }
        first = false;
        if ((M[Z_TRCFLG] & 0x80) && as_running()) {
            as_outdo('#');
            print_uint(w16(Z_CURLIN));
            as_outdo(' ');
        }
        uint8_t c = chrget();
        execute(c);
    }
}

/// Runs from TXTPTR (a statement boundary) until something jumps back to the
/// prompt. ONERR, RESUME and ?REENTER come back here to carry on.
static void run_from(bool first) {
    newstt_jmp_valid = true;
    if (setjmp(newstt_jmp))
        first = false;
    newstt(first);
}

void as_enter_line(const char *text) {
    int n = (int)strlen(text);
    if (n > 239)
        n = 239;
    memcpy(&M[0x200], text, n);
    M[0x200 + n] = 0;
    for (int i = 0; i < n; ++i)
        M[0x200 + i] &= 0x7f;
    tp = 0x1ff;
    uint8_t c = chrget();
    if (!c)
        return;
    if (is_digit(c)) {
        linget();
        // The rest of the line, crunched, goes in at $200.
        int skip = tp - 0x200;
        memmove(&M[0x200], &M[tp], 256 - skip);
        insert_line(as_crunch());
    }
}

static void prompt_loop(void) {
    setjmp(restart_jmp);
    newstt_jmp_valid = false;
    for (;;) {
        as_crdo();
        as_inlin(']');
        tp = 0x1ff;
        M[Z_ERRFLG] >>= 1;
        uint8_t c = chrget();
        if (c == 0)
            continue;
        M[Z_CURLIN + 1] = 0xff;
        if (dos_command((const char *)&M[0x200], false))
            continue;
        if (is_digit(c)) {
            linget();
            int skip = tp - 0x200;
            memmove(&M[0x200], &M[tp], 256 - skip);
            insert_line(as_crunch());
            continue;
        }
        int skip = tp - 0x200;
        memmove(&M[0x200], &M[tp], 256 - skip);
        as_crunch();
        tp = 0x1ff;
        run_from(true);
    }
}

static void cold_start(void) {
    memset(M, 0, 0x10000);
    fac = fp_unpack(zero5);
    fac_string = false;
    // The ROM's CHRGET, which machine code calls and some programs patch.
    static const uint8_t chrget_code[] = {
        0xe6, 0xb8, 0xd0, 0x02, 0xe6, 0xb9, 0xad, 0x00, 0x08, 0xc9, 0x3a, 0xb0,
        0x0a, 0xc9, 0x20, 0xf0, 0xef, 0x38, 0xe9, 0x30, 0x38, 0xe9, 0xd0, 0x60,
    };
    memcpy(&M[Z_CHRGET], chrget_code, sizeof chrget_code);
    static const uint8_t seed[5] = {0x80, 0x4f, 0xc7, 0x52, 0x00};
    memcpy(&M[Z_RNDSEED], seed, 5);
    M[0x0a] = 0x4c;  // USR's JMP, to ILLEGAL QUANTITY until set
    sw16(0x0b, 0xe199);
    M[0x3f5] = 0x4c;  // & vector
    sw16(0x3f6, 0xff58);
    M[Z_INVFLG] = 0xff;
    M[Z_SPEED] = 0;
    M[Z_HCOLOR1] = 0;
    M[Z_SCALE] = 1;
    sw16(Z_TXTTAB, 0x801);
    sw16(Z_MEMSIZ, 0x9600);
    M[0x800] = 0;
    M[Z_TEMPPT] = Z_TEMPST;
    as_new_program();
}

void applesoft_main(const char *file) {
    M = asp_alloc(0x10000);
    if (!M) {
        printf("Applesoft needs 64 KB of free PSRAM\n");
        return;
    }
    gc_refs = asp_alloc(sizeof(gc_ref) * GC_MAX_REFS);
    as_fp_error = fp_error_hook;
    asp_screen_begin();
    cold_start();
    mach_init();
    video_init();
    text_init();
    dos_init();
    if (!setjmp(quit_jmp)) {
        if (file) {
            if (!setjmp(restart_jmp)) {
                char cmd[80];
                snprintf(cmd, sizeof cmd, "RUN %s", file);
                M[Z_CURLIN + 1] = 0xff;
                dos_command(cmd, false);
            }
        }
        prompt_loop();
    }
    dos_close_all();
    video_cursor(false, 0, 0);
    video_update(true);
    if (applesoft_on_exit)
        applesoft_on_exit(M);
    asp_screen_end();
    asp_free(gc_refs);
    gc_refs = NULL;
    asp_free(M);
    M = NULL;
}
