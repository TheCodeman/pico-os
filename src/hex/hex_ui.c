/**
 * @file hex_ui.c
 * @brief The `hex` command: a graphical hex editor in the sprite editor's style (bevelled panels, the scanout mouse arrow) for files of any size up to 4 GB.
 *
 * The screen: the bytes, 16 a row, in hex and as text, with a scroll bar;
 * under them what the byte at the cursor is as bits (click one to flip it)
 * and the numbers the bytes there make (8, 16 and 32 bits, both byte
 * orders, and a float); and the file buttons. Typing hex digits (or text,
 * in the text column) overwrites or inserts bytes; Del deletes them. A
 * selection can be cut, copied and pasted; Find looks for hex bytes or text
 * and Goto jumps to an offset. Changed bytes show in orange.
 *
 * Memory: the file stays on the drive and is read 512 bytes at a time
 * through a small cache. The edits are a piece table: the document is a list
 * of pieces, each a run of the file or of the bytes typed (kept in their own
 * buffer, which only grows), so a cut or paste of any size costs a few
 * pieces. Undo steps keep the pieces they replaced. All of it is in SRAM,
 * taken when the editor starts and given back when it quits; only the
 * off-screen picture (drawn and copied out at once) is in PSRAM. Saving
 * writes the document to a new file, then puts it in the old one's place.
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

#include "console.h"
#include "version.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "hardware/xip_cache.h"
#include "hexedit.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "usb_keyboard.h"

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height

#define PAGE 512               ///< the file is read in pages of this size
#define NPAGES 8               ///< pages kept
#define MAX_UNDO 64            ///< edits kept for undo (and as many for redo)
#define MAX_PIECES 65536       ///< the most pieces the document may be cut into
#define ADD_MAX (1024 * 1024)  ///< the most bytes typed between saves
#define CLIP_KEEP (256 * 1024) ///< the clipboard is kept over a save or load up to this size
#define MAX_REGIONS (2 * MAX_UNDO + 8)  ///< blocks of PSRAM the checksums look after
#define GBLOCK 4096            ///< the bytes each checksum covers
#define REFRESH_MS 10000       ///< every block is checked and rewritten this often
#ifndef PSRAM_UNCACHED_OFFSET
/// From the PSRAM's cached address to its uncached one.
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)
#endif
#define RESERVE (16 * 1024)    ///< SRAM left for everything else (mem warns below this)
#define CHUNK 4096             ///< bytes at a time when saving or searching
#define MAX_FILES 128          ///< entries the file box lists
#define POOL (4 * 1024)        ///< room for their names
#define PAT_MAX 64             ///< the longest search
#define TMP_NAME "HEXEDIT.TMP" ///< what a save writes before it replaces the file

/// Shift held (the HID modifier bits: left and right).
#define SHIFT_BITS 0x22

/// The colours (RGB332), as in the sprite editor.
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text, bytes
#define C_DIM    0x92  ///< quiet text, zero bytes
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles, the cursor
#define C_CYAN   0x1f  ///< typed text
#define C_ORANGE 0xf0  ///< changed bytes, warnings
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the selection
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip
#define C_CUR2   0x49  ///< the cursor in the other column
#define C_ALARM  0x80  ///< the status line after a memory error

/// Layout.
#define BAR_H  16      ///< the menu bar
#define HEAD_Y 22      ///< the column numbers
#define ROW_Y  40      ///< the first row of bytes
#define ROWS   20      ///< rows shown
#define OFF_X  12      ///< the offsets
#define HEX_X  88      ///< the first byte in hex
#define ASC_X  488     ///< the first byte as text
#define SB_X   622     ///< the scroll bar
#define SB_W   12      ///< its width
#define INS_Y  371     ///< the panel about the cursor
#define BITS_X 52      ///< the bits of the byte at the cursor
#define STAT_Y 456     ///< the status line
#define SHADOW 6       ///< a drop-down's shadow
#define C_GREEN 0x3c   ///< a ticked menu item

/// What a click hits.
enum {
    HIT_HEX = 1, HIT_ASC, HIT_SB, HIT_BIT, HIT_MODE, HIT_MENU, HIT_ITEM, HIT_MENU_OFF,
    HIT_ASK, HIT_ROW, HIT_FD_OK, HIT_FD_CANCEL, HIT_FD_UP, HIT_IN_OK, HIT_IN_CANCEL, HIT_IN_MODE, HIT_IN_CASE,
    HIT_SWALLOW,
};

/// The menus' commands.
enum {
    CMD_NEW = 1, CMD_LOAD, CMD_SAVE, CMD_SAVEAS, CMD_QUIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_DELETE, CMD_SELALL, CMD_INSERT, CMD_TEXT,
    CMD_FIND, CMD_NEXT, CMD_GOTO, CMD_SUMS, CMD_HELP, CMD_ABOUT,
};

/// A menu item.
typedef struct {
    const char *text;  ///< NULL ends; "-" is a line between items
    int id;            ///< CMD_*
    const char *key;   ///< the shortcut shown on the right
} hitem_t;

/// A menu on the bar.
typedef struct {
    const char *title;    ///< its title
    const hitem_t *items;  ///< its items
} hmenu_t;

/// The File menu.
static const hitem_t file_items[] = {
    {"New", CMD_NEW, "Ctrl-N"},       {"Open...", CMD_LOAD, "Ctrl-O"}, {"Save", CMD_SAVE, "Ctrl-S"},
    {"Save as...", CMD_SAVEAS, ""},   {"-", 0, NULL},                  {"Quit", CMD_QUIT, "Esc"},
    {NULL, 0, NULL},
};
/// The Edit menu.
static const hitem_t edit_items[] = {
    {"Undo", CMD_UNDO, "Ctrl-Z"},     {"Redo", CMD_REDO, "Ctrl-Y"},          {"-", 0, NULL},
    {"Cut", CMD_CUT, "Ctrl-X"},       {"Copy", CMD_COPY, "Ctrl-C"},          {"Paste", CMD_PASTE, "Ctrl-V"},
    {"Delete", CMD_DELETE, "Del"},    {"Select all", CMD_SELALL, "Ctrl-A"},  {"-", 0, NULL},
    {"Insert mode", CMD_INSERT, "Ins"}, {"Type in the text column", CMD_TEXT, "Tab"},
    {NULL, 0, NULL},
};
/// The Search menu.
static const hitem_t search_items[] = {
    {"Find...", CMD_FIND, "Ctrl-F"}, {"Find next", CMD_NEXT, "F3"}, {"Go to offset...", CMD_GOTO, "Ctrl-G"},
    {"-", 0, NULL},                  {"Checksums and CRCs...", CMD_SUMS, "Ctrl-K"},
    {NULL, 0, NULL},
};
/// The Help menu.
static const hitem_t help_items[] = {
    {"Keys", CMD_HELP, "F1"}, {"About", CMD_ABOUT, ""}, {NULL, 0, NULL},
};
/// The menus.
static const hmenu_t menus[] = {
    {"File", file_items}, {"Edit", edit_items}, {"Search", search_items}, {"Help", help_items},
};
#define NMENUS 4   ///< their count
#define MAX_ITEMS 12  ///< the most items a menu has

/// Things to do, after a question or a save.
enum { ACT_NONE, ACT_NEW, ACT_LOAD, ACT_QUIT, ACT_SAVE_THEN, ACT_WRITE };

/// The boxes over the editor.
enum { DLG_NONE, DLG_ASK, DLG_FILE, DLG_FIND, DLG_GOTO, DLG_HELP, DLG_ABOUT, DLG_SUMS };
/// What the file box is for.
enum { FD_LOAD, FD_SAVE };

/// Where a piece's bytes are.
enum { SRC_FILE, SRC_ADD };

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A run of the document's bytes: from the file, or from the bytes typed.
typedef struct {
    uint32_t off;  ///< where in its source
    uint32_t len;  ///< how many bytes
    uint8_t src;   ///< SRC_*
} piece_t;

/// An edit to undo (or redo): the bytes now at pos..pos+len were, before, the pieces kept.
typedef struct {
    uint32_t pos;  ///< where
    uint32_t len;  ///< the bytes there now
    int n;         ///< the pieces that were there
    piece_t *pc;   ///< them (malloc; NULL when none)
} undo_t;

/// A block of PSRAM the checksums look after (as the WAV editor's).
typedef struct {
    uint8_t *p;         ///< it
    uint32_t len;       ///< its bytes (a multiple of 8)
    uint32_t want;      ///< the bytes asked for (the rest is never written)
    uint32_t live_was;  ///< the bytes in use when last checked
    uint32_t *sums;     ///< a checksum for each GBLOCK (SRAM; NULL: none, only kept fresh)
    uint8_t *dirty;     ///< changed since its sum was taken (after sums)
} region_t;

/// An entry in the file box.
typedef struct {
    uint16_t name;  ///< its name, in the pool
    bool dir;       ///< a folder
    uint32_t size;  ///< its size
} fd_entry_t;

/// Checksums and CRCs of a range of bytes (CRCs as their catalogue gives them, each with its check value
/// for "123456789").
typedef struct {
    uint32_t lo, hi;      ///< the range
    uint8_t sum8;         ///< the bytes added up, 8 bits
    uint16_t sum16;       ///< 16 bits
    uint32_t sum32;       ///< 32 bits
    uint8_t xor8;         ///< the bytes XORed
    uint8_t crc8;         ///< CRC-8/SMBUS (0x07; check F4)
    uint16_t ccitt;       ///< CRC-16/CCITT-FALSE (0x1021, init FFFF; check 29B1)
    uint16_t xmodem;      ///< CRC-16/XMODEM (0x1021, init 0; check 31C3)
    uint16_t arc;         ///< CRC-16/ARC (0x8005 reflected, init 0; check BB3D)
    uint16_t modbus;      ///< CRC-16/MODBUS (0x8005 reflected, init FFFF; check 4B37)
    uint32_t crc32;       ///< CRC-32 as in zip and PNG (check CBF43926)
    uint32_t adler;       ///< Adler-32 (check 091E01DE)
    uint16_t fletcher;    ///< Fletcher-16 (check 1EDE)
} sums_t;

/// Everything the editor keeps (SRAM, while it runs).
typedef struct {
    // The file.
    FIL file;              ///< the file the document is made from
    bool file_open;        ///< there is one
    uint32_t fsize;        ///< its size
    char src[200];         ///< its name
    uint8_t cache[NPAGES][PAGE];  ///< pages of it
    uint32_t ctag[NPAGES]; ///< which page each is (+1; 0: none)
    uint32_t cuse[NPAGES]; ///< when each was last used
    uint32_t ctick;        ///< the clock for that
    bool read_err;         ///< a read failed
    // PSRAM: the pieces, the bytes typed, the undo steps' and the clipboard's pieces.
    region_t reg[MAX_REGIONS];  ///< the blocks
    int nreg;              ///< how many
    int rnext;             ///< the refresh's next region,
    uint32_t rblock;       ///< and block in it
    uint32_t refresh_at;   ///< when the next pass starts
    bool damaged;          ///< a block changed by itself
    // The document.
    piece_t *pc;           ///< its pieces
    int np;                ///< how many
    int pcap;              ///< room for
    uint8_t *add;          ///< the bytes typed
    uint32_t addlen;       ///< how many
    uint32_t addcap;       ///< room for
    uint32_t size;         ///< its length
    int hint_i;            ///< a piece found last,
    uint32_t hint_s;       ///< and where it starts
    undo_t undo[MAX_UNDO]; ///< edits to undo, oldest first
    int nundo;             ///< their count
    undo_t redo[MAX_UNDO]; ///< and to redo
    int nredo;             ///< their count
    bool typing;           ///< the last undo step grows as typing goes on
    piece_t *clip;         ///< the clipboard (pieces of the file or of the bytes typed)
    int nclip;             ///< how many
    uint32_t cliplen;      ///< its bytes
    char path[200];        ///< the file it saves to, or ""
    bool modified;         ///< changed since saved
    // The cursor.
    uint32_t cur;          ///< the byte (size: after the end)
    bool lo;               ///< on the second hex digit
    bool nib;              ///< the first digit was typed: the second changes that byte
    bool text;             ///< in the text column (else hex)
    bool ins;              ///< insert (else overwrite)
    bool sel;              ///< a selection, from anchor to cur
    uint32_t anchor;       ///< its other end
    uint32_t top;          ///< the first row shown
    // The mouse.
    int mdrag;             ///< 1: selecting, 2: the scroll bar's thumb
    uint32_t drag_a;       ///< the byte the selection began on
    int drag_dy;           ///< where the thumb was held
    bool drag_moved;       ///< the pointer left that byte
    int mx, my;            ///< the pointer
    uint32_t next_scroll;  ///< when dragging past the rows scrolls again
    // Find and Goto.
    uint8_t pat[PAT_MAX];  ///< what Find looks for
    int pat_n;             ///< its length
    bool pat_text;         ///< it was typed as text
    bool match_case;       ///< text: upper and lower case differ
    char in[132];          ///< what is typed in the Find or Goto box
    bool in_text;          ///< Find: text (else hex bytes); Goto: decimal (else hex)
    // The menus.
    int menu;              ///< the menu open (-1: none; NMENUS: the right button's Edit menu)
    int msel;              ///< its item chosen (-1: none)
    bool marmed;           ///< opened by a button still down: letting go on an item runs it
    int popx, popy;        ///< where the right button's menu is
    // Checksums.
    sums_t sums;           ///< the last ones worked out
    // The boxes.
    int dlg;               ///< DLG_*
    char ask[96];          ///< the question
    const char *ask_btn[3];  ///< its buttons (NULL: fewer)
    int ask_act[3];        ///< what each does
    int then;              ///< what to do after a save (ACT_*)
    int fd_mode;           ///< FD_*
    char fd_dir[200];      ///< the folder shown
    char fd_name[100];     ///< the name typed
    fd_entry_t *fd;        ///< its entries (MAX_FILES; malloc while the box is up)
    char *fd_pool;         ///< their names (POOL; in the same block as fd)
    bool fd_psram;         ///< that block is in PSRAM (SRAM was short)
    int fd_n;              ///< how many
    int fd_sel;            ///< the one chosen
    int fd_top;            ///< the first shown
    char target[200];      ///< the file to write (ACT_WRITE)
    char msg[96];          ///< a message for the title strip
    uint32_t msg_until;    ///< until when
    uint8_t *g;            ///< the off-screen picture (PSRAM), or NULL: drawn on the screen
    uint8_t *t;            ///< where drawing goes: g, or the screen itself
    hit_t hits[64];        ///< places to click
    int nhits;             ///< their count
    bool collect;          ///< drawing the whole picture: places to click are kept
    bool dirty;            ///< draw everything again
    bool view_dirty;       ///< draw the bytes and the panel under them again (straight onto the screen)
    bool quit;             ///< leaving
} ed_t;

static ed_t *F;  ///< the state, while the editor runs

// ---- SRAM ------------------------------------------------------------------------------------------

#ifdef HEX_PC_TEST
long hex_heap_free(void);  ///< the test's
#else
#include <malloc.h>
#include <unistd.h>
extern char __StackLimit;  ///< top of the heap, from the SDK's linker script

/// SRAM the heap can still give (worked out as the shell's mem does).
/// @return bytes
static long hex_heap_free(void) {
    struct mallinfo mi = mallinfo();
    return (long)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks;
}
#endif

/// malloc, but NULL (rather than a panic) when it would leave less than RESERVE.
/// @param n bytes
/// @return the block, or NULL
static void *salloc(size_t n) {
    if (hex_heap_free() - (long)n < RESERVE)
        return NULL;
    return malloc(n ? n : 1);
}

/// realloc, the same way.
/// @param p the block (NULL: a new one)
/// @param n bytes
/// @return the block (maybe moved), or NULL (p kept)
static void *srealloc(void *p, size_t n) {
    if (hex_heap_free() - (long)n < RESERVE)
        return NULL;
    return realloc(p, n ? n : 1);
}

static bool undo_drop_oldest(void);
static uint32_t now_ms(void);
static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- PSRAM, kept fresh ----------------------------------------------------------------------------
// The board's PSRAM loses bits when it holds data for minutes, so, as in the
// text and WAV editors, every block is read and written back every
// REFRESH_MS through the uncached window, and its checksum compared: a block
// that changed by itself is a MEMORY ERROR, and Save then won't write over
// the file.

/// The region a pointer is in.
/// @param p the pointer
/// @return it, or NULL (SRAM, or none)
static region_t *region_of(const void *p) {
    const uint8_t *b = p;
    for (int i = 0; i < F->nreg; ++i)
        if (b >= F->reg[i].p && b < F->reg[i].p + F->reg[i].len)
            return &F->reg[i];
    return NULL;
}

/// Gives a region its checksums, all to be taken.
/// @param r the region
static void region_sums(region_t *r) {
    uint32_t nb = (r->len + GBLOCK - 1) / GBLOCK;
    free(r->sums);
    r->sums = salloc(nb * 5);
    r->dirty = r->sums ? (uint8_t *)(r->sums + nb) : NULL;
    if (r->dirty)
        memset(r->dirty, 1, nb);
}

/// Memory for the edits: PSRAM the checksums look after (SRAM if there is no PSRAM); when there isn't room,
/// the oldest undo steps make way.
/// @param n bytes
/// @return it, or NULL
static void *galloc(size_t n) {
    size_t want = n;
    n = (n + 7) & ~(size_t)7;
    if (!n)
        n = 8;
    if (!psram_present())
        return salloc(n);
    void *p;
    while (F->nreg == MAX_REGIONS || !(p = psram_malloc(n)))
        if (!undo_drop_oldest())
            return NULL;
    region_t *r = &F->reg[F->nreg++];
    *r = (region_t){p, (uint32_t)n, (uint32_t)want, 0, NULL, NULL};
    region_sums(r);
    return p;
}

/// Frees memory from galloc.
/// @param p it (NULL is fine)
static void gfree(void *p) {
    if (!p)
        return;
    region_t *r = region_of(p);
    if (!r) {
        free(p);
        return;
    }
    free(r->sums);
    psram_free(r->p);
    *r = F->reg[--F->nreg];
    if (F->rnext >= F->nreg)
        F->rnext = 0, F->rblock = 0;
}

/// Makes memory from galloc bigger (or NULL: new), the oldest undo steps making way if need be.
/// @param p it, or NULL
/// @param n the bytes wanted
/// @return it (maybe moved), or NULL (p kept)
static void *grealloc(void *p, size_t n) {
    if (!p)
        return galloc(n);
    if (!psram_present())
        return srealloc(p, n);
    size_t want = n;
    n = (n + 7) & ~(size_t)7;
    void *np;
    while (!(np = psram_realloc(p, n)))
        if (!undo_drop_oldest())
            return NULL;
    region_t *r = region_of(p);
    r->p = np;
    r->want = (uint32_t)want;
    r->live_was = 0;
    r->len = (uint32_t)n;
    region_sums(r);
    return np;
}

/// Marks bytes the editor changed, so their checksums are taken again.
/// @param p the first
/// @param n how many
static void gtouch(const void *p, size_t n) {
    region_t *r = n ? region_of(p) : NULL;
    if (!r || !r->dirty)
        return;
    uint32_t a = (uint32_t)((const uint8_t *)p - r->p), b = a + (uint32_t)n - 1;
    if (b >= r->len)
        b = r->len - 1;
    for (uint32_t k = a / GBLOCK; k <= b / GBLOCK; ++k)
        r->dirty[k] = 1;
}

/// The bytes of a region in use (the room past the ends of the pieces and of the bytes typed is never
/// written, so not checked).
/// @param r the region
/// @return them
static uint32_t live_bytes(const region_t *r) {
    uint32_t n = r->want;
    if (r->p == (const uint8_t *)F->pc)
        n = (uint32_t)F->np * sizeof *F->pc;
    else if (r->p == F->add)
        n = F->addlen;
    return n < r->len ? n : r->len;
}

/// One block: from the chip, summed and written back. False if a block the editor hasn't changed no
/// longer matches its sum.
/// @param r the region
/// @param k the block
/// @return it was all right
static bool refresh_block(region_t *r, uint32_t k) {
    uint32_t live = live_bytes(r);
    if (live != r->live_was && r->dirty) {  // what came into use, or went, is summed afresh
        uint32_t lo = live < r->live_was ? live : r->live_was, hi = live ^ r->live_was ^ lo;
        for (uint32_t b = lo / GBLOCK; b <= (hi ? hi - 1 : 0) / GBLOCK && b * GBLOCK < r->len; ++b)
            r->dirty[b] = 1;
    }
    r->live_was = live;
    uint32_t a = k * GBLOCK;
    if (a >= live) {
        if (r->dirty)
            r->dirty[k] = 1;
        return true;
    }
    uint32_t n = live - a < GBLOCK ? live - a : GBLOCK;
    volatile uint32_t *w = (volatile uint32_t *)(r->p + a + PSRAM_UNCACHED_OFFSET);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n / 4; ++i) {
        uint32_t v = w[i];
        w[i] = v;
        sum = (sum << 5 | sum >> 27) ^ v;
    }
    for (uint32_t i = n & ~3u; i < n; ++i)  // the odd bytes at the end
        sum = (sum << 5 | sum >> 27) ^ ((volatile uint8_t *)w)[i];
    if (!r->sums)
        return true;
    if (r->dirty[k]) {
        r->sums[k] = sum;
        r->dirty[k] = 0;
        return true;
    }
    return r->sums[k] == sum;
}

/// Checks (and renews) n blocks, going round all the regions.
/// @param n blocks (-1: a whole pass)
/// @return false when the pass is over
static bool refresh(int n) {
    xip_cache_clean_all();  // the editor's writes out to the chip first
    for (int done = 0; n < 0 || done < n; ++done) {
        if (F->rnext >= F->nreg) {
            F->rnext = 0;
            F->rblock = 0;
            return false;
        }
        region_t *r = &F->reg[F->rnext];
        if (F->rblock * GBLOCK >= r->len) {  // a region freed or made smaller meanwhile
            F->rblock = 0, ++F->rnext;
            continue;
        }
        if (!refresh_block(r, F->rblock) && !F->damaged) {
            F->damaged = true;
            message("MEMORY ERROR: the edits changed by themselves; save as a new file");
        }
        if (++F->rblock * GBLOCK >= r->len)
            F->rblock = 0, ++F->rnext;
    }
    return true;
}

/// While the editor waits: four blocks at a time, a pass every REFRESH_MS.
static void refresh_step(void) {
    if ((int32_t)(now_ms() - F->refresh_at) < 0)
        return;
    if (!refresh(4))
        F->refresh_at = now_ms() + REFRESH_MS;
}

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static uint32_t now_ms(void);
static void progress(const char *what, uint32_t done, uint32_t total);

// ---- The file, a page at a time --------------------------------------------------------------------

/// Forgets the pages kept.
static void cache_clear(void) {
    memset(F->ctag, 0, sizeof F->ctag);
}

/// Reads bytes of the file (zeros, and read_err, if it fails).
/// @param off where
/// @param buf where to
/// @param n how many
static void file_read(uint32_t off, uint8_t *buf, uint32_t n) {
    if (n >= 2 * PAGE) {  // big reads go straight to the file
        UINT got = 0;
        if (f_lseek(&F->file, off) != FR_OK || f_read(&F->file, buf, n, &got) != FR_OK || got != n) {
            memset(buf + got, 0, n - got);
            F->read_err = true;
        }
        return;
    }
    while (n) {
        uint32_t page = off / PAGE, o = off % PAGE, k = PAGE - o;
        if (k > n)
            k = n;
        int slot = 0;
        for (int i = 0; i < NPAGES; ++i) {
            if (F->ctag[i] == page + 1) {
                slot = i;
                goto have;
            }
            if (F->cuse[i] < F->cuse[slot])
                slot = i;
        }
        UINT got = 0;
        if (f_lseek(&F->file, (FSIZE_t)page * PAGE) != FR_OK ||
            f_read(&F->file, F->cache[slot], PAGE, &got) != FR_OK || got < o + k) {
            memset(buf, 0, k);
            F->read_err = true;
            F->ctag[slot] = 0;
            goto next;
        }
        F->ctag[slot] = page + 1;
    have:
        F->cuse[slot] = ++F->ctick;
        memcpy(buf, F->cache[slot] + o, k);
    next:
        buf += k;
        off += k;
        n -= k;
    }
}

// ---- The document: a piece table -------------------------------------------------------------------

/// Reads bytes of a piece.
/// @param p the piece
/// @param o from where in it
/// @param buf where to
/// @param n how many
static void piece_read(const piece_t *p, uint32_t o, uint8_t *buf, uint32_t n) {
    if (p->src == SRC_ADD)
        memcpy(buf, F->add + p->off + o, n);
    else
        file_read(p->off + o, buf, n);
}

/// The piece that holds a byte.
/// @param pos the byte
/// @param start set to where that piece starts
/// @return its index (np if pos is at or after the end)
static int piece_at(uint32_t pos, uint32_t *start) {
    int i = 0;
    uint32_t s = 0;
    if (F->hint_i < F->np && F->hint_s <= pos)
        i = F->hint_i, s = F->hint_s;
    while (i < F->np && s + F->pc[i].len <= pos)
        s += F->pc[i++].len;
    if (i < F->np)
        F->hint_i = i, F->hint_s = s;
    *start = s;
    return i;
}

/// Reads bytes of the document.
/// @param pos from where
/// @param buf where to
/// @param chg set to 1 for each byte that was typed or pasted, else 0 (NULL: not wanted)
/// @param n how many (those past the end are left alone)
static void doc_read(uint32_t pos, uint8_t *buf, uint8_t *chg, uint32_t n) {
    uint32_t s;
    for (int i = piece_at(pos, &s); n && i < F->np; s += F->pc[i++].len) {
        const piece_t *p = &F->pc[i];
        uint32_t o = pos - s, k = p->len - o;
        if (k > n)
            k = n;
        piece_read(p, o, buf, k);
        if (chg)
            memset(chg, p->src == SRC_ADD, k), chg += k;
        buf += k, pos += k, n -= k;
    }
}

/// A byte of the document.
/// @param pos where (before the end)
/// @return it
static uint8_t doc_byte(uint32_t pos) {
    uint8_t b = 0;
    doc_read(pos, &b, NULL, 1);
    return b;
}

static bool undo_drop_oldest(void);

/// Makes room for n pieces (the oldest undo steps make way if need be).
/// @param n pieces
/// @return there is room
static bool pieces_room(int n) {
    if (n <= F->pcap)
        return true;
    if (n > MAX_PIECES)
        return false;
    int cap = F->pcap ? F->pcap : 64;
    while (cap < n)
        cap *= 2;
    if (cap > MAX_PIECES)
        cap = MAX_PIECES;
    piece_t *p;
    if (!(p = grealloc(F->pc, cap * sizeof *p)))
        return false;
    F->pc = p;
    F->pcap = cap;
    return true;
}

/// Makes room for n more bytes typed.
/// @param n bytes
/// @return there is room
static bool add_room(uint32_t n) {
    if (F->addlen + n <= F->addcap)
        return true;
    if (F->addlen + n > ADD_MAX)
        return false;
    uint32_t cap = F->addcap ? F->addcap : 1024;
    while (cap < F->addlen + n)
        cap *= 2;
    if (cap > ADD_MAX)
        cap = ADD_MAX;
    uint8_t *a;
    if (!(a = grealloc(F->add, cap)))
        return false;
    F->add = a;
    F->addcap = cap;
    return true;
}

/// Makes a piece start at a byte, cutting the one across it in two (room for one more must be there).
/// @param pos the byte
/// @return the index of the piece that starts there
static int split_at(uint32_t pos) {
    uint32_t s;
    int i = piece_at(pos, &s);
    if (i == F->np || s == pos)
        return i;
    piece_t *p = &F->pc[i];
    uint32_t o = pos - s;
    memmove(&F->pc[i + 2], &F->pc[i + 1], (F->np - i - 1) * sizeof *p);
    F->pc[i + 1] = (piece_t){p->off + o, p->len - o, p->src};
    p->len = o;
    ++F->np;
    gtouch(&F->pc[i], (F->np - i) * sizeof *p);
    F->hint_i = i + 1, F->hint_s = pos;
    return i + 1;
}

/// Joins pieces that follow on from each other in their source, and drops empty ones, from lo to hi.
/// @param lo the first piece to look at
/// @param hi the last
static void merge(int lo, int hi) {
    if (lo < 0)
        lo = 0;
    for (int i = lo; i <= hi && i < F->np;) {
        piece_t *p = &F->pc[i];
        int drop = -1;
        if (!p->len)
            drop = i;
        else if (i + 1 < F->np && p[1].src == p->src && p->off + p->len == p[1].off)
            p->len += p[1].len, drop = i + 1;
        if (drop < 0) {
            ++i;
            continue;
        }
        memmove(&F->pc[drop], &F->pc[drop + 1], (F->np - drop - 1) * sizeof *p);
        --F->np, --hi;
    }
    if (lo < F->np)
        gtouch(&F->pc[lo], (F->np - lo) * sizeof *F->pc);
}

/// The total length of some pieces.
/// @param p the pieces
/// @param n how many
/// @return their bytes
static uint32_t pieces_len(const piece_t *p, int n) {
    uint32_t len = 0;
    for (int i = 0; i < n; ++i)
        len += p[i].len;
    return len;
}

/// Replaces bytes of the document with pieces.
/// @param pos where
/// @param len how many bytes go
/// @param ins the pieces that come in (not the document's own)
/// @param nins how many
/// @param out set to a copy of the pieces that went (malloc; NULL if none or if out is NULL)
/// @param nout set to how many
/// @return it was done (false: no memory, and nothing changed)
static bool replace(uint32_t pos, uint32_t len, const piece_t *ins, int nins, piece_t **out, int *nout) {
    uint32_t inslen = pieces_len(ins, nins);
    if ((uint64_t)F->size - len + inslen > 0xfffffff0u || !pieces_room(F->np + nins + 2))
        return false;
    int a = split_at(pos), b = split_at(pos + len), nold = b - a;
    if (out) {
        *out = NULL;
        *nout = nold;
        if (nold && !(*out = galloc(nold * sizeof **out)))
            return false;  // the cuts change nothing
        if (nold)
            memcpy(*out, &F->pc[a], nold * sizeof **out);
    }
    memmove(&F->pc[a + nins], &F->pc[b], (F->np - b) * sizeof *F->pc);
    if (nins)
        memcpy(&F->pc[a], ins, nins * sizeof *F->pc);
    F->np += nins - nold;
    if (a < F->np)
        gtouch(&F->pc[a], (F->np - a) * sizeof *F->pc);
    F->size = F->size - len + inslen;
    merge(a - 1, a + nins);
    F->hint_i = 0, F->hint_s = 0;
    return true;
}

/// Copies the pieces that make up some bytes of the document.
/// @param pos where
/// @param len how many
/// @param n set to how many pieces
/// @return them (malloc), or NULL (none, or no memory)
static piece_t *pieces_of(uint32_t pos, uint32_t len, int *n) {
    uint32_t s;
    int i0 = piece_at(pos, &s), i = i0;
    uint32_t s0 = s, end = pos + len;
    for (*n = 0; i < F->np && s < end; s += F->pc[i++].len)
        ++*n;
    piece_t *p = *n ? galloc(*n * sizeof *p) : NULL;
    if (!p)
        return NULL;
    memcpy(p, &F->pc[i0], *n * sizeof *p);
    uint32_t cut = pos - s0;  // the first starts at pos,
    p[0].off += cut, p[0].len -= cut;
    p[*n - 1].len -= s - end;  // the last ends at end
    return p;
}

/// Starts the document afresh: the whole file (or nothing), no undo.
static void doc_reset(void);

// ---- Undo ------------------------------------------------------------------------------------------

/// Forgets the redo steps.
static void redo_clear(void) {
    while (F->nredo)
        gfree(F->redo[--F->nredo].pc);
}

/// Forgets every step.
static void undo_forget(void) {
    redo_clear();
    while (F->nundo)
        gfree(F->undo[--F->nundo].pc);
    F->typing = false;
}

/// Forgets the oldest undo step (or redo step), to make room.
/// @return there was one
static bool undo_drop_oldest(void) {
    undo_t *u = F->nundo ? F->undo : F->redo;
    int *n = F->nundo ? &F->nundo : &F->nredo;
    if (!*n)
        return false;
    gfree(u[0].pc);
    memmove(&u[0], &u[1], (*n - 1) * sizeof *u);
    if (!--*n)
        F->typing = false;
    return true;
}

/// Edits the document, keeping a step to undo.
/// @param pos where
/// @param len how many bytes go
/// @param ins the pieces that come in
/// @param nins how many
/// @param typing part of typing: it joins the last step if it carries straight on from it
/// @return it was done
static bool edit(uint32_t pos, uint32_t len, const piece_t *ins, int nins, bool typing) {
    undo_t *u = F->nundo ? &F->undo[F->nundo - 1] : NULL;
    bool join = typing && F->typing && u && pos == u->pos + u->len;
    piece_t *old;
    int nold;
    if (!replace(pos, len, ins, nins, &old, &nold)) {
        message("Not enough memory for that: save, and it goes on from there");
        return false;
    }
    redo_clear();
    uint32_t inslen = pieces_len(ins, nins);
    piece_t *p = NULL;
    if (join && (!nold || (p = grealloc(u->pc, (u->n + nold) * sizeof *p)))) {
        if (nold) {
            memcpy(p + u->n, old, nold * sizeof *p);
            u->pc = p;
            u->n += nold;
        }
        u->len += inslen;
        gfree(old);
    } else {
        if (F->nundo == MAX_UNDO)
            undo_drop_oldest();
        F->undo[F->nundo++] = (undo_t){pos, inslen, nold, old};
    }
    F->typing = typing;
    F->modified = true;
    F->view_dirty = true;
    return true;
}

/// Undoes or redoes a step.
/// @param redo redo (else undo)
static void undo_step(bool redo) {
    undo_t *from = redo ? F->redo : F->undo, *to = redo ? F->undo : F->redo;
    int *nfrom = redo ? &F->nredo : &F->nundo, *nto = redo ? &F->nundo : &F->nredo;
    F->typing = F->nib = false;
    if (!*nfrom) {
        message("Nothing to %s", redo ? "redo" : "undo");
        return;
    }
    undo_t *u = &from[*nfrom - 1];
    piece_t *now;
    int nnow;
    if (!replace(u->pos, u->len, u->pc, u->n, &now, &nnow)) {
        message("Not enough memory to %s", redo ? "redo" : "undo");
        return;
    }
    --*nfrom;
    if (*nto == MAX_UNDO) {
        gfree(to[0].pc);
        memmove(&to[0], &to[1], (MAX_UNDO - 1) * sizeof *to);
        --*nto;
    }
    to[(*nto)++] = (undo_t){u->pos, pieces_len(u->pc, u->n), nnow, now};
    gfree(u->pc);
    F->cur = u->pos;
    F->sel = F->lo = false;
    F->modified = true;
    F->dirty = true;
}

// ---- Drawing ----------------------------------------------------------------------------------

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
    if (w <= 0 || h <= 0)  // off the screen
        return;
    for (int r = 0; r < h; ++r)
        memset(F->t + (y + r) * W + x, c, w);
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
        uint8_t *p = F->t + (y + r) * W + x;
        for (int i = 0; i < 8; ++i)
            p[i] = gl[r] & (0x80 >> i) ? fg : bg;
    }
}

/// Draws text.
/// @param x left
/// @param y top
/// @param s the text
/// @param fg its colour
/// @param bg the background
/// @return the x after it
static int text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; ++s, x += 8)
        ch(x, y, (uint8_t)*s, fg, bg);
    return x;
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
    char buf[100];
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
    if (F->collect && F->nhits < (int)(sizeof F->hits / sizeof *F->hits))
        F->hits[F->nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point (the last one added wins).
/// @param x the point's x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = F->nhits - 1; i >= 0; --i) {
        const hit_t *h = &F->hits[i];
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

/// The file's name without its folder.
/// @return it, or "untitled"
static const char *file_name(void) {
    if (!F->path[0])
        return "untitled";
    const char *s = strrchr(F->path, '/');
    return s ? s + 1 : F->path;
}

/// The screen x of a byte's first hex digit.
/// @param col the byte's column (0-15)
/// @return it
static int hex_x(int col) {
    return HEX_X + col * 24 + (col >= 8 ? 8 : 0);
}

/// The rows a scroll can show: one for each 16 bytes and one for the end.
/// @return them
static uint32_t total_rows(void) {
    return F->size / 16 + 1;
}

/// The first row shown at most.
/// @return it
static uint32_t top_max(void) {
    return total_rows() > ROWS ? total_rows() - ROWS : 0;
}

/// The selection.
/// @param lo set to its first byte
/// @param hi set to after its last
/// @return there is one (not empty)
static bool sel_range(uint32_t *lo, uint32_t *hi) {
    uint32_t a = F->anchor < F->cur ? F->anchor : F->cur, b = F->anchor < F->cur ? F->cur : F->anchor;
    if (b > F->size)
        b = F->size;
    *lo = a, *hi = b;
    return F->sel && a < b;
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

/// Draws the menu bar, as the tracker's: the titles (the one open lit), the file's name on the right.
static void draw_bar(void) {
    fill(0, 0, W, BAR_H, C_FACE);
    for (int i = 0; i < NMENUS; ++i) {
        int bx = bar_x(i), bw = ((int)strlen(menus[i].title) + 4) * 8;
        bool open = i == F->menu;
        if (open)
            fill(bx, 1, bw, 14, C_SEL);
        text(bx + 16, 0, menus[i].title, open ? C_YELLOW : C_TEXT, open ? C_SEL : C_FACE);
        fill(bx + 16, 14, 8, 1, open ? C_YELLOW : C_LABEL);  // its key letter (Alt and it)
        hit(bx, 0, bw, BAR_H, HIT_MENU, i);
    }
    char name[60];
    snprintf(name, sizeof name, "%.40s%s  Hex Editor " VER_HEX, file_name(), F->modified ? "*" : "");
    text(W - 8 - (int)strlen(name) * 8, 0, name, C_YELLOW, C_FACE);
    fill(0, 0, W, 1, C_LIGHT);
    fill(0, BAR_H - 1, W, 1, C_DARK);
}

/// Draws the status line: a message, or the size and the mode.
static void draw_status(void) {
    uint8_t bg = F->damaged ? C_ALARM : C_STRIP;  // white on dark red after a memory error
    fill(0, STAT_Y, W, H - STAT_Y, bg);
    if (F->msg[0] && now_ms() < F->msg_until)
        textf(8, STAT_Y + 4, C_TEXT, bg, "%.78s", F->msg);
    else if (F->damaged)
        text(8, STAT_Y + 4, "MEMORY ERROR in the edits: Save as a new file, and check it", C_TEXT, bg);
    else
        textf(8, STAT_Y + 4, C_LABEL, C_STRIP, "%lu bytes  %s  %s column    F1 keys  F10 menus", (unsigned long)F->size,
              F->ins ? "Insert" : "Overwrite", F->text ? "text" : "hex");
}

/// Draws the column numbers, the rows of bytes and the scroll bar (each pixel drawn once, so straight onto
/// the screen they don't flicker).
static void draw_rows(void) {
    uint32_t lo, hi;
    bool sel = sel_range(&lo, &hi);
    int ccol = F->cur % 16;
    // The column numbers.
    fill(8, HEAD_Y, 608, 16, C_FACE);
    text(OFF_X, HEAD_Y, "Offset", C_LABEL, C_FACE);
    for (int c = 0; c < 16; ++c) {
        uint8_t fg = c == ccol ? C_YELLOW : C_LABEL;
        textf(hex_x(c), HEAD_Y, fg, C_FACE, "%02X", c);
        ch(ASC_X + c * 8, HEAD_Y, "0123456789ABCDEF"[c], fg, C_FACE);
    }
    // The rows.
    uint8_t b[16], chg[16];
    for (int r = 0; r < ROWS; ++r) {
        uint32_t base = (F->top + r) * 16, y = ROW_Y + r * 16;
        uint8_t bg = r & 1 ? C_ROWALT : C_FIELD;
        int n = base < F->size ? (F->size - base < 16 ? (int)(F->size - base) : 16) : 0;
        if (base > F->size)
            n = -1;  // past the end altogether
        if (n > 0)
            doc_read(base, b, chg, n);
        fill(8, y, HEX_X - 8, 16, bg);
        fill(hex_x(15) + 16, y, ASC_X - hex_x(15) - 16, 16, bg);
        fill(ASC_X + 128, y, SB_X - ASC_X - 128, 16, bg);
        if (n >= 0)
            textf(OFF_X, y, F->cur / 16 == base / 16 ? C_YELLOW : C_LABEL, bg, "%08lX", (unsigned long)base);
        for (int c = 0; c < 16; ++c) {
            uint32_t at = base + c;
            bool in = n >= 0 && c < n, is_cur = n >= 0 && at == F->cur;
            uint8_t cbg = in && sel && at >= lo && at < hi ? C_SEL : bg;
            uint8_t fg = !in ? C_TEXT : chg[c] ? C_ORANGE : b[c] ? C_TEXT : C_DIM;
            int x = hex_x(c);
            // The hex digits, the cursor on one of them (or dim in the text column).
            uint8_t bg1 = cbg, bg2 = cbg, fg1 = fg, fg2 = fg;
            if (is_cur) {
                bg1 = bg2 = C_CUR2;
                if (!F->text) {
                    if (F->lo || F->nib)
                        bg2 = C_YELLOW, fg2 = C_BG;
                    else
                        bg1 = C_YELLOW, fg1 = C_BG;
                }
            }
            ch(x, y, in ? "0123456789ABCDEF"[b[c] >> 4] : ' ', fg1, bg1);
            ch(x + 8, y, in ? "0123456789ABCDEF"[b[c] & 15] : ' ', fg2, bg2);
            // The gap after it: part of the selection if the next byte is too.
            bool next_sel = cbg == C_SEL && c < 15 && at + 1 < hi;
            fill(x + 16, y, c == 7 ? 16 : c == 15 ? 0 : 8, 16, next_sel ? C_SEL : bg);
            // As text.
            uint8_t t = in ? b[c] : ' ';
            bool show = t >= 0x20 && t < 0x7f;
            uint8_t tbg = is_cur ? (F->text ? C_YELLOW : C_CUR2) : cbg;
            uint8_t tfg = is_cur && F->text ? C_BG : !in || show ? (in && chg[c] ? C_ORANGE : C_TEXT) : C_DIM;
            ch(ASC_X + c * 8, y, show ? t : in ? '.' : ' ', tfg, tbg);
        }
    }
    // The scroll bar.
    int sh = ROWS * 16, th = sh, ty = 0;
    uint32_t rows = total_rows();
    if (rows > ROWS) {
        th = (int)((uint64_t)sh * ROWS / rows);
        if (th < 16)
            th = 16;
        ty = (int)((uint64_t)(sh - th) * F->top / top_max());
    }
    fill(SB_X, ROW_Y, SB_W, sh, C_DARK);
    bevel(SB_X, ROW_Y + ty, SB_W, th, true, C_FACE);
}

/// Formats a number of up to 8 bytes, or "-" if they go past the end.
/// @param buf where
/// @param size its room
/// @param v the number
/// @param ok there were enough bytes
/// @param sign signed
/// @param bytes how many (for the sign)
static void num(char *buf, int size, uint32_t v, bool ok, bool sign, int bytes) {
    if (!ok) {
        snprintf(buf, size, "-");
    } else if (sign) {
        int shift = 32 - bytes * 8;
        snprintf(buf, size, "%ld", (long)((int32_t)(v << shift) >> shift));
    } else {
        snprintf(buf, size, "%lu", (unsigned long)v);
    }
}

/// Draws the panel about the cursor: where it is, the selection, the byte's bits and the numbers there.
static void draw_inspector(void) {
    fill(8, INS_Y + 4, 536, 74, C_FACE);
    uint8_t b[4] = {0};
    uint32_t have = F->cur < F->size ? F->size - F->cur : 0;
    if (have > 4)
        have = 4;
    doc_read(F->cur, b, NULL, have);
    int x = textf(12, INS_Y + 5, C_LABEL, C_FACE, "At ");
    x = textf(x, INS_Y + 5, C_TEXT, C_FACE, "%08lX", (unsigned long)F->cur);
    x = textf(x, INS_Y + 5, C_LABEL, C_FACE, " (%lu)", (unsigned long)F->cur);
    uint32_t lo, hi;
    if (sel_range(&lo, &hi)) {
        x = textf(x + 16, INS_Y + 5, C_LABEL, C_FACE, "Selected ");
        textf(x, INS_Y + 5, C_CYAN, C_FACE, "%08lX-%08lX  %lu byte%s", (unsigned long)lo, (unsigned long)hi - 1,
              (unsigned long)(hi - lo), hi - lo == 1 ? "" : "s");
    } else if (F->cliplen) {
        textf(x + 16, INS_Y + 5, C_DIM, C_FACE, "Clipboard %lu byte%s", (unsigned long)F->cliplen,
              F->cliplen == 1 ? "" : "s");
    }
    // The bits, then the byte as numbers and the four bytes as a float.
    int y = INS_Y + 23;
    text(12, y, "Bits", C_LABEL, C_FACE);
    for (int i = 0; i < 8; ++i) {
        int bx = BITS_X + i * 12;
        if (have)
            ch(bx, y, b[0] & (0x80 >> i) ? '1' : '0', C_YELLOW, C_SEL);
        else
            ch(bx, y, '-', C_DIM, C_FACE);
        fill(bx + 8, y, 4, 16, C_FACE);
    }
    char s1[16], s2[16], s3[16], s4[16];
    num(s1, sizeof s1, b[0], have >= 1, false, 1);
    num(s2, sizeof s2, b[0], have >= 1, true, 1);
    x = textf(BITS_X + 100, y, C_LABEL, C_FACE, "u8 ");
    x = textf(x, y, C_TEXT, C_FACE, "%-4s", s1);
    x = textf(x + 8, y, C_LABEL, C_FACE, "i8 ");
    x = textf(x, y, C_TEXT, C_FACE, "%-5s", s2);
    x = textf(x + 8, y, C_LABEL, C_FACE, "f32 ");
    if (have == 4) {
        union { uint32_t u; float f; } v = {(uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24};
        textf(x, y, C_TEXT, C_FACE, "%.7g", (double)v.f);
    } else {
        text(x, y, "-", C_TEXT, C_FACE);
    }
    // 16 and 32 bits, both byte orders.
    for (int w = 0; w < 2; ++w) {
        int bytes = w ? 4 : 2;
        bool ok = have >= (uint32_t)bytes;
        uint32_t le = 0, be = 0;
        for (int i = 0; i < bytes; ++i) {
            le |= (uint32_t)b[i] << (8 * i);
            be = be << 8 | b[i];
        }
        y = INS_Y + 41 + w * 18;
        num(s1, sizeof s1, le, ok, false, bytes);
        num(s2, sizeof s2, be, ok, false, bytes);
        num(s3, sizeof s3, le, ok, true, bytes);
        num(s4, sizeof s4, be, ok, true, bytes);
        x = textf(12, y, C_LABEL, C_FACE, "u%d LE ", bytes * 8);
        x = textf(x, y, C_TEXT, C_FACE, "%-*s", w ? 10 : 5, s1);
        x = textf(x + 8, y, C_LABEL, C_FACE, "BE ");
        x = textf(x, y, C_TEXT, C_FACE, "%-*s", w ? 10 : 5, s2);
        x = textf(x + 16, y, C_LABEL, C_FACE, "i%d LE ", bytes * 8);
        x = textf(x, y, C_TEXT, C_FACE, "%-*s", w ? 11 : 6, s3);
        x = textf(x + 8, y, C_LABEL, C_FACE, "BE ");
        textf(x, y, C_TEXT, C_FACE, "%-*s", w ? 11 : 6, s4);
    }
}

/// The mode button (insert or overwrite).
static void draw_mode(void) {
    button(548, INS_Y + 6, 84, 22, F->ins ? "Insert" : "Overwrite", F->ins, HIT_MODE, 0);
    button(548, INS_Y + 32, 84, 22, F->text ? "Text" : "Hex", false, HIT_MODE, 1);
}

/// Draws what moving the cursor or typing changes: the bar, the rows, the panel under them and the status.
static void draw_view(void) {
    draw_bar();
    draw_rows();
    draw_inspector();
    draw_mode();
    draw_status();
}

// ---- The menus ---------------------------------------------------------------------------------

/// Whether a menu item can be chosen now.
/// @param it the item
/// @return it can
static bool usable(const hitem_t *it) {
    uint32_t lo, hi;
    switch (it->id) {
    case 0: return false;
    case CMD_UNDO: return F->nundo > 0;
    case CMD_REDO: return F->nredo > 0;
    case CMD_CUT: case CMD_COPY: return sel_range(&lo, &hi);
    case CMD_DELETE: return sel_range(&lo, &hi) || F->cur < F->size;
    case CMD_PASTE: return F->nclip > 0;
    case CMD_NEXT: return F->pat_n > 0;
    }
    return true;
}

/// Whether a menu item shows a tick.
/// @param it the item
/// @return it does
static bool ticked(const hitem_t *it) {
    return (it->id == CMD_INSERT && F->ins) || (it->id == CMD_TEXT && F->text);
}

/// The items of the menu open.
/// @return them
static const hitem_t *menu_items(void) {
    return F->menu == NMENUS ? edit_items : menus[F->menu].items;
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
    *x = F->menu == NMENUS ? F->popx : bar_x(F->menu);
    *y = F->menu == NMENUS ? F->popy : BAR_H;
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
    if (F->menu < NMENUS)
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
        bool on = i == F->msel, off = !usable(it);
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
/// @param m the menu (NMENUS: the Edit menu where the pointer is)
/// @param armed opened by a button that is still down
static void menu_open(int m, bool armed) {
    F->menu = m;
    F->msel = -1;
    F->marmed = armed;
    F->dirty = true;
}

/// Closes the menu.
static void menu_close(void) {
    F->menu = -1;
    F->dirty = true;
}

static void command(int id);

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
    F->dirty = true;
    if (k == KEY_ESC || k == KEY_F9 || k == KEY_F10 || k == CTRL('c')) {
        menu_close();
    } else if (k == KEY_UP || k == KEY_DOWN) {
        int d = k == KEY_UP ? -1 : 1, s = F->msel < 0 ? (d > 0 ? -1 : 0) : F->msel;
        for (int t = 0; t < n; ++t) {
            s = (s + d + n) % n;
            if (usable(&items[s]))
                break;
        }
        F->msel = s;
    } else if ((k == KEY_LEFT || k == KEY_RIGHT) && F->menu < NMENUS) {
        menu_open((F->menu + (k == KEY_LEFT ? NMENUS - 1 : 1)) % NMENUS, false);
    } else if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
        for (int i = 0; i < NMENUS; ++i)
            if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                menu_open(i, false);
    } else if (k == '\r' || k == '\n') {
        if (F->msel >= 0)
            item_run(F->msel);
    } else if (k >= 0x20 && k < 0x7f) {
        for (int i = 0; i < n; ++i)
            if (acc[i] >= 0 && (items[i].text[acc[i]] | 0x20) == (k | 0x20) && usable(&items[i])) {
                item_run(i);
                break;
            }
    }
}

// ---- The boxes ---------------------------------------------------------------------------------

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

/// Draws the question.
static void draw_ask(void) {
    int w = 460, h = 96, x = (W - w) / 2, y = 180;
    box(x, y, w, h, F->ask);
    for (int i = 0; i < 3 && F->ask_btn[i]; ++i)
        button(x + 16 + i * 128, y + 52, 120, 26, F->ask_btn[i], i == 0, HIT_ASK, i);
}

/// The file box's rows shown.
#define FD_ROWS 12

/// Draws the file box.
static void draw_file_box(void) {
    int w = 500, h = 360, x = (W - w) / 2, y = 60;
    box(x, y, w, h, F->fd_mode == FD_LOAD ? "Open a file" : "Save the file as");
    textf(x + 16, y + 32, C_LABEL, C_FACE, "%.52s", F->fd_dir);
    button(x + w - 64, y + 30, 48, 20, "Up", false, HIT_FD_UP, 0);
    int ly = y + 56;
    bevel(x + 12, ly - 2, w - 24, FD_ROWS * 18 + 4, false, C_FIELD);
    if (F->fd_sel >= F->fd_n)
        F->fd_sel = F->fd_n ? F->fd_n - 1 : 0;
    if (F->fd_sel < F->fd_top)
        F->fd_top = F->fd_sel;
    if (F->fd_sel >= F->fd_top + FD_ROWS)
        F->fd_top = F->fd_sel - FD_ROWS + 1;
    for (int r = 0; r < FD_ROWS; ++r) {
        int i = F->fd_top + r, ry = ly + r * 18;
        uint8_t bg = i == F->fd_sel && i < F->fd_n ? C_SEL : (r & 1) ? C_ROWALT : C_FIELD;
        fill(x + 14, ry, w - 28, 18, bg);
        if (i >= F->fd_n)
            continue;
        const fd_entry_t *e = &F->fd[i];
        textf(x + 20, ry + 1, e->dir ? C_CYAN : C_TEXT, bg, "%s%.42s", e->dir ? "\x10 " : "  ", F->fd_pool + e->name);
        if (!e->dir) {
            char sz[16];
            if (e->size < 10000000)
                snprintf(sz, sizeof sz, "%lu", (unsigned long)e->size);
            else
                snprintf(sz, sizeof sz, "%lu MB", (unsigned long)(e->size >> 20));
            text(x + w - 24 - (int)strlen(sz) * 8, ry + 1, sz, C_LABEL, bg);
        }
        hit(x + 14, ry, w - 28, 18, HIT_ROW, i);
    }
    int ny = ly + FD_ROWS * 18 + 12;
    text(x + 16, ny + 3, "Name", C_LABEL, C_FACE);
    bevel(x + 60, ny, w - 76, 22, false, C_FIELD);
    int n = strlen(F->fd_name), skip = n > 50 ? n - 50 : 0;  // the end of a long name
    textf(x + 64, ny + 3, C_CYAN, C_FIELD, "%s_", F->fd_name + skip);
    button(x + 16, y + h - 36, 120, 26, F->fd_mode == FD_LOAD ? "Open (Enter)" : "Save (Enter)", true, HIT_FD_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_FD_CANCEL, 0);
}

/// Draws the Find or Goto box.
static void draw_input_box(void) {
    int w = 500, h = 150, x = (W - w) / 2, y = 150;
    bool find = F->dlg == DLG_FIND;
    box(x, y, w, h, find ? "Find (F3 finds the next)" : "Go to an offset (+ or - goes from the cursor)");
    bevel(x + 16, y + 36, w - 32, 22, false, C_FIELD);
    int n = strlen(F->in), skip = n > 56 ? n - 56 : 0;
    textf(x + 20, y + 39, C_CYAN, C_FIELD, "%s_", F->in + skip);
    const char *m0 = find ? "Hex bytes" : "Hex", *m1 = find ? "Text" : "Decimal";
    button(x + 16, y + 70, 100, 22, m0, !F->in_text, HIT_IN_MODE, 0);
    button(x + 120, y + 70, 100, 22, m1, F->in_text, HIT_IN_MODE, 1);
    text(x + 228, y + 73, "(Tab)", C_DIM, C_FACE);
    if (find && F->in_text)
        button(x + 290, y + 70, 110, 22, "Match case", F->match_case, HIT_IN_CASE, 0);
    button(x + 16, y + h - 36, 120, 26, find ? "Find (Enter)" : "Go (Enter)", true, HIT_IN_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_IN_CANCEL, 0);
}

/// Draws the keys.
static void draw_help(void) {
    int w = 620, h = 420, x = (W - w) / 2, y = 30;
    box(x, y, w, h, "Keys (any key or click closes this)");
    static const char *const lines[] = {
        "Hex digits overwrite (or insert) a byte, a digit at a time; in the",
        "text column (Tab, or click there) letters type themselves.",
        "Arrows      move (Shift: select)   PgUp/PgDn  a screen at a time",
        "Home / End  the row's start / end; again: the file's start / end",
        "Ins         insert or overwrite    Tab  hex or text column",
        "Del         delete the byte or the selection",
        "Backspace   insert: delete the byte before; else back a byte",
        "Ctrl-A      select all   Ctrl-C copy   Ctrl-X cut   Ctrl-V paste",
        "Ctrl-Z, Y   undo, redo (64 steps; a save forgets them)",
        "Ctrl-F      find hex bytes or text      F3  find the next",
        "Ctrl-G      go to an offset (hex; + or - from the cursor)",
        "Ctrl-K      checksums and CRCs of the selection (or the file)",
        "Ctrl-S, F2  save   Ctrl-O open   Ctrl-N new   Esc quit",
        "F10, F9     the menus (Alt and a letter opens one)",
        "Mouse: click a byte to put the cursor there, drag to select;",
        "the wheel and the scroll bar scroll; the right button gives",
        "the Edit menu. Click a bit under the bytes to flip it. Orange",
        "bytes are changed and not saved yet.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 36 + (int)i * 20, lines[i], C_TEXT, C_FACE);
}

/// Draws the checksums box.
static void draw_sums(void) {
    int w = 520, h = 330, x = (W - w) / 2, y = 70;
    const sums_t *k = &F->sums;
    box(x, y, w, h, "Checksums and CRCs");
    uint32_t n = k->hi - k->lo;
    if (n)
        textf(x + 16, y + 34, C_LABEL, C_FACE, "%s %08lX-%08lX, %lu byte%s", k->lo || k->hi != F->size ? "Bytes" : "The file",
              (unsigned long)k->lo, (unsigned long)k->hi - 1, (unsigned long)n, n == 1 ? "" : "s");
    else
        text(x + 16, y + 34, "No bytes", C_LABEL, C_FACE);
    static const char *const names[] = {
        "Sum-8", "Sum-16", "Sum-32", "XOR-8", "Two's complement (Intel HEX)", "CRC-8/SMBUS", "CRC-16/CCITT-FALSE",
        "CRC-16/XMODEM", "CRC-16/ARC", "CRC-16/MODBUS", "CRC-32 (zip, PNG)", "Adler-32", "Fletcher-16",
    };
    uint32_t v[] = {k->sum8, k->sum16, k->sum32, k->xor8, (uint8_t)-k->sum8, k->crc8, k->ccitt, k->xmodem,
                    k->arc, k->modbus, k->crc32, k->adler, k->fletcher};
    static const uint8_t digits[] = {2, 4, 8, 2, 2, 2, 4, 4, 4, 4, 8, 8, 4};
    for (int i = 0; i < 13; ++i) {
        int ry = y + 58 + i * 18;
        text(x + 16, ry, names[i], C_LABEL, C_FACE);
        textf(x + 256, ry, C_YELLOW, C_FACE, "%0*lX", digits[i], (unsigned long)v[i]);
        textf(x + 344, ry, C_DIM, C_FACE, "%lu", (unsigned long)v[i]);
    }
    text(x + 16, y + h - 26, "(any key or click closes this)", C_DIM, C_FACE);
}

/// Works out the checksums of the selection (or of the whole file) and shows them; Esc stops a long one.
static void checksums(void) {
    uint32_t lo, hi;
    if (!sel_range(&lo, &hi))
        lo = 0, hi = F->size;
    // Byte-at-a-time tables for the CRCs, while they are worked out.
    typedef struct {
        uint32_t c32[256];
        uint16_t c16[256], r16[256];
        uint8_t c8[256];
        uint8_t buf[CHUNK];
    } work_t;
    work_t *t = salloc(sizeof *t);
    if (!t) {
        message("Not enough memory for that");
        return;
    }
    for (int i = 0; i < 256; ++i) {
        uint32_t c = i, r = i, f = (uint32_t)i << 8, e = i;
        for (int b = 0; b < 8; ++b) {
            c = c & 1 ? 0xedb88320u ^ c >> 1 : c >> 1;
            r = r & 1 ? 0xa001 ^ r >> 1 : r >> 1;
            f = f & 0x8000 ? (f << 1 ^ 0x1021) & 0xffff : f << 1 & 0xffff;
            e = e & 0x80 ? (e << 1 ^ 0x07) & 0xff : e << 1 & 0xff;
        }
        t->c32[i] = c, t->r16[i] = (uint16_t)r, t->c16[i] = (uint16_t)f, t->c8[i] = (uint8_t)e;
    }
    sums_t k = {lo, hi, 0, 0, 0, 0, 0, 0xffff, 0, 0, 0xffff, 0xffffffffu, 1, 0};
    uint32_t a = 1, b = 0, f1 = 0, f2 = 0;
    bool stopped = false;
    F->read_err = false;
    uint32_t next_show = now_ms() + 300;
    for (uint32_t pos = lo; pos < hi && !stopped; pos += CHUNK) {
        uint32_t n = hi - pos < CHUNK ? hi - pos : CHUNK;
        doc_read(pos, t->buf, NULL, n);
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t c = t->buf[i];
            k.sum32 += c;
            k.xor8 ^= c;
            k.crc8 = t->c8[k.crc8 ^ c];
            k.ccitt = (uint16_t)(k.ccitt << 8 ^ t->c16[(k.ccitt >> 8 ^ c) & 0xff]);
            k.xmodem = (uint16_t)(k.xmodem << 8 ^ t->c16[(k.xmodem >> 8 ^ c) & 0xff]);
            k.arc = (uint16_t)(k.arc >> 8 ^ t->r16[(k.arc ^ c) & 0xff]);
            k.modbus = (uint16_t)(k.modbus >> 8 ^ t->r16[(k.modbus ^ c) & 0xff]);
            k.crc32 = k.crc32 >> 8 ^ t->c32[(k.crc32 ^ c) & 0xff];
            a += c;
            if (a >= 65521)
                a -= 65521;
            b += a;
            if (b >= 65521)
                b -= 65521;
            f1 += c;
            if (f1 >= 255)
                f1 -= 255;
            f2 += f1;
            if (f2 >= 255)
                f2 -= 255;
        }
        if ((int32_t)(now_ms() - next_show) >= 0) {
            progress("Checksums", pos - lo, hi - lo);
            next_show = now_ms() + 300;
            stopped = key_stop();
        }
    }
    free(t);
    F->dirty = true;
    if (stopped) {
        message("Checksums stopped");
        return;
    }
    if (F->read_err)
        message("Reading the file failed: these take those bytes as 00");
    k.sum8 = (uint8_t)k.sum32;
    k.sum16 = (uint16_t)k.sum32;
    k.crc32 ^= 0xffffffffu;
    k.adler = b << 16 | a;
    k.fletcher = (uint16_t)(f2 << 8 | f1);
    F->sums = k;
    F->dlg = DLG_SUMS;
}

/// Draws the About box.
static void draw_about(void) {
    int w = 460, h = 170, x = (W - w) / 2, y = 150;
    box(x, y, w, h, "Hex Editor");
    textf(x + 16, y + 120, C_DIM, C_FACE, "SRAM free %ld KB; edits %d pieces, %lu bytes typed", hex_heap_free() / 1024,
          F->np, (unsigned long)F->addlen);
    static const char *const lines[] = {
        "Edits files of any size up to 4 GB: the file stays on",
        "the drive, read a page at a time; the changes are kept",
        "apart until Save writes the file anew.",
        "Copyright (c) 2026 Ken Segler. MIT licence.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 40 + (int)i * 20, lines[i], i == 3 ? C_LABEL : C_TEXT, C_FACE);
    text(x + 16, y + h - 26, "(any key or click closes this)", C_DIM, C_FACE);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    F->t = F->g ? F->g : dvi_framebuf;
    F->collect = true;
    F->nhits = 0;
    fill(0, 0, W, H, C_BG);
    bevel(4, BAR_H + 2, 632, ROWS * 16 + 28, true, C_FACE);
    hit(HEX_X, ROW_Y, ASC_X - HEX_X - 8, ROWS * 16, HIT_HEX, 0);
    hit(ASC_X, ROW_Y, 128, ROWS * 16, HIT_ASC, 0);
    hit(SB_X, ROW_Y, SB_W, ROWS * 16, HIT_SB, 0);
    bevel(4, INS_Y, 632, 81, true, C_FACE);
    for (int i = 0; i < 8; ++i)
        hit(BITS_X + i * 12 - 2, INS_Y + 22, 12, 18, HIT_BIT, i);
    draw_view();
    switch (F->dlg) {
    case DLG_ASK: draw_ask(); break;
    case DLG_FILE: draw_file_box(); break;
    case DLG_FIND:
    case DLG_GOTO: draw_input_box(); break;
    case DLG_HELP: draw_help(); break;
    case DLG_ABOUT: draw_about(); break;
    case DLG_SUMS: draw_sums(); break;
    }
    if (F->menu >= 0)
        draw_menu();
    F->collect = false;
    F->t = dvi_framebuf;
    F->view_dirty = false;
}

/// Copies the picture onto the screen.
static void show(void) {
    if (F->g)
        memcpy(dvi_framebuf, F->g, W * H);
}

/// Sets a message for the title strip (for 4 seconds).
/// @param fmt printf's format
static void message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(F->msg, sizeof F->msg, fmt, ap);
    va_end(ap);
    F->msg_until = now_ms() + 4000;
    F->view_dirty = true;
}

/// Shows how far a long job has got, straight onto the screen.
/// @param what the job
/// @param done how far
/// @param total of how far
static void progress(const char *what, uint32_t done, uint32_t total) {
    F->t = dvi_framebuf;
    fill(0, STAT_Y, W, H - STAT_Y, C_STRIP);
    textf(8, STAT_Y + 4, C_YELLOW, C_STRIP, "%s %lu%%  (Esc stops)", what,
          (unsigned long)(total ? (uint64_t)done * 100 / total : 100));
}

// ---- The cursor --------------------------------------------------------------------------------

/// Scrolls so the cursor shows.
static void show_cursor(void) {
    uint32_t row = F->cur / 16;
    if (row < F->top)
        F->top = row;
    if (row >= F->top + ROWS)
        F->top = row - ROWS + 1;
    if (F->top > top_max())
        F->top = top_max();
}

/// Moves the cursor.
/// @param pos where to (kept within the document; its size is just after the end)
/// @param extend select from where it was (or from the selection's other end)
static void move_to(int64_t pos, bool extend) {
    if (pos < 0)
        pos = 0;
    if (pos > F->size)
        pos = F->size;
    if (extend && !F->sel)
        F->sel = true, F->anchor = F->cur;
    else if (!extend)
        F->sel = false;
    F->cur = (uint32_t)pos;
    F->lo = F->nib = F->typing = false;
    show_cursor();
    F->view_dirty = true;
}

/// Scrolls the rows.
/// @param rows how many (up if less than 0)
static void scroll(int64_t rows) {
    int64_t t = (int64_t)F->top + rows;
    F->top = t < 0 ? 0 : t > top_max() ? top_max() : (uint32_t)t;
    F->view_dirty = true;
}

// ---- Editing -----------------------------------------------------------------------------------

/// Deletes the selection, if there is one.
/// @return there was one
static bool delete_selection(void) {
    uint32_t lo, hi;
    if (!sel_range(&lo, &hi))
        return false;
    if (edit(lo, hi - lo, NULL, 0, false)) {
        F->sel = false;
        F->cur = lo;
        show_cursor();
    }
    return true;
}

/// Puts a byte at the cursor (over the one there, or in front of it).
/// @param pos where
/// @param b the byte
/// @param over overwrite (else insert)
/// @return it was done
static bool put_byte(uint32_t pos, uint8_t b, bool over) {
    if (!add_room(1)) {
        message("Too much typed: save, and it goes on from there");
        return false;
    }
    F->add[F->addlen] = b;
    gtouch(&F->add[F->addlen], 1);
    piece_t p = {F->addlen++, 1, SRC_ADD};
    if (!edit(pos, over && pos < F->size ? 1 : 0, &p, 1, true)) {
        --F->addlen;
        return false;
    }
    return true;
}

/// A hex digit typed: the first sets the byte's top half (inserting a byte, in insert mode), the second its
/// bottom half; then the cursor moves on.
/// @param d the digit (0-15)
static void type_hex(int d) {
    if (delete_selection())
        F->typing = false;
    uint32_t c = F->cur;
    if (F->nib) {  // the second digit: changes the byte the first one made
        uint32_t s;
        int i = piece_at(c, &s);
        if (i < F->np && F->pc[i].src == SRC_ADD && F->pc[i].off + (c - s) == F->addlen - 1) {
            uint8_t *b = &F->add[F->addlen - 1];
            *b = (uint8_t)((*b & 0xf0) | d);
            gtouch(b, 1);
            F->nib = F->lo = false;
            F->cur = c + 1;
            show_cursor();
            F->view_dirty = true;
            return;
        }
    }
    bool have = c < F->size, low = F->lo || F->nib;
    uint8_t old = have ? doc_byte(c) : 0;
    if (low) {
        if (!put_byte(c, have ? (uint8_t)((old & 0xf0) | d) : (uint8_t)d, true))
            return;
        F->lo = F->nib = false;
        F->cur = c + 1;
    } else {
        bool over = !F->ins && have;
        if (!put_byte(c, over ? (uint8_t)((old & 0x0f) | d << 4) : (uint8_t)(d << 4), over))
            return;
        F->nib = true;
    }
    show_cursor();
}

/// A character typed in the text column.
/// @param k the character
static void type_text(uint8_t k) {
    if (delete_selection())
        F->typing = false;
    if (put_byte(F->cur, k, !F->ins)) {
        ++F->cur;
        F->nib = F->lo = false;
        show_cursor();
    }
}

/// Flips a bit of the byte at the cursor.
/// @param i the bit (0: the top one)
static void flip_bit(int i) {
    if (F->cur >= F->size)
        return;
    F->typing = false;
    if (put_byte(F->cur, doc_byte(F->cur) ^ (0x80 >> i), true))
        F->typing = false;
}

/// Del: the selection, or the byte at the cursor.
static void del_key(void) {
    F->nib = F->lo = false;
    if (delete_selection() || F->cur >= F->size)
        return;
    edit(F->cur, 1, NULL, 0, true);
}

/// Backspace: the selection; in insert mode the byte before the cursor; else back a byte.
static void backspace_key(void) {
    F->nib = F->lo = false;
    if (delete_selection() || !F->cur)
        return;
    if (!F->ins) {
        move_to(F->cur - 1, false);
        return;
    }
    if (edit(F->cur - 1, 1, NULL, 0, false)) {
        --F->cur;
        show_cursor();
    }
}

/// Selects the whole document.
static void select_all(void) {
    F->sel = true;
    F->anchor = 0;
    F->cur = F->size;
    F->lo = F->nib = F->typing = false;
    show_cursor();
    F->view_dirty = true;
}

/// Copies the selection to the clipboard (as pieces: nothing is read).
/// @return there was one
static bool copy_selection(void) {
    uint32_t lo, hi;
    if (!sel_range(&lo, &hi)) {
        message("Nothing selected (Shift and the arrows, or drag with the mouse)");
        return false;
    }
    int n;
    piece_t *p = pieces_of(lo, hi - lo, &n);
    if (!p) {
        message("Not enough memory to copy that");
        return false;
    }
    gfree(F->clip);
    F->clip = p;
    F->nclip = n;
    F->cliplen = hi - lo;
    message("Copied %lu byte%s", (unsigned long)F->cliplen, F->cliplen == 1 ? "" : "s");
    return true;
}

/// Pastes the clipboard over the selection, or at the cursor (over the bytes there, in overwrite mode).
static void paste(void) {
    if (!F->nclip) {
        message("The clipboard is empty");
        return;
    }
    uint32_t lo, hi, at = F->cur, len = 0;
    if (sel_range(&lo, &hi))
        at = lo, len = hi - lo;
    else if (!F->ins)
        len = F->size - at < F->cliplen ? F->size - at : F->cliplen;
    if (edit(at, len, F->clip, F->nclip, false)) {
        F->sel = F->nib = F->lo = false;
        F->cur = at + F->cliplen;
        show_cursor();
    }
}

/// Reads the clipboard's bytes into SRAM before the file it comes from goes (if it is small enough).
/// @param len set to its length
/// @return them (malloc), or NULL: the clipboard is emptied
static uint8_t *clip_detach(uint32_t *len) {
    uint8_t *b = F->cliplen && F->cliplen <= CLIP_KEEP ? psram_buffer_alloc(F->cliplen) : NULL;  // moments
    *len = 0;
    if (b) {
        for (int i = 0, o = 0; i < F->nclip; o += F->clip[i++].len)
            piece_read(&F->clip[i], 0, b + o, F->clip[i].len);
        *len = F->cliplen;
    }
    gfree(F->clip);
    F->clip = NULL;
    F->nclip = 0;
    F->cliplen = 0;
    return b;
}

/// Puts bytes from clip_detach back as the clipboard, among the bytes typed.
/// @param b them (freed)
/// @param len their length
static void clip_attach(uint8_t *b, uint32_t len) {
    if (b && add_room(len) && (F->clip = galloc(sizeof *F->clip))) {
        memcpy(F->add + F->addlen, b, len);
        gtouch(F->add + F->addlen, len);
        *F->clip = (piece_t){F->addlen, len, SRC_ADD};
        F->addlen += len;
        F->nclip = 1;
        F->cliplen = len;
    }
    psram_buffer_free(b);
}

// ---- Files ----------------------------------------------------------------------------------------

static void doc_reset(void) {
    undo_forget();
    F->np = 0;
    F->addlen = 0;
    F->size = 0;
    if (F->file_open && F->fsize && pieces_room(1)) {
        F->pc[0] = (piece_t){0, F->fsize, SRC_FILE};
        gtouch(F->pc, sizeof *F->pc);
        F->np = 1;
        F->size = F->fsize;
    }
    F->hint_i = 0, F->hint_s = 0;
    F->cur = F->top = 0;
    F->sel = F->lo = F->nib = false;
    F->read_err = false;
    F->modified = false;
    F->damaged = false;
}

/// Closes the file the document is made from.
static void close_source(void) {
    if (F->file_open)
        f_close(&F->file);
    F->file_open = false;
    F->fsize = 0;
    F->src[0] = 0;
    cache_clear();
}

/// Opens a file as the document, in place of the one there. The clipboard must be detached first
/// (clip_detach): it may hold pieces of the file closed.
/// @param name the file
/// @param err set to why, on failure
/// @return it was opened (else the document is as it was)
static bool open_source(const char *name, const char **err) {
    char path[200];  // a copy: it may be F->src, which closing clears
    snprintf(path, sizeof path, "%s", name);
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) {
        *err = "can't find it";
        return false;
    }
    if (fi.fattrib & AM_DIR) {
        *err = "it is a folder";
        return false;
    }
    if (fi.fsize > 0xfffffff0u) {
        *err = "it is too big (4 GB at most)";
        return false;
    }
    if (fi.fsize && !pieces_room(1)) {
        *err = "not enough memory";
        return false;
    }
    close_source();
    if (f_open(&F->file, path, FA_READ) != FR_OK) {
        *err = "can't open it";
        doc_reset();
        return false;
    }
    F->file_open = true;
    F->fsize = (uint32_t)f_size(&F->file);
    snprintf(F->src, sizeof F->src, "%s", path);
    doc_reset();
    return true;
}

/// Makes a path whole: one not starting with / goes in the shell's folder.
/// @param out the path
/// @param size room in out
/// @param path as given
static void whole_path(char *out, int size, const char *path) {
    char cwd[160];
    if (path[0] == '/' || strchr(path, ':') || f_getcwd(cwd, sizeof cwd) != FR_OK)
        snprintf(out, size, "%s", path);
    else
        snprintf(out, size, "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", path);
}

/// Loads a file in place of the document.
/// @param path the file
/// @return it loaded
static bool load(const char *path) {
    char p[200];
    whole_path(p, sizeof p, path);
    const char *err = "?";
    FILINFO fi;
    if (f_stat(p, &fi) != FR_OK || (fi.fattrib & AM_DIR)) {  // the usual failures, before the clipboard goes
        message("%.40s: %s", path, fi.fattrib & AM_DIR ? "it is a folder" : "can't find it");
        return false;
    }
    uint32_t cl;
    uint8_t *cb = clip_detach(&cl);
    bool ok = open_source(p, &err);
    clip_attach(cb, cl);
    if (!ok) {
        message("%.40s: %s", path, err);
        return false;
    }
    snprintf(F->path, sizeof F->path, "%s", p);
    message("%s: %lu bytes", file_name(), (unsigned long)F->size);
    F->dirty = true;
    return true;
}

/// Writes the document to a file, which becomes the one it is made from. Writing over the file it is made
/// from goes to a new file first, which then takes its place.
/// @param path the file
/// @return it was written
static bool write_file(const char *name) {
    char path[200];  // a copy: it may be F->path, which changes on the way
    snprintf(path, sizeof path, "%s", name);
    bool same = F->file_open && !strcasecmp(path, F->src);
    char tmp[220];
    const char *slash = strrchr(path, '/');
    snprintf(tmp, sizeof tmp, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, TMP_NAME);
    const char *out = same ? tmp : path;
    if (same && F->damaged) {
        message("The edits may be damaged: save as a new file");
        return false;
    }
    FIL o;
    if (f_open(&o, out, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        message("Can't write %.50s", out);
        return false;
    }
    uint8_t small[PAGE], *buf = salloc(CHUNK);
    uint32_t step = buf ? CHUNK : PAGE;
    if (!buf)
        buf = small;
    bool ok = true, stopped = false;
    F->read_err = false;
    uint32_t next_show = now_ms() + 300;
    for (uint32_t pos = 0; ok && pos < F->size; pos += step) {
        uint32_t n = F->size - pos < step ? F->size - pos : step;
        UINT put = 0;
        doc_read(pos, buf, NULL, n);
        ok = !F->read_err && f_write(&o, buf, n, &put) == FR_OK && put == n;
        if ((int32_t)(now_ms() - next_show) >= 0) {
            progress("Saving", pos, F->size);
            next_show = now_ms() + 300;
            if (key_stop())
                ok = false, stopped = true;
        }
    }
    if (buf != small)
        free(buf);
    if (f_close(&o) != FR_OK)
        ok = false;
    if (!ok) {
        f_unlink(out);
        message(stopped ? "Save stopped: the file is as it was" : F->read_err ? "Reading the file failed: not saved"
                                                                            : "Writing %.40s failed (drive full?)",
                out);
        F->dirty = true;
        return false;
    }
    uint32_t cl;
    uint8_t *cb = clip_detach(&cl);  // while the file it may come from is still there
    if (same) {  // the new file takes the old one's place
        f_close(&F->file);
        if (f_unlink(path) != FR_OK) {
            f_unlink(tmp);
            f_open(&F->file, F->src, FA_READ);
            cache_clear();
            clip_attach(cb, cl);
            message("Can't replace %.40s (read-only?)", path);
            return false;
        }
        F->file_open = false;
        if (f_rename(tmp, path) != FR_OK)
            snprintf(path, sizeof path, "%s", tmp);
    }
    const char *err = "?";
    if (!open_source(path, &err)) {
        close_source();
        doc_reset();
        message("Saved, but can't open it again: %s", err);
    } else if (strcmp(path, name)) {
        message("Saved, but as %s: renaming it failed", TMP_NAME);
    } else {
        message("Saved %.40s, %lu bytes", path, (unsigned long)F->size);
    }
    clip_attach(cb, cl);
    snprintf(F->path, sizeof F->path, "%s", path);
    F->modified = false;
    F->dirty = true;
    return true;
}

/// Starts afresh: an empty, untitled document.
static void new_doc(void) {
    uint32_t cl;
    uint8_t *cb = clip_detach(&cl);
    close_source();
    doc_reset();
    clip_attach(cb, cl);
    F->path[0] = 0;
    F->dirty = true;
}

// ---- Find and Goto ---------------------------------------------------------------------------------

/// Whether bytes match what Find looks for.
/// @param b the bytes (pat_n of them)
/// @return they match
static bool matches(const uint8_t *b) {
    if (!F->pat_text || F->match_case)
        return !memcmp(b, F->pat, F->pat_n);
    for (int i = 0; i < F->pat_n; ++i)
        if (tolower(b[i]) != tolower(F->pat[i]))
            return false;
    return true;
}

/// Looks for the pattern starting in [from, to).
/// @param from the first place
/// @param to after the last
/// @param at set to where it is
/// @param stopped set if Esc stopped it
/// @return found
static bool search(uint32_t from, uint32_t to, uint32_t *at, bool *stopped) {
    uint8_t small[PAGE + PAT_MAX], *buf = salloc(CHUNK + PAT_MAX);
    uint32_t step = buf ? CHUNK : PAGE;
    if (!buf)
        buf = small;
    bool found = false;
    uint32_t next_show = now_ms() + 300;
    for (uint32_t pos = from; !found && pos < to && pos + F->pat_n <= F->size; pos += step) {
        uint32_t n = F->size - pos < step + F->pat_n - 1 ? F->size - pos : step + F->pat_n - 1;
        doc_read(pos, buf, NULL, n);
        for (uint32_t i = 0; i + F->pat_n <= n && i < step && pos + i < to; ++i)
            if (buf[i] == F->pat[0] || (F->pat_text && !F->match_case && tolower(buf[i]) == tolower(F->pat[0])))
                if (matches(buf + i)) {
                    *at = pos + i;
                    found = true;
                    break;
                }
        if (!found && (int32_t)(now_ms() - next_show) >= 0) {
            progress("Searching", pos - from, to - from);
            next_show = now_ms() + 300;
            if (key_stop()) {
                *stopped = true;
                break;
            }
        }
    }
    if (buf != small)
        free(buf);
    return found;
}

/// Finds the pattern after the cursor (going round to the start), and selects it.
static void find_next(void) {
    if (!F->pat_n) {
        F->dlg = DLG_FIND;
        F->dirty = true;
        return;
    }
    uint32_t at = 0, lo, hi;
    uint32_t from = sel_range(&lo, &hi) ? lo + 1 : F->cur + (F->cur < F->size);
    bool stopped = false;
    bool found = search(from, F->size, &at, &stopped);
    bool round = false;
    if (!found && !stopped && from)
        found = round = search(0, from + F->pat_n - 1, &at, &stopped);
    F->dirty = true;
    if (stopped) {
        message("Find stopped");
    } else if (!found) {
        message("Not found");
    } else {
        F->sel = true;
        F->anchor = at;
        F->cur = at + F->pat_n;
        F->lo = F->nib = F->typing = false;
        F->top = at / 16 > 2 ? at / 16 - 2 : 0;  // a little above it
        if (F->top > top_max())
            F->top = top_max();
        show_cursor();
        message("Found at %08lX%s", (unsigned long)at, round ? " (went round to the start)" : "");
    }
}

/// The value of a hex digit.
/// @param c the character
/// @return 0-15, or -1
static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/// The Find box's Enter: takes what was typed as the pattern and looks for it.
static void find_ok(void) {
    int n = 0;
    if (F->in_text) {
        for (const char *s = F->in; *s && n < PAT_MAX; ++s)
            F->pat[n++] = (uint8_t)*s;
    } else {
        int d = -1;
        for (const char *s = F->in; *s; ++s) {
            int v = hexval(*s);
            if (*s == ' ' || *s == ',')
                continue;
            if (v < 0 || (d < 0 && n == PAT_MAX)) {
                message("Hex bytes are pairs of 0-9 and A-F, e.g. DE AD BE EF");
                return;
            }
            if (d < 0)
                d = v;
            else
                F->pat[n++] = (uint8_t)(d << 4 | v), d = -1;
        }
        if (d >= 0) {
            message("An odd number of hex digits: each byte is two");
            return;
        }
    }
    if (!n) {
        message("Type what to look for");
        return;
    }
    F->pat_n = n;
    F->pat_text = F->in_text;
    F->dlg = DLG_NONE;
    find_next();
}

/// The Goto box's Enter: moves the cursor to the offset typed.
static void goto_ok(void) {
    const char *s = F->in;
    while (*s == ' ')
        ++s;
    int sign = *s == '+' ? 1 : *s == '-' ? -1 : 0;
    if (sign)
        ++s;
    bool hex = !F->in_text;
    if (*s == '$' || *s == '&')
        hex = true, s += 1 + (s[1] == 'h' || s[1] == 'H');
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        hex = true, s += 2;
    uint64_t v = 0;
    int digits = 0;
    for (; *s && *s != ' '; ++s, ++digits) {
        int d = hex ? hexval(*s) : *s >= '0' && *s <= '9' ? *s - '0' : -1;
        if (d < 0 || v > 0xffffffffu) {
            message(hex ? "An offset in hex: 0-9 and A-F" : "An offset in decimal: 0-9");
            return;
        }
        v = v * (hex ? 16 : 10) + d;
    }
    if (!digits) {
        message("Type an offset");
        return;
    }
    int64_t to = sign ? (int64_t)F->cur + sign * (int64_t)v : (int64_t)v;
    F->dlg = DLG_NONE;
    F->dirty = true;
    move_to(to, false);
    if (to > F->size)
        message("The file is only %lu bytes: at its end", (unsigned long)F->size);
}

/// Opens the Find box (with the selection in it, if it is short) or the Goto box.
/// @param dlg DLG_FIND or DLG_GOTO
static void input_open(int dlg) {
    F->dlg = dlg;
    F->in[0] = 0;
    uint32_t lo, hi;
    if (dlg == DLG_FIND) {
        F->in_text = F->pat_n ? F->pat_text : false;
        if (sel_range(&lo, &hi) && hi - lo <= 32) {
            F->in_text = false;
            for (uint32_t p = lo; p < hi; ++p)
                snprintf(F->in + strlen(F->in), sizeof F->in - strlen(F->in), "%s%02X", p > lo ? " " : "",
                         doc_byte(p));
        } else if (F->pat_n) {
            for (int i = 0; i < F->pat_n; ++i) {
                size_t n = strlen(F->in);
                if (F->pat_text)
                    snprintf(F->in + n, sizeof F->in - n, "%c", F->pat[i]);
                else
                    snprintf(F->in + n, sizeof F->in - n, "%s%02X", i ? " " : "", F->pat[i]);
            }
        }
    } else {
        F->in_text = false;
    }
    F->dirty = true;
}

// ---- The file box ---------------------------------------------------------------------------------

/// Sorts folders first, then by name.
/// @param a an entry
/// @param b another
/// @return their order
static int fd_compare(const void *a, const void *b) {
    const fd_entry_t *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(F->fd_pool + x->name, F->fd_pool + y->name);
}

/// Reads the folder shown into the list.
static void fd_read(void) {
    F->fd_n = F->fd_sel = F->fd_top = 0;
    DIR d;
    if (f_opendir(&d, F->fd_dir) != FR_OK) {
        message("Can't open %.60s", F->fd_dir);
        return;
    }
    FILINFO fi;
    int used = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] && F->fd_n < MAX_FILES) {
        int n = strlen(fi.fname) + 1;
        if (fi.fname[0] == '.' || used + n > POOL)
            continue;
        fd_entry_t *e = &F->fd[F->fd_n++];
        memcpy(F->fd_pool + used, fi.fname, n);
        e->name = (uint16_t)used;
        e->dir = fi.fattrib & AM_DIR;
        e->size = (uint32_t)fi.fsize;
        used += n;
    }
    f_closedir(&d);
    qsort(F->fd, F->fd_n, sizeof *F->fd, fd_compare);
}

/// Joins a folder and a name.
/// @param out the path
/// @param size room in out
/// @param dir the folder
/// @param name the name (one starting with / stands alone)
static void join(char *out, int size, const char *dir, const char *name) {
    if (name[0] == '/')
        snprintf(out, size, "%s", name);
    else
        snprintf(out, size, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

/// Closes the file box.
static void fd_close(void) {
    if (F->fd_psram)
        psram_free(F->fd);
    else
        free(F->fd);
    F->fd = NULL;
    F->fd_pool = NULL;
    F->fd_n = 0;
    if (F->dlg == DLG_FILE)
        F->dlg = DLG_NONE;
    F->dirty = true;
}

/// Opens the file box.
/// @param mode FD_*
static void fd_open(int mode) {
    // The list and its names: SRAM if there is room, else PSRAM (only while the box is up).
    size_t need = MAX_FILES * sizeof *F->fd + POOL;
    F->fd_psram = false;
    if (!(F->fd = salloc(need)) && (F->fd = psram_malloc(need)))
        F->fd_psram = true;
    if (!F->fd) {
        message("Not enough memory for the file box (%ld KB SRAM free)", hex_heap_free() / 1024);
        return;
    }
    F->fd_pool = (char *)(F->fd + MAX_FILES);
    F->fd_mode = mode;
    F->dlg = DLG_FILE;
    // Start in the file's folder, or the shell's.
    const char *slash = strrchr(F->path, '/');
    if (slash && slash != F->path)
        snprintf(F->fd_dir, sizeof F->fd_dir, "%.*s", (int)(slash - F->path), F->path);
    else if (slash)
        strcpy(F->fd_dir, "/");
    else if (f_getcwd(F->fd_dir, sizeof F->fd_dir) != FR_OK)
        strcpy(F->fd_dir, "/");
    F->fd_name[0] = 0;
    if (mode == FD_SAVE && F->path[0])
        snprintf(F->fd_name, sizeof F->fd_name, "%s", file_name());
    fd_read();
    F->dirty = true;
}

/// Goes into a folder of the list, or up.
/// @param name the folder, or ".." for up
static void fd_go(const char *name) {
    if (!strcmp(name, "..")) {
        char *s = strrchr(F->fd_dir, '/');
        if (s && s != F->fd_dir)
            *s = 0;
        else
            strcpy(F->fd_dir, "/");
    } else {
        char p[200];
        join(p, sizeof p, F->fd_dir, name);
        snprintf(F->fd_dir, sizeof F->fd_dir, "%s", p);
    }
    fd_read();
    F->dirty = true;
}

static void act(int a);

/// Asks a question; each button does an ACT_.
/// @param q the question
/// @param b0 the first button (Enter)
/// @param a0 what it does
/// @param b1 the second
/// @param a1 what it does
/// @param b2 a third, or NULL
/// @param a2 what it does
static void ask(const char *q, const char *b0, int a0, const char *b1, int a1, const char *b2, int a2) {
    snprintf(F->ask, sizeof F->ask, "%s", q);
    F->ask_btn[0] = b0, F->ask_act[0] = a0;
    F->ask_btn[1] = b1, F->ask_act[1] = a1;
    F->ask_btn[2] = b2, F->ask_act[2] = a2;
    F->dlg = DLG_ASK;
    F->dirty = true;
}

/// The file box's OK: loads, or writes (asking first if that replaces another file).
static void fd_ok(void) {
    if (!F->fd_name[0]) {
        if (F->fd_sel < F->fd_n && F->fd[F->fd_sel].dir)
            fd_go(F->fd_pool + F->fd[F->fd_sel].name);
        return;
    }
    char p[200];
    join(p, sizeof p, F->fd_dir, F->fd_name);
    FILINFO fi;
    if (f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR)) {
        F->fd_name[0] = 0;
        fd_go(p);
        return;
    }
    int mode = F->fd_mode;
    fd_close();
    if (mode == FD_LOAD) {
        load(p);
        return;
    }
    snprintf(F->target, sizeof F->target, "%s", p);
    if (f_stat(p, &fi) == FR_OK && strcasecmp(p, F->src)) {
        char q[96];
        const char *n = strrchr(p, '/');
        snprintf(q, sizeof q, "%.40s is there already: replace it?", n ? n + 1 : p);
        ask(q, "Replace", ACT_WRITE, "Cancel", ACT_NONE, NULL, ACT_NONE);
        return;
    }
    act(ACT_WRITE);
}

/// Does something.
/// @param a ACT_*
static void act(int a) {
    F->dlg = DLG_NONE;
    F->dirty = true;
    switch (a) {
    case ACT_NONE: F->then = ACT_NONE; break;
    case ACT_NEW: new_doc(); break;
    case ACT_LOAD: fd_open(FD_LOAD); break;
    case ACT_QUIT: F->quit = true; break;
    case ACT_SAVE_THEN:
        if (!F->path[0] || F->damaged) {
            fd_open(FD_SAVE);  // then F->then, after the save
            return;
        }
        if (write_file(F->path))
            act(F->then);
        F->then = ACT_NONE;
        break;
    case ACT_WRITE:
        if (write_file(F->target) && F->then != ACT_NONE) {
            int t = F->then;
            F->then = ACT_NONE;
            act(t);
        }
        break;
    }
}

/// Does something that would lose the changes, asking to save them first.
/// @param a ACT_NEW, ACT_LOAD or ACT_QUIT
static void act_unsaved(int a) {
    F->then = ACT_NONE;
    if (!F->modified) {
        act(a);
        return;
    }
    char q[96];
    snprintf(q, sizeof q, "Save the changes to %.40s first?", file_name());
    F->then = a;
    ask(q, "Save", ACT_SAVE_THEN, "Don't save", a, "Cancel", ACT_NONE);
}

static void del_key(void);
static void paste(void);
static bool copy_selection(void);
static bool delete_selection(void);
static void find_next(void);
static void select_all(void);

/// Does a command from a menu or a key.
/// @param id CMD_*
static void command(int id) {
    switch (id) {
    case CMD_NEW: act_unsaved(ACT_NEW); break;
    case CMD_LOAD: act_unsaved(ACT_LOAD); break;
    case CMD_SAVE: F->then = ACT_NONE; act(ACT_SAVE_THEN); break;
    case CMD_SAVEAS: F->then = ACT_NONE; fd_open(FD_SAVE); break;
    case CMD_QUIT: act_unsaved(ACT_QUIT); break;
    case CMD_UNDO: undo_step(false); break;
    case CMD_REDO: undo_step(true); break;
    case CMD_CUT:
        if (copy_selection())
            delete_selection();
        break;
    case CMD_COPY: copy_selection(); break;
    case CMD_PASTE: paste(); break;
    case CMD_DELETE: del_key(); break;
    case CMD_SELALL: select_all(); break;
    case CMD_INSERT: F->ins = !F->ins; break;
    case CMD_TEXT: F->text = !F->text; F->lo = F->nib = false; break;
    case CMD_FIND: input_open(DLG_FIND); break;
    case CMD_NEXT: find_next(); break;
    case CMD_GOTO: input_open(DLG_GOTO); break;
    case CMD_HELP: F->dlg = DLG_HELP; break;
    case CMD_ABOUT: F->dlg = DLG_ABOUT; break;
    case CMD_SUMS: checksums(); break;
    }
    F->dirty = true;
}

// ---- Keys and the mouse ------------------------------------------------------------------------------

/// A key while the file box is up.
/// @param k the key
static void fd_key(int k) {
    int n = strlen(F->fd_name);
    switch (k) {
    case '\r': case '\n': fd_ok(); break;
    case KEY_ESC: fd_close(); F->then = ACT_NONE; break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -FD_ROWS : FD_ROWS;
        F->fd_sel += d;
        if (F->fd_sel < 0)
            F->fd_sel = 0;
        if (F->fd_sel >= F->fd_n)
            F->fd_sel = F->fd_n - 1;
        if (F->fd_sel >= 0 && F->fd_sel < F->fd_n)
            snprintf(F->fd_name, sizeof F->fd_name, "%s",
                     F->fd[F->fd_sel].dir ? "" : F->fd_pool + F->fd[F->fd_sel].name);
        break;
    }
    case 8: case 0x7f:
        if (n)
            F->fd_name[n - 1] = 0;
        else
            fd_go("..");
        break;
    default:
        if (k >= 0x20 && k < 0x7f && n < (int)sizeof F->fd_name - 1) {
            F->fd_name[n] = (char)k;
            F->fd_name[n + 1] = 0;
        }
        break;
    }
}

/// A key while the Find or Goto box is up.
/// @param k the key
static void input_key(int k) {
    int n = strlen(F->in);
    switch (k) {
    case '\r': case '\n': F->dlg == DLG_FIND ? find_ok() : goto_ok(); break;
    case KEY_ESC: F->dlg = DLG_NONE; break;
    case '\t': F->in_text = !F->in_text; break;
    case 8: case 0x7f:
        if (n)
            F->in[n - 1] = 0;
        break;
    default:
        if (k >= 0x20 && k < 0x7f && n < (F->dlg == DLG_FIND && F->in_text ? PAT_MAX : (int)sizeof F->in - 1)) {
            F->in[n] = (char)k;
            F->in[n + 1] = 0;
        }
        break;
    }
}

/// Whether Shift is held.
/// @return it is
static bool shift_held(void) {
    return (usb_keyboard_modifiers() & SHIFT_BITS) != 0;
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    if (F->menu >= 0) {
        menu_key(k);
        return;
    }
    switch (F->dlg) {
    case DLG_HELP: case DLG_ABOUT: case DLG_SUMS: F->dlg = DLG_NONE; F->dirty = true; return;
    case DLG_FILE: fd_key(k); F->dirty = true; return;
    case DLG_FIND: case DLG_GOTO: input_key(k); F->dirty = true; return;
    case DLG_ASK:
        if (k == '\r' || k == '\n')
            act(F->ask_act[0]);
        else if (k == KEY_ESC)
            F->dlg = DLG_NONE, F->then = ACT_NONE;
        F->dirty = true;
        return;
    }
    if (F->mdrag)
        return;
    bool sh = shift_held();
    uint32_t c = F->cur, row0 = c - c % 16;
    F->view_dirty = true;
    switch (k) {
    case KEY_LEFT: move_to((int64_t)c - 1, sh); break;
    case KEY_RIGHT: move_to((int64_t)c + 1, sh); break;
    case KEY_UP: move_to(c >= 16 ? (int64_t)c - 16 : c, sh); break;
    case KEY_DOWN: move_to(c + 16 <= F->size ? (int64_t)c + 16 : c, sh); break;
    case KEY_PGUP:
        scroll(-ROWS);
        move_to(c >= 16 * ROWS ? (int64_t)c - 16 * ROWS : c % 16, sh);
        break;
    case KEY_PGDN:
        scroll(ROWS);
        move_to((int64_t)c + 16 * ROWS <= F->size ? (int64_t)c + 16 * ROWS : F->size, sh);
        break;
    case KEY_HOME: move_to(c == row0 ? 0 : row0, sh); break;
    case KEY_END: {
        uint32_t end = row0 + 15 < F->size ? row0 + 15 : F->size;
        move_to(c == end ? F->size : end, sh);
        break;
    }
    case '\t': command(CMD_TEXT); break;
    case KEY_INS: command(CMD_INSERT); break;
    case KEY_DEL: del_key(); break;
    case 8: case 0x7f: backspace_key(); break;
    case CTRL('a'): select_all(); break;
    case CTRL('c'): command(CMD_COPY); break;
    case CTRL('x'): command(CMD_CUT); break;
    case CTRL('v'): command(CMD_PASTE); break;
    case CTRL('z'): command(CMD_UNDO); break;
    case CTRL('y'): command(CMD_REDO); break;
    case CTRL('s'): case KEY_F2: command(CMD_SAVE); break;
    case CTRL('o'): command(CMD_LOAD); break;
    case CTRL('n'): command(CMD_NEW); break;
    case CTRL('f'): command(CMD_FIND); break;
    case CTRL('g'): command(CMD_GOTO); break;
    case CTRL('k'): command(CMD_SUMS); break;
    case KEY_F3: command(CMD_NEXT); break;
    case KEY_F1: command(CMD_HELP); break;
    case KEY_F9: case KEY_F10: menu_open(0, false); break;
    case KEY_ESC: command(CMD_QUIT); break;
    default:
        if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
            for (int i = 0; i < NMENUS; ++i)
                if ((menus[i].title[0] | 0x20) == k - KEY_ALT)
                    menu_open(i, false);
        } else if (F->text && k >= 0x20 && k < 0x7f) {
            type_text((uint8_t)k);
        } else if (!F->text && hexval(k) >= 0) {
            type_hex(hexval(k));
        }
        break;
    }
}

/// The byte (and hex digit) under a point in the rows.
/// @param x the point's x
/// @param y its y
/// @param text in the text column (else hex)
/// @param lo set to whether it is on a byte's second hex digit (may be NULL)
/// @return the byte (kept within the document)
static uint32_t byte_at(int x, int y, bool text, bool *lo) {
    int r = (y - ROW_Y) / 16, c;
    if (y < ROW_Y)
        r = -1;
    if (r >= ROWS)
        r = ROWS;
    if (text) {
        c = (x - ASC_X) / 8;
    } else {
        int dx = x - HEX_X;
        if (dx >= 8 * 24)
            dx -= 8;
        c = dx / 24;
        if (lo)
            *lo = dx % 24 >= 8;
    }
    c = c < 0 ? 0 : c > 15 ? 15 : c;
    int64_t pos = ((int64_t)F->top + r) * 16 + c;
    return pos < 0 ? 0 : pos > F->size ? F->size : (uint32_t)pos;
}

/// The pointer moved while selecting: the selection runs from the byte it began on to the one under it.
/// @param x the pointer's x
/// @param y its y
static void drag_select(int x, int y) {
    uint32_t b = byte_at(x, y, F->text, NULL), a = F->drag_a;
    if (b == a && !F->drag_moved)
        return;
    F->drag_moved = true;
    uint32_t last = F->size ? F->size - 1 : 0;
    if (a > last)
        a = last;
    if (b > last)
        b = last;
    F->sel = true;
    if (b >= a)
        F->anchor = a, F->cur = b + 1;
    else
        F->anchor = a + 1, F->cur = b;
    F->lo = F->nib = F->typing = false;
    F->view_dirty = true;
}

/// The scroll bar's thumb dragged to a point.
/// @param y the pointer's y
static void drag_thumb(int y) {
    uint32_t rows = total_rows();
    if (rows <= ROWS)
        return;
    int sh = ROWS * 16, th = (int)((uint64_t)sh * ROWS / rows);
    if (th < 16)
        th = 16;
    int t = y - ROW_Y - F->drag_dy;
    t = t < 0 ? 0 : t > sh - th ? sh - th : t;
    F->top = (uint32_t)((uint64_t)t * top_max() / (sh - th));
    F->view_dirty = true;
}

static void menu_mouse(const mouse_event_t *e);

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    F->mx = e->x, F->my = e->y;
    if (F->menu >= 0) {
        menu_mouse(e);
        return;
    }
    if (e->type == MOUSE_UP) {
        F->mdrag = 0;
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (F->mdrag == 1)
            drag_select(e->x, e->y);
        else if (F->mdrag == 2)
            drag_thumb(e->y);
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type == MOUSE_WHEEL) {
        int d = e->wheel > 0 ? -1 : 1;
        if (F->dlg == DLG_FILE) {
            F->fd_sel += d * 3;
            F->fd_sel = F->fd_sel < 0 ? 0 : F->fd_sel >= F->fd_n ? F->fd_n - 1 : F->fd_sel;
            F->dirty = true;
        } else if (!F->dlg) {
            scroll(d * 3);
        }
        return;
    }
    if (e->type != MOUSE_DOWN || !h || F->mdrag)
        return;
    if (e->button == MOUSE_RIGHT && !F->dlg && (h->id == HIT_HEX || h->id == HIT_ASC)) {
        F->popx = e->x, F->popy = e->y;
        menu_open(NMENUS, true);
        return;
    }
    switch (h->id) {
    case HIT_MENU: menu_open(h->arg, true); break;
    case HIT_HEX:
    case HIT_ASC: {
        bool lo = false, text = h->id == HIT_ASC;
        uint32_t pos = byte_at(e->x, e->y, text, &lo);
        F->text = text;
        if (shift_held()) {
            move_to(pos, true);
            break;
        }
        move_to(pos, false);
        F->lo = !text && lo && pos < F->size;
        F->mdrag = 1;
        F->drag_a = pos;
        F->drag_moved = false;
        break;
    }
    case HIT_SB: {
        uint32_t rows = total_rows();
        int sh = ROWS * 16, th = sh, ty = 0;
        if (rows > ROWS) {
            th = (int)((uint64_t)sh * ROWS / rows);
            if (th < 16)
                th = 16;
            ty = (int)((uint64_t)(sh - th) * F->top / top_max());
        }
        int y = e->y - ROW_Y;
        if (y < ty) {
            scroll(-ROWS);
        } else if (y >= ty + th) {
            scroll(ROWS);
        } else {
            F->mdrag = 2;
            F->drag_dy = y - ty;
        }
        break;
    }
    case HIT_BIT: flip_bit(h->arg); break;
    case HIT_MODE:
        if (h->arg)
            F->text = !F->text, F->lo = F->nib = false;
        else
            F->ins = !F->ins;
        F->view_dirty = true;
        break;
    case HIT_ASK: act(F->ask_act[h->arg]); break;
    case HIT_ROW: {
        F->fd_sel = h->arg;
        const fd_entry_t *fe = &F->fd[h->arg];
        if (fe->dir) {  // folders open at a click
            F->fd_name[0] = 0;
            fd_go(F->fd_pool + fe->name);
        } else {
            snprintf(F->fd_name, sizeof F->fd_name, "%s", F->fd_pool + fe->name);
            if (e->double_click)
                fd_ok();
        }
        F->dirty = true;
        break;
    }
    case HIT_FD_OK: fd_ok(); break;
    case HIT_FD_CANCEL: fd_close(); F->then = ACT_NONE; break;
    case HIT_FD_UP: fd_go(".."); break;
    case HIT_IN_OK: key('\r'); break;
    case HIT_IN_CANCEL: F->dlg = DLG_NONE; F->dirty = true; break;
    case HIT_IN_MODE: F->in_text = h->arg; F->dirty = true; break;
    case HIT_IN_CASE: F->match_case = !F->match_case; F->dirty = true; break;
    case HIT_SWALLOW:
        if (F->dlg == DLG_HELP || F->dlg == DLG_ABOUT || F->dlg == DLG_SUMS)
            F->dlg = DLG_NONE, F->dirty = true;
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
    if (id == HIT_MENU && h->arg != F->menu && e->type != MOUSE_UP) {
        menu_open(h->arg, F->marmed || e->type == MOUSE_DOWN);
        return;
    }
    if (id == HIT_ITEM) {
        if (F->msel != h->arg)
            F->msel = h->arg, F->dirty = true;
        if (e->type == MOUSE_DOWN || (e->type == MOUSE_UP && F->marmed)) {
            item_run(h->arg);
            return;
        }
    } else if (F->msel >= 0 && e->type == MOUSE_MOVE) {
        F->msel = -1;
        F->dirty = true;
    }
    if (e->type == MOUSE_DOWN && id != HIT_ITEM && id != HIT_MENU)
        menu_close();
    else if (e->type == MOUSE_DOWN && id == HIT_MENU)
        menu_close();  // its own title again
    if (e->type == MOUSE_UP)
        F->marmed = false;
}

/// While selecting with the mouse past the top or bottom row: scrolls, and the selection follows.
static void drag_scroll(void) {
    if (F->mdrag != 1 || (F->my >= ROW_Y && F->my < ROW_Y + ROWS * 16))
        return;
    uint32_t now = now_ms();
    if ((int32_t)(now - F->next_scroll) < 0)
        return;
    F->next_scroll = now + 40;
    scroll(F->my < ROW_Y ? -1 : 1);
    drag_select(F->mx, F->my);
}

// ---- The editor ----------------------------------------------------------------------------------

/// Starts the state (SRAM): an empty document.
/// @return false if there is no memory for it
static bool start(void) {
    F = salloc(sizeof *F);
    if (!F)
        return false;
    memset(F, 0, sizeof *F);
    F->menu = -1;
    F->t = dvi_framebuf;
    F->refresh_at = now_ms() + REFRESH_MS;
    return true;
}

/// Frees the state.
static void stop(void) {
    close_source();
    undo_forget();
    gfree(F->clip);
    gfree(F->pc);
    gfree(F->add);
    if (F->fd_psram)
        psram_free(F->fd);
    else
        free(F->fd);
    psram_free(F->g);
    free(F);
    F = NULL;
}

/// The editor.
static void editor(void) {
    F->g = psram_malloc(W * H);  // drawn into and copied out at once, so PSRAM's fading doesn't matter
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_ARROW);
    draw();
    show();
    bool msg_shown = F->msg[0] != 0;
    while (!F->quit) {
        int k;
        while (!F->quit && (k = key_poll()) != KEY_NONE)
            key(k);
        mouse_event_t e;
        while (!F->quit && mouse_event(&e))
            mouse(&e);
        drag_scroll();
        refresh_step();
        mouse_pointer_update();
        if (F->read_err) {
            F->read_err = false;
            message("Reading the file failed: those bytes show as 00");
        }
        bool msg_on = F->msg[0] && now_ms() < F->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            F->view_dirty = true;
        }
        if (F->dirty && !F->quit) {
            F->dirty = false;
            draw();
            show();
        } else if (F->view_dirty && !F->quit && !F->dlg && F->menu < 0) {
            F->view_dirty = false;
            F->t = dvi_framebuf;
            draw_view();
        }
        sleep_ms(F->mdrag ? 1 : 5);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
}

// ---- The command ----------------------------------------------------------------------------------

void hex_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        printf("usage: hex [file]   the hex editor (hexedit too); F1 in it lists its keys\n");
        return;
    }
    if (!start()) {
        printf("hex: not enough memory\n");
        return;
    }
    if (argc > 1) {
        FILINFO fi;
        if (f_stat(argv[1], &fi) == FR_OK) {
            if (!load(argv[1])) {
                printf("hex: %s\n", F->msg);
                stop();
                return;
            }
        } else {
            whole_path(F->path, sizeof F->path, argv[1]);
            message("%s is new", file_name());
        }
    }
    editor();
    stop();
}
