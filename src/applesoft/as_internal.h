/**
 * @file as_internal.h
 * @brief Shared by the Applesoft interpreter's parts.
 *
 * the emulated memory, the zero-page locations the ROM uses (kept where the
 * ROM keeps them, so PEEK, POKE and machine code see what they expect), and
 * the calls between parts.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "as_float.h"

/// The 64 KB the program sees. Main memory only; the I/O page and ROM area
/// are handled by as_peek/as_poke.
extern uint8_t *M;

/// Zero page, by the ROM's names.
enum {
    Z_WNDLFT = 0x20, Z_WNDWDTH = 0x21, Z_WNDTOP = 0x22, Z_WNDBTM = 0x23,
    Z_CH = 0x24, Z_CV = 0x25, Z_GBASL = 0x26, Z_GBASH = 0x27, Z_BASL = 0x28, Z_BASH = 0x29,
    Z_H2 = 0x2c, Z_V2 = 0x2d, Z_MASK = 0x2e, Z_COLOR = 0x30, Z_INVFLG = 0x32, Z_PROMPT = 0x33,
    Z_YSAV1 = 0x35, Z_CSWL = 0x36, Z_KSWL = 0x38, Z_A1L = 0x3c, Z_A2L = 0x3e,
    Z_RNDL = 0x4e, Z_RNDH = 0x4f,
    Z_LINNUM = 0x50, Z_TEMPPT = 0x52, Z_LASTPT = 0x53, Z_TEMPST = 0x55,
    Z_TXTTAB = 0x67, Z_VARTAB = 0x69, Z_ARYTAB = 0x6b, Z_STREND = 0x6d, Z_FRETOP = 0x6f,
    Z_FRESPC = 0x71, Z_MEMSIZ = 0x73, Z_CURLIN = 0x75, Z_OLDLIN = 0x77, Z_OLDTEXT = 0x79,
    Z_DATLIN = 0x7b, Z_DATPTR = 0x7d, Z_INPTR = 0x7f, Z_VARNAM = 0x81, Z_VARPNT = 0x83,
    Z_FORPNT = 0x85,
    Z_FAC = 0x9d, Z_ARG = 0xa5, Z_PRGEND = 0xaf, Z_CHRGET = 0xb1, Z_CHRGOT = 0xb7,
    Z_TXTPTR = 0xb8, Z_RNDSEED = 0xc9, Z_LOCK = 0xd6,
    Z_ERRFLG = 0xd8, Z_ERRLIN = 0xda, Z_ERRPOS = 0xdc, Z_ERRNUM = 0xde, Z_ERRSTK = 0xdf,
    Z_HGRX = 0xe0, Z_HGRY = 0xe2, Z_HCOLOR1 = 0xe4, Z_HPAG = 0xe6, Z_SCALE = 0xe7,
    Z_HSHAPE = 0xe8, Z_FIRST = 0xf0, Z_SPEED = 0xf1, Z_TRCFLG = 0xf2, Z_FLASH = 0xf3,
    Z_TXTPSV = 0xf4, Z_CURLSV = 0xf6, Z_REMSTK = 0xf8, Z_ROT = 0xf9,
};

/// Reads a 16-bit little-endian word from emulated memory.
static inline uint16_t w16(uint16_t a) {
    return M[a] | M[(uint16_t)(a + 1)] << 8;
}

/// Writes a 16-bit little-endian word to emulated memory.
static inline void sw16(uint16_t a, uint16_t v) {
    M[a] = (uint8_t)v;
    M[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}

/// Applesoft error codes (offsets into the ROM's message table, as PEEK(222)
/// shows them).
enum {
    ERR_NEXT_WITHOUT_FOR = 0, ERR_SYNTAX = 16, ERR_RETURN_WITHOUT_GOSUB = 22,
    ERR_OUT_OF_DATA = 42, ERR_ILLEGAL_QUANTITY = 53, ERR_OVERFLOW = 69,
    ERR_OUT_OF_MEMORY = 77, ERR_UNDEFD_STATEMENT = 90, ERR_BAD_SUBSCRIPT = 107,
    ERR_REDIMD_ARRAY = 120, ERR_DIVISION_BY_ZERO = 133, ERR_ILLEGAL_DIRECT = 149,
    ERR_TYPE_MISMATCH = 163, ERR_STRING_TOO_LONG = 176, ERR_FORMULA_TOO_COMPLEX = 191,
    ERR_CANT_CONTINUE = 210, ERR_UNDEFD_FUNCTION = 224, ERR_REENTER = 254, ERR_BREAK = 255,
    /// Not Applesoft's: DOS errors, reported in the DOS 3.3 style, and a call
    /// into a ROM routine we don't have.
    ERR_DOS = 0x100,
    ERR_UNSUPPORTED = 0x200,
};

/// Raises an Applesoft error (ONERR GOTO can catch it). Doesn't return.
void as_error(int code) __attribute__((noreturn));
/// A DOS 3.3 error ("FILE NOT FOUND", code 6 and so on). Doesn't return.
void as_dos_error(int code) __attribute__((noreturn));
/// Leaves the interpreter (BYE).
void as_quit(void) __attribute__((noreturn));
/// Back to the ] prompt (after LOAD from a program).
void as_quit_to_prompt(void) __attribute__((noreturn));
/// Runs the program now in memory (DOS RUN), from wherever we are.
void as_start_run(void) __attribute__((noreturn));
/// Loads program `name` and runs it, keeping the variables (CHAIN); doesn't return.
void as_chain(const char *name) __attribute__((noreturn));
/// An error ends DOS READ/WRITE redirection.
void as_close_dos_io(void);

// ---- as_text.c: the text screen, keyboard and line input -----------------------

/// Sets up the text screen and its window.
void text_init(void);
/// Character output as Applesoft's OUTDO and the monitor's COUT do it: c with
/// or without its high bit. DOS commands (CHR$(4)) are caught here.
void as_outdo(uint8_t c);
void as_cout(uint8_t c);  ///< COUT: goes through the output hook (PR#)
/// Prints a carriage return (Applesoft's CRDO).
void as_crdo(void);
/// Clears the text window and homes the cursor (HOME).
void as_home(void);
/// Clears to the end of the line (CLREOL).
void as_clreol(void);
/// Clears to the end of the text window (CLREOP).
void as_clreop(void);
void as_vtab(void);       ///< recompute BASL from CV
void as_text_mode(void);  ///< TEXT: full window, text display
/// Rings the bell.
void as_bell(void);
/// Keyboard: the key latched at $C000 (bit 7 set while unread), polling for
/// one if none.
uint8_t as_kbd(void);
/// Clears the keyboard strobe, as reading $C010 does.
void as_kbd_strobe(void);
bool as_kbd_poll_break(void);  ///< true if Ctrl-C is waiting (and leaves it)
uint8_t as_rdkey(void);        ///< waits for a key (cursor shown); bit 7 set
/// GETLN: reads a line into $200 after printing the prompt at $33; returns its
/// length. Characters are stored with bit 7 set, as the monitor does.
int as_getln(void);
/// INLIN: GETLN, then the line cut to 239 characters, NUL-terminated, with the
/// high bits stripped. Returns the length.
int as_inlin(uint8_t prompt);
/// Output hooks: DOS 3.3 style command capture and PR#/IN#.
void as_pr(int slot);
/// Takes input from slot `slot` (IN#).
void as_in(int slot);
/// Keys fed in from a file (EXEC) come before the keyboard's.
void as_exec_feed(void);

// ---- as_gfx.c: low- and high-resolution graphics -------------------------------

/// Switches to the lo-res graphics screen (GR).
void gfx_gr(void);
/// Plots a lo-res block in the current colour (PLOT).
void gfx_plot(int x, int y);
/// Draws a horizontal lo-res line (HLIN x1,x2 AT y).
void gfx_hlin(int x1, int x2, int y);
/// Draws a vertical lo-res line (VLIN y1,y2 AT x).
void gfx_vlin(int y1, int y2, int x);
/// The lo-res colour at x, y (SCRN).
int gfx_scrn(int x, int y);
/// Sets the lo-res colour (COLOR=).
void gfx_set_color(int c);
void gfx_hgr(int page);  ///< 1 or 2
/// Sets the hi-res colour (HCOLOR=).
void gfx_hcolor(int c);
void gfx_hplot_to(int x, int y);   ///< a line from the last point
void gfx_hplot(int x, int y);      ///< a point (and sets the last point)
void gfx_hposn(int x, int y);      ///< just sets the position
/// Draws (or with xdraw, XDRAWs) a shape from the shape table, optionally AT x, y.
void gfx_draw(int shape, bool xdraw, bool at, int x, int y);

// ---- as_video.c: drawing the Apple's display into the framebuffer ---------------

/// The soft switches that pick what is shown.
extern struct as_video {
    /// the soft switches: text, mixed, page 2, hi-res, 80 columns, alternate characters, 80STORE, double hi-res
    ///@{
    bool text, mixed, page2, hires, col80, altchar, store80, dhires;
    ///@}
} vid;
/// Sets up the video emulation.
void video_init(void);
void video_mark_all(void);              ///< redraw everything next time
void video_mark(uint16_t addr);         ///< a byte of screen memory changed
void video_update(bool force);          ///< redraw what changed (rate limited)
/// Shows or hides the text cursor at row, col.
void video_cursor(bool on, int row, int col);

// ---- as_mach.c: memory-mapped I/O, PEEK/POKE, CALL, USR and & ------------------

/// Reads emulated memory, soft switches included (PEEK).
uint8_t as_peek(uint16_t addr);
/// Writes emulated memory, soft switches included (POKE).
void as_poke(uint16_t addr, uint8_t v);
/// Runs machine code at `addr` on the 65C02 (CALL).
void as_call(uint16_t addr);
/// Calls the USR function's machine code.
void as_usr(void);
/// Runs the & command's machine code.
void as_ampersand(void);
/// Sets up the emulated machine (memory and 65C02).
void mach_init(void);
/// Soft-switch access from the interpreter itself (TEXT, GR, HGR...).
uint8_t as_softswitch(uint16_t addr, bool write, uint8_t v);
/// The speaker clicks; on the Apple each touch of $C030 flips the cone.
void as_click(void);
/// The time base, in microseconds; the 6502 (CALL) keeps its own as cycles.
uint64_t as_now_us(void);

// ---- as_dos.c: DOS 3.3 style commands and files on the drive -------------------

/// Sets up the DOS 3.3 style file commands.
void dos_init(void);
/// Tries a line typed at the ] prompt or printed after CHR$(4) as a DOS
/// command; true if it was one (and has been done).
bool dos_command(const char *line, bool from_program);
/// Characters printed while a file is open for WRITE go to it; true if taken.
bool dos_output(uint8_t c);
/// INPUT and GET while a file is open for READ read it; -1 if not.
int dos_input_char(void);
/// True while a DOS text file is open for reading (input comes from it).
bool dos_reading(void);
/// Closes all DOS files.
void dos_close_all(void);
/// RUN/LOAD/SAVE of the program in memory.
void dos_load_program(const char *name);
/// Saves the program in memory as `name`.
void dos_save_program(const char *name);

// ---- as_core.c: helpers the other parts use -------------------------------------

extern uint16_t tp;  ///< TXTPTR, kept in C while running (as_sync_* to zero page)
void as_sync_out(void);  ///< C state to zero page (before machine code runs)
void as_sync_in(void);   ///< and back

/// The FAC, and the type of the value in it.
extern as_fp fac;
extern bool fac_string;   ///< VALTYP $11
extern uint16_t fac_desc; ///< a string's descriptor address

/// Advances to the next non-space character of the program text (CHRGET).
uint8_t chrget(void);
/// The current character of the program text, again (CHRGOT).
uint8_t chrgot(void);
/// True if `c` ends a statement (end of line or colon).
bool is_end(uint8_t c);
void synchr(uint8_t c);  ///< the next character must be c (SYNTAX ERROR if not)
/// Checks for a comma, or gives ?SYNTAX ERROR (CHKCOM).
void chkcom(void);
/// Evaluates an expression into the accumulator (FRMEVL).
void frmevl(void);
/// Evaluates a numeric expression (FRMNUM).
void frmnum(void);
int getbyt(void);       ///< GTBYTC without the CHRGET: a byte expression
int gtbytc(void);       ///< CHRGET, then a byte expression
uint16_t getadr(void);  ///< FRMNUM then GETADR
void linget(void);      ///< line number into LINNUM

void as_run_program(void);      ///< RUN from the start
void as_new_program(void);      ///< NEW
void as_clear(void);            ///< CLEAR (variables, stack, DATA)
void as_fix_links(void);        ///< relink the program after loading it
/// Enters a line of text (as typed) into the program, or runs it if it has
/// no line number. Used by LOAD for text files.
void as_enter_line(const char *text);
/// True while a program is running (not at the ] prompt).
bool as_running(void);
/// Turns line-number tracing on or off (TRACE, NOTRACE).
void as_set_trace(bool on);

/// Prints a NUL-terminated string through OUTDO.
void as_print(const char *s);
/// Tokenises the text at $200 in place (the ROM's PARSE); returns the length.
int as_crunch(void);
/// Detokenises a program line for SAVE (no LIST spacing quirks).
int as_detok_line(uint16_t line_addr, char *out, int max);
