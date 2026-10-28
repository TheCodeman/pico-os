/**
 * @file wav_ui.c
 * @brief The `wav` command: a graphical WAV editor in the sprite editor's and tracker's style (bevelled panels drawn off the screen, the scanout mouse arrow), playing through HDMI audio.
 *
 * The screen: an overview of the whole sound with the part shown marked; a
 * time ruler; the waveform (one lane, or two for stereo) with the selection
 * and the cursor; two lines about the cursor, selection and pointer; the play,
 * view, edit and effect buttons; and the file buttons along the bottom, as in
 * the sprite editor.
 *
 * Files: WAV, PCM 8, 16, 24 or 32 bits or 32-bit float, mono or stereo (of
 * more channels the first two are kept), at any rate; saved as 8 or 16-bit PCM.
 * The sound is kept as 16-bit samples at its own rate and played at 48 kHz
 * with linear interpolation.
 *
 * Memory: the editor's state, the sound, the clipboard, the undo steps and
 * the off-screen picture are in PSRAM while it runs; nothing stays in SRAM but
 * the checksums below, freed on leaving. This board's PSRAM has had cells
 * that forget after a minute or more untouched, so (as in the text editor)
 * every 8 KB block of the sound, clipboard and undo steps has a checksum, and
 * is read from the chip, checked and written back every 15 seconds. A block
 * that changed by itself is reported, and Save then won't write over the file.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "hardware/xip_cache.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "wav.h"

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height
#define MAX_UNDO 32   ///< steps kept for undo (and as many for redo)
#define MAX_FILES 128 ///< entries the file box lists
#define MAX_REGIONS (2 * MAX_UNDO + 8)  ///< blocks of memory the checksums look after
#define GBLOCK 8192   ///< the bytes each checksum covers
#define REFRESH_MS 15000  ///< every block is checked and rewritten this often
#define SUMN 512      ///< frames each entry of the overview's summary covers
#define PLAY_RATE 48000  ///< HDMI audio's rate
#ifndef PSRAM_UNCACHED_OFFSET
/// From the PSRAM's cached address to its uncached one.
#define PSRAM_UNCACHED_OFFSET (0x15000000u - 0x11000000u)
#endif

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles, the cursor
#define C_CYAN   0x1f  ///< typed text
#define C_ORANGE 0xf0  ///< the play position
#define C_RED    0xe0  ///< memory errors
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the selected row
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip
#define C_WAVE   0x1c  ///< the waveform
#define C_WAVE_S 0xdf  ///< the waveform in the selection
#define C_SELBG  0x0a  ///< the selection behind it
#define C_MID    0x08  ///< the zero line
#define C_LANE   0x49  ///< between the left and right lanes
#define C_SHOWN   0xfc  ///< the part shown, on the overview

/// Layout.
#define TOP_H   24     ///< the title strip
#define OV_X    6      ///< the overview's inside
#define OV_Y    29     ///< its top
#define OV_W    628    ///< its width
#define OV_H    26     ///< its height
#define RUL_Y   60     ///< the time ruler
#define WV_X    6      ///< the waveform's left
#define WV_Y    78     ///< its top
#define WV_W    628    ///< its width (a column a pixel)
#define WV_H    222    ///< its height
#define INFO_Y  305    ///< the two lines about the cursor and selection
#define TOOL_Y  344    ///< the buttons' panel
#define FILE_Y  444    ///< the file buttons

/// What a click hits.
enum {
    HIT_WAVE = 1, HIT_OVER, HIT_TOOL, HIT_FILE,
    HIT_ASK, HIT_ROW, HIT_FD_OK, HIT_FD_CANCEL, HIT_FD_UP,
    HIT_GAIN, HIT_GAIN_OK, HIT_GAIN_NORM, HIT_BOX_CANCEL,
    HIT_RATE, HIT_CHANS, HIT_BITS, HIT_RESAMPLE, HIT_FMT_OK, HIT_SWALLOW,
};

/// The buttons above the file buttons (HIT_TOOL).
enum {
    TB_PLAY, TB_LOOP, TB_ZIN, TB_ZOUT, TB_ZSEL, TB_ZALL, TB_ALL,
    TB_CUT, TB_COPY, TB_PASTE, TB_DELETE, TB_TRIM, TB_SILENCE, TB_REVERSE,
    TB_GAIN, TB_NORMAL, TB_FADEIN, TB_FADEOUT, TB_COUNT
};
/// The file buttons (HIT_FILE).
enum { FB_NEW, FB_LOAD, FB_SAVE, FB_SAVEAS, FB_FORMAT, FB_UNDO, FB_REDO, FB_HELP, FB_QUIT, FB_COUNT };

/// Things to do, after a question or a save.
enum { ACT_NONE, ACT_NEW, ACT_LOAD, ACT_QUIT, ACT_SAVE_THEN, ACT_WRITE };

/// The boxes over the editor.
enum { DLG_NONE, DLG_ASK, DLG_FILE, DLG_GAIN, DLG_FORMAT, DLG_HELP };
/// What the file box is for.
enum { FD_LOAD, FD_SAVE };

/// What a drag with the mouse does.
enum { DRAG_NONE, DRAG_SEL, DRAG_OVER };

/// The rates the format box offers.
static const uint32_t rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
#define NRATES (int)(sizeof rates / sizeof *rates)  ///< how many

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A block of PSRAM the checksums look after.
typedef struct {
    uint8_t *p;      ///< it
    uint32_t len;    ///< its bytes (a multiple of 4)
    uint32_t *sums;  ///< a checksum for each GBLOCK (SRAM)
    uint8_t *dirty;  ///< changed by the editor since its sum was taken (after sums)
} region_t;

/// A step to undo (or redo): what goes back where.
typedef struct {
    bool fmt;          ///< a format change: the format (and d, if not NULL: the whole sound) go back
    uint32_t pos;      ///< where it goes back (frames)
    uint32_t len;      ///< the frames there now that it replaces
    int16_t *d;        ///< what goes back, or NULL for nothing
    uint32_t n;        ///< its frames
    uint32_t cap;      ///< fmt: d's room in frames
    int ch;            ///< fmt: the channels going back
    int bits;          ///< fmt: the bits going back
    uint32_t rate;     ///< fmt: the rate going back
    /// the selection to go back to
    ///@{
    uint32_t sel0, sel1;
    ///@}
    char what[12];     ///< what it was, for the message
} undo_t;

/// An entry in the file box.
typedef struct {
    char name[40];  ///< its name
    bool dir;       ///< a folder
} fd_entry_t;

/// Everything the editor keeps (PSRAM, while it runs).
typedef struct {
    // The sound.
    int16_t *d;            ///< its frames, the channels side by side
    uint32_t len;          ///< its frames
    uint32_t cap;          ///< room in d (frames)
    int ch;                ///< 1 or 2 channels
    int bits;              ///< 8 or 16: saved like this
    uint32_t rate;         ///< frames a second
    int16_t *sm;           ///< the summary: lowest and highest of each channel for every SUMN frames
    uint32_t sm_cap;       ///< its room (entries)
    char path[160];        ///< the file, or ""
    bool modified;         ///< changed since saved
    char note[48];         ///< about the file as loaded (what was changed to read it)
    // The selection and the clipboard.
    /// the selection, sel0 <= sel1 (the cursor is sel0 when they are equal)
    ///@{
    uint32_t sel0, sel1;
    ///@}
    uint32_t anchor;       ///< where a drag began
    int16_t *clip;         ///< the clipboard's frames
    uint32_t clip_len;     ///< how many
    int clip_ch;           ///< its channels
    // Undo.
    undo_t undo[MAX_UNDO]; ///< steps to undo, oldest first
    int nundo;             ///< their count
    undo_t redo[MAX_UNDO]; ///< and to redo
    int nredo;             ///< their count
    bool undo_lost;        ///< the last change was too big to keep for undo
    // Keeping the PSRAM fresh.
    region_t reg[MAX_REGIONS];  ///< the blocks looked after
    int nreg;              ///< how many
    int rnext;             ///< the next region to check
    uint32_t rblock;       ///< and its next block
    uint32_t refresh_at;   ///< when the next pass starts
    bool damaged;          ///< a block changed by itself
    // Playing (core 1 reads these).
    volatile bool pl_on;       ///< playing
    volatile uint32_t pl_pos;  ///< the frame it is at
    volatile uint32_t pl_frac; ///< and the fraction past it (16 bits)
    volatile uint32_t pl_step; ///< frames per 48 kHz frame (16.16)
    volatile uint32_t pl_end;  ///< where it stops or loops
    volatile uint32_t pl_loop0;  ///< where a loop goes back to
    volatile bool pl_loop;     ///< loops
    bool loop;             ///< the Loop button
    int ph_x;              ///< the column the play position is drawn in (-1: none)
    uint32_t next_ph;      ///< when it is next moved
    // The view.
    uint32_t view0;        ///< the first frame shown
    uint32_t vlen;         ///< frames across the waveform
    int16_t cmin[2][WV_W]; ///< each column's lowest value, by channel
    int16_t cmax[2][WV_W]; ///< and highest
    bool chas[WV_W];       ///< the column has sound
    bool cols_dirty;       ///< work the columns out again
    int drag;              ///< DRAG_*
    /// the pointer
    ///@{
    int mx, my;
    ///@}
    uint32_t next_scroll;  ///< when dragging past the edge scrolls again
    int hover_x;           ///< the column under the pointer (-1: none)
    // The boxes.
    int dlg;               ///< DLG_*
    char ask[96];          ///< the question
    const char *ask_btn[3];  ///< its buttons (NULL: fewer)
    int ask_act[3];        ///< what each does
    int then;              ///< what to do after a save (ACT_*)
    int fd_mode;           ///< FD_*
    char fd_dir[160];      ///< the folder shown
    char fd_name[40];      ///< the name typed
    fd_entry_t *fd;        ///< its entries (MAX_FILES)
    int fd_n;              ///< how many
    int fd_sel;            ///< the one chosen
    int fd_top;            ///< the first shown
    char target[200];      ///< the file to write (ACT_WRITE)
    int gain_db10;         ///< the gain box's gain, tenths of a dB
    /// the format box's choices
    ///@{
    int f_rate, f_ch, f_bits;
    bool f_resample;
    ///@}
    char msg[96];          ///< a message for the title strip
    uint32_t msg_until;    ///< until when
    uint8_t io[4096];      ///< file reading and writing
    uint8_t *g;            ///< the off-screen picture (PSRAM)
    uint8_t *t;            ///< where drawing goes: g, or the screen itself for small changes
    hit_t hits[120];       ///< places to click
    int nhits;             ///< their count
    bool collect;          ///< drawing the whole picture: places to click are kept
    bool dirty;            ///< draw everything again
    bool quit;             ///< leaving
} ed_t;

static ed_t *F;  ///< the state, while the editor or a command runs

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void draw_top(void);

// ---- Keeping the PSRAM fresh ------------------------------------------------------------------

/// The region a pointer is in.
/// @param p the pointer
/// @return it, or NULL
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
    r->sums = malloc(nb * 5);
    r->dirty = (uint8_t *)(r->sums + nb);
    memset(r->dirty, 1, nb);
}

static bool undo_drop_oldest(void);

/// PSRAM that the checksums look after; when there isn't room, the oldest
/// undo steps make way.
/// @param n bytes
/// @return it, or NULL
static void *galloc(size_t n) {
    if (F->nreg == MAX_REGIONS)
        return NULL;
    n = (n + 7) & ~(size_t)7;
    if (!n)
        n = 8;
    void *p;
    while (!(p = psram_malloc(n)))
        if (!undo_drop_oldest())
            return NULL;
    region_t *r = &F->reg[F->nreg++];
    r->p = p;
    r->len = (uint32_t)n;
    r->sums = NULL;
    region_sums(r);
    return p;
}

/// Frees memory from galloc.
/// @param p it (NULL is fine)
static void gfree(void *p) {
    region_t *r = p ? region_of(p) : NULL;
    if (!r)
        return;
    free(r->sums);
    psram_free(r->p);
    *r = F->reg[--F->nreg];
    if (F->rnext >= F->nreg)
        F->rnext = 0, F->rblock = 0;
}

/// Makes memory from galloc bigger or smaller (the undo steps make way if need be).
/// @param p it
/// @param n the bytes wanted
/// @return it (maybe moved), or NULL (p kept)
static void *grealloc(void *p, size_t n) {
    region_t *r = region_of(p);
    n = (n + 7) & ~(size_t)7;
    void *np;
    while (!(np = psram_realloc(p, n)))
        if (!undo_drop_oldest())
            return NULL;
    r = region_of(p);  // the drops may have moved it in the table
    r->p = np;
    r->len = (uint32_t)n;
    region_sums(r);
    return np;
}

/// Marks bytes the editor changed, so their checksums are taken again.
/// @param p the first
/// @param n how many
static void gtouch(const void *p, size_t n) {
    region_t *r = n ? region_of(p) : NULL;
    if (!r)
        return;
    uint32_t a = (uint32_t)((const uint8_t *)p - r->p), b = a + (uint32_t)n - 1;
    if (b >= r->len)
        b = r->len - 1;
    for (uint32_t k = a / GBLOCK; k <= b / GBLOCK; ++k)
        r->dirty[k] = 1;
}

/// One block: from the chip, summed and written back. False if a block the
/// editor hasn't changed no longer matches its sum.
/// @param r the region
/// @param k the block
/// @return it was all right
static bool refresh_block(region_t *r, uint32_t k) {
    uint32_t a = k * GBLOCK, n = r->len - a < GBLOCK ? r->len - a : GBLOCK;
    volatile uint32_t *w = (volatile uint32_t *)(r->p + a + PSRAM_UNCACHED_OFFSET);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n / 4; ++i) {
        uint32_t v = w[i];
        w[i] = v;
        sum = (sum << 5 | sum >> 27) ^ v;
    }
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
            message("MEMORY ERROR: the sound changed by itself; Save as a new file");
        }
        if (++F->rblock * GBLOCK >= r->len)
            F->rblock = 0, ++F->rnext;
    }
    return true;
}

/// While the editor waits: two blocks at a time, a pass every REFRESH_MS.
static void refresh_step(void) {
    if ((int32_t)(now_ms() - F->refresh_at) < 0)
        return;
    if (!refresh(2))
        F->refresh_at = now_ms() + REFRESH_MS;
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
    for (int r = 0; r < h; ++r)
        memset(F->t + (y + r) * W + x, c, w);
}

/// A rectangle's outline.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void frame_box(int x, int y, int w, int h, uint8_t c) {
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
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

/// A button that can't be used now: its text dimmed.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param label its text
static void dim_label(int x, int y, int w, int h, const char *label) {
    text(x + (w - (int)strlen(label) * 8) / 2, y + (h - 16) / 2, label, C_DIM, C_FACE);
}

// ---- Times ------------------------------------------------------------------------------------

/// A number of frames as m:ss.mmm.
/// @param s where it goes
/// @param n its room
/// @param frames the frames
static void fmt_time(char *s, int n, uint32_t frames) {
    uint64_t ms = (uint64_t)frames * 1000 / F->rate;
    snprintf(s, n, "%u:%02u.%03u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000));
}

/// A level as dB below full scale.
/// @param s where it goes
/// @param n its room
/// @param peak the level (0-32768)
static void fmt_db(char *s, int n, int peak) {
    if (peak <= 0) {
        snprintf(s, n, "silent");
        return;
    }
    int t = (int)lrintf(200.0f * log10f(peak / 32768.0f));  // tenths
    snprintf(s, n, "%s%d.%d dB", t < 0 ? "-" : "", abs(t) / 10, abs(t) % 10);
}

// ---- The summary and the columns ------------------------------------------------------------------

/// Works the summary out again from a frame on (after a change there).
/// @param from the frame
static void summ_update(uint32_t from) {
    uint32_t nb = (F->len + SUMN - 1) / SUMN;
    if (nb + 1 > F->sm_cap) {
        uint32_t cap = nb + 256;
        int16_t *sm = F->sm ? grealloc(F->sm, cap * 4 * sizeof *sm) : galloc(cap * 4 * sizeof *sm);
        if (!sm)
            return;  // the overview is out of date; it isn't the sound
        F->sm = sm;
        F->sm_cap = cap;
        from = 0;
    }
    for (uint32_t k = from / SUMN; k < nb; ++k) {
        int lo[2] = {32767, 32767}, hi[2] = {-32768, -32768};
        uint32_t a = k * SUMN, b = a + SUMN < F->len ? a + SUMN : F->len;
        for (uint32_t i = a; i < b; ++i)
            for (int c = 0; c < F->ch; ++c) {
                int v = F->d[i * F->ch + c];
                lo[c] = v < lo[c] ? v : lo[c];
                hi[c] = v > hi[c] ? v : hi[c];
            }
        int16_t *e = F->sm + k * 4;
        e[0] = (int16_t)lo[0], e[1] = (int16_t)hi[0];
        e[2] = (int16_t)(F->ch > 1 ? lo[1] : lo[0]), e[3] = (int16_t)(F->ch > 1 ? hi[1] : hi[0]);
    }
    if (nb > from / SUMN)
        gtouch(F->sm + from / SUMN * 4, (nb - from / SUMN) * 4 * sizeof *F->sm);
}

/// A channel's value at a place between frames (16.16).
/// @param t the place
/// @param c the channel
/// @return the value
static int value_at(uint64_t t, int c) {
    uint32_t i = (uint32_t)(t >> 16), f = (uint32_t)t & 0xffff;
    if (i >= F->len)
        return 0;
    int a = F->d[i * F->ch + c], b = i + 1 < F->len ? F->d[(i + 1) * F->ch + c] : a;
    return a + (int)(((int64_t)(b - a) * f) >> 16);
}

/// The lowest and highest values of a channel over frames a to b (inclusive),
/// from the summary where it covers whole entries.
/// @param a the first frame
/// @param b the last
/// @param c the channel
/// @param lo lowered to the lowest
/// @param hi raised to the highest
static void range_minmax(uint32_t a, uint32_t b, int c, int *lo, int *hi) {
    if (!F->len)
        return;
    if (b >= F->len)
        b = F->len - 1;
    bool rough = b - a >= 4 * SUMN;  // a wide span: the summary's entries at its ends will do
    while (a <= b) {
        if ((rough || (a % SUMN == 0 && a + SUMN - 1 <= b)) && F->sm && a / SUMN < F->sm_cap) {
            const int16_t *e = F->sm + a / SUMN * 4 + c * 2;
            *lo = e[0] < *lo ? e[0] : *lo;
            *hi = e[1] > *hi ? e[1] : *hi;
            a = (a / SUMN + 1) * SUMN;
            continue;
        }
        int v = F->d[a * F->ch + c];
        *lo = v < *lo ? v : *lo;
        *hi = v > *hi ? v : *hi;
        ++a;
    }
}

/// The place (16.16 frames) at a column's left edge.
/// @param x the column (0 to WV_W)
/// @return the place
static uint64_t col_pos(int x) {
    return ((uint64_t)F->view0 << 16) + ((uint64_t)x * F->vlen << 16) / WV_W;
}

/// Works out each column's lowest and highest value.
static void wave_columns(void) {
    F->cols_dirty = false;
    for (int x = 0; x < WV_W; ++x) {
        uint64_t t0 = col_pos(x), t1 = col_pos(x + 1);
        F->chas[x] = F->len && (t0 >> 16) < F->len;
        if (!F->chas[x])
            continue;
        for (int c = 0; c < F->ch; ++c) {
            int lo = 32767, hi = -32768;
            uint32_t a = (uint32_t)(t0 >> 16), b = (uint32_t)(t1 >> 16);
            if (b > a + 1) {
                range_minmax(a, b, c, &lo, &hi);
            } else {  // zoomed in: the line between the frames
                int v0 = value_at(t0, c), v1 = value_at(t1, c);
                lo = v0 < v1 ? v0 : v1;
                hi = v0 > v1 ? v0 : v1;
                if (b > a && b < F->len) {
                    int v = F->d[b * F->ch + c];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
            }
            F->cmin[c][x] = (int16_t)lo;
            F->cmax[c][x] = (int16_t)hi;
        }
    }
}

/// The column a frame is in.
/// @param p the frame
/// @return the column, or -1 if it isn't shown
static int frame_x(uint32_t p) {
    if (p < F->view0 || p > F->view0 + F->vlen)
        return -1;
    int x = (int)((uint64_t)(p - F->view0) * WV_W / F->vlen);
    return x < WV_W ? x : -1;
}

/// The frame at a screen x (the nearest frame boundary).
/// @param sx the screen's x
/// @return the frame (0 to the length)
static uint32_t x_frame(int sx) {
    int64_t x = sx - WV_X;
    int64_t p = (int64_t)F->view0 + (x * (int64_t)F->vlen * 2 + WV_W) / (2 * WV_W);
    if (p < 0)
        p = 0;
    if (p > F->len)
        p = F->len;
    return (uint32_t)p;
}

/// Keeps the view within the sound (and at least a little of it).
static void view_fix(void) {
    uint32_t min = WV_W / 16, all = F->len > min ? F->len : (F->len ? min : F->rate);
    if (F->vlen < min)
        F->vlen = min;
    if (F->vlen > all)
        F->vlen = all;
    if (F->view0 + F->vlen > F->len)
        F->view0 = F->len > F->vlen ? F->len - F->vlen : 0;
    F->cols_dirty = true;
}

// ---- The waveform ----------------------------------------------------------------------------------

/// Draws one column of the waveform, with the selection, cursor and play position.
/// @param x the column
static void draw_col(int x) {
    uint32_t p0 = (uint32_t)(col_pos(x) >> 16), p1 = (uint32_t)((col_pos(x + 1) + 0xffff) >> 16);
    bool sel = F->sel1 > F->sel0 && p1 > F->sel0 && p0 < F->sel1;
    uint8_t bg = sel ? C_SELBG : C_FIELD, wc = sel ? C_WAVE_S : C_WAVE;
    int lanes = F->ch, lh = WV_H / lanes;
    uint8_t *col = F->t + WV_Y * W + WV_X + x;
    for (int y = 0; y < WV_H; ++y)
        col[y * W] = bg;
    for (int l = 0; l < lanes; ++l) {
        int top = l * lh, mid = top + lh / 2, amp = lh / 2 - 2;
        col[mid * W] = C_MID;
        if (l)
            col[top * W] = C_LANE;
        if (!F->chas[x])
            continue;
        int y0 = mid - F->cmax[l][x] * amp / 32768, y1 = mid - F->cmin[l][x] * amp / 32768;
        for (int y = y0; y <= y1; ++y)
            col[y * W] = wc;
    }
    uint8_t line = 0;
    if (F->sel0 == F->sel1 && (frame_x(F->sel0) == x || (x == WV_W - 1 && F->sel0 == F->view0 + F->vlen)))
        line = C_YELLOW;
    if (x == F->ph_x)
        line = C_ORANGE;
    if (line)
        for (int y = 0; y < WV_H; ++y)
            col[y * W] = line;
}

/// Draws the whole waveform.
static void draw_wave(void) {
    if (F->cols_dirty)
        wave_columns();
    for (int x = 0; x < WV_W; ++x)
        draw_col(x);
}

/// Draws the time ruler.
static void draw_ruler(void) {
    fill(WV_X, RUL_Y, WV_W, 17, C_FACE);
    fill(WV_X, RUL_Y + 17, WV_W, 1, C_DARK);
    // A tick every so many ms: the least that leaves room for its label.
    static const uint32_t steps[] = {1,    2,    5,     10,    20,    50,     100,    200,    500,   1000,
                                     2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000};
    uint64_t span_ms = (uint64_t)F->vlen * 1000 / F->rate;
    uint32_t step = steps[sizeof steps / sizeof *steps - 1];
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; ++i)
        if ((uint64_t)steps[i] * WV_W >= span_ms * 80) {
            step = steps[i];
            break;
        }
    uint64_t t0 = (uint64_t)F->view0 * 1000 / F->rate;
    for (uint64_t t = (t0 + step - 1) / step * step;; t += step) {
        uint32_t p = (uint32_t)((t * F->rate + 999) / 1000);
        int x = frame_x(p);
        if (p > F->view0 + F->vlen || x < 0)
            break;
        fill(WV_X + x, RUL_Y + 11, 1, 6, C_LABEL);
        char s[24];
        unsigned sec = (unsigned)(t / 1000), ms = (unsigned)(t % 1000);
        if (step >= 1000)
            snprintf(s, sizeof s, "%u:%02u", sec / 60, sec % 60);
        else if (step >= 100)
            snprintf(s, sizeof s, "%u.%u", sec, ms / 100);
        else if (step >= 10)
            snprintf(s, sizeof s, "%u.%02u", sec, ms / 10);
        else
            snprintf(s, sizeof s, "%u.%03u", sec, ms);
        if (WV_X + x + 2 + (int)strlen(s) * 8 <= WV_X + WV_W)
            text(WV_X + x + 2, RUL_Y - 2, s, C_LABEL, C_FACE);
    }
}

/// Draws the overview: the whole sound, the selection, and the part shown.
static void draw_over(void) {
    bevel(OV_X - 2, OV_Y - 2, OV_W + 4, OV_H + 4, false, C_FIELD);
    hit(OV_X, OV_Y, OV_W, OV_H, HIT_OVER, 0);
    if (!F->len)
        return;
    int mid = OV_Y + OV_H / 2, amp = OV_H / 2 - 1;
    for (int x = 0; x < OV_W; ++x) {
        uint32_t a = (uint32_t)((uint64_t)x * F->len / OV_W), b = (uint32_t)((uint64_t)(x + 1) * F->len / OV_W);
        if (b > a)
            --b;
        if (F->sel1 > F->sel0 && b >= F->sel0 && a < F->sel1)
            fill(OV_X + x, OV_Y, 1, OV_H, C_SELBG);
        int lo = 32767, hi = -32768;
        for (int c = 0; c < F->ch; ++c)
            range_minmax(a, b, c, &lo, &hi);
        fill(OV_X + x, mid - hi * amp / 32768, 1, (hi - lo) * amp / 32768 + 1, C_WAVE);
    }
    int x0 = (int)((uint64_t)F->view0 * OV_W / F->len);
    int x1 = (int)((uint64_t)(F->view0 + F->vlen) * OV_W / F->len);
    if (x1 > OV_W)
        x1 = OV_W;
    frame_box(OV_X + x0, OV_Y, x1 - x0 > 2 ? x1 - x0 : 2, OV_H, C_SHOWN);
}

/// The file's name without its folder.
/// @return it, or "untitled"
static const char *file_name(void) {
    if (!F->path[0])
        return "untitled";
    const char *s = strrchr(F->path, '/');
    return s ? s + 1 : F->path;
}

/// Draws the two lines about the cursor, the selection, the pointer and memory.
static void draw_info(void) {
    fill(8, INFO_Y, W - 16, 34, C_FACE);
    char a[20], b[20], c[20];
    int x;
    if (F->sel1 > F->sel0) {
        fmt_time(a, sizeof a, F->sel0);
        fmt_time(b, sizeof b, F->sel1);
        fmt_time(c, sizeof c, F->sel1 - F->sel0);
        x = text(12, INFO_Y, "Selected ", C_LABEL, C_FACE);
        textf(x, INFO_Y, C_TEXT, C_FACE, "%s to %s  (%s, %lu frames)", a, b, c, (unsigned long)(F->sel1 - F->sel0));
    } else {
        fmt_time(a, sizeof a, F->sel0);
        x = text(12, INFO_Y, "Cursor ", C_LABEL, C_FACE);
        textf(x, INFO_Y, C_TEXT, C_FACE, "%s  (frame %lu)", a, (unsigned long)F->sel0);
    }
    if (F->clip_len) {
        fmt_time(a, sizeof a, F->clip_len);
        textf(W - 12 - (int)(6 + strlen(a)) * 8, INFO_Y, C_DIM, C_FACE, "Clip %s", a);
    }
    int y = INFO_Y + 18;
    if (F->hover_x >= 0) {
        uint32_t p = x_frame(WV_X + F->hover_x);
        int peak = 0;
        if (F->chas[F->hover_x])
            for (int ch = 0; ch < F->ch; ++ch) {
                int m = abs(F->cmin[ch][F->hover_x]) > abs(F->cmax[ch][F->hover_x]) ? abs(F->cmin[ch][F->hover_x])
                                                                                     : abs(F->cmax[ch][F->hover_x]);
                peak = m > peak ? m : peak;
            }
        fmt_time(a, sizeof a, p);
        fmt_db(b, sizeof b, peak);
        x = text(12, y, "Pointer ", C_LABEL, C_FACE);
        x = textf(x, y, C_TEXT, C_FACE, "%s  %s", a, b);
    } else {
        fmt_time(a, sizeof a, F->view0);
        fmt_time(b, sizeof b, F->view0 + F->vlen < F->len ? F->view0 + F->vlen : F->len);
        x = text(12, y, "Shown ", C_LABEL, C_FACE);
        textf(x, y, C_TEXT, C_FACE, "%s to %s", a, b);
    }
    size_t fr = psram_free_total();
    textf(W - 12 - 21 * 8, y, C_DIM, C_FACE, "%5lu.%lu MB memory free", (unsigned long)(fr >> 20),
          (unsigned long)((fr & 0xfffff) * 10 >> 20));
}

/// The buttons' labels and widths, in rows.
static const struct {
    const char *label;  ///< its text
    int w;              ///< its width
    int row;            ///< 0-2
} tools[TB_COUNT] = {
    [TB_PLAY] = {"Play", 64, 0},     [TB_LOOP] = {"Loop", 56, 0},     [TB_ZIN] = {"Zoom +", 64, 0},
    [TB_ZOUT] = {"Zoom -", 64, 0},   [TB_ZSEL] = {"Zoom sel", 80, 0}, [TB_ZALL] = {"Zoom all", 80, 0},
    [TB_ALL] = {"Select all", 96, 0}, [TB_CUT] = {"Cut", 56, 1},       [TB_COPY] = {"Copy", 56, 1},
    [TB_PASTE] = {"Paste", 56, 1},   [TB_DELETE] = {"Delete", 64, 1}, [TB_TRIM] = {"Trim", 56, 1},
    [TB_SILENCE] = {"Silence", 72, 1}, [TB_REVERSE] = {"Reverse", 72, 1}, [TB_GAIN] = {"Gain", 56, 2},
    [TB_NORMAL] = {"Normalize", 88, 2}, [TB_FADEIN] = {"Fade in", 72, 2}, [TB_FADEOUT] = {"Fade out", 80, 2},
};

/// Draws the play, view, edit and effect buttons.
static void draw_tools(void) {
    bevel(4, TOOL_Y, W - 8, 96, true, C_FACE);
    static const char *const row_label[3] = {"Play", "Edit", "Change"};
    int x[3] = {72, 72, 72};
    for (int r = 0; r < 3; ++r)
        text(12, TOOL_Y + 10 + r * 30, row_label[r], C_LABEL, C_FACE);
    bool sel = F->sel1 > F->sel0;
    for (int i = 0; i < TB_COUNT; ++i) {
        int r = tools[i].row, y = TOOL_Y + 6 + r * 30;
        const char *label = i == TB_PLAY && F->pl_on ? "Stop" : tools[i].label;
        bool lit = (i == TB_PLAY && F->pl_on) || (i == TB_LOOP && F->loop);
        button(x[r], y, tools[i].w, 26, label, lit, HIT_TOOL, i);
        bool off = ((i == TB_CUT || i == TB_COPY || i == TB_DELETE || i == TB_TRIM || i == TB_ZSEL) && !sel) ||
                   (i == TB_PASTE && !F->clip_len) || (!F->len && i != TB_PASTE && i != TB_SILENCE && i != TB_LOOP);
        if (off && !lit)
            dim_label(x[r], y, tools[i].w, 26, label);
        x[r] += tools[i].w + 4;
    }
    text(x[2] + 12, TOOL_Y + 6 + 60 + 5, sel ? "on the selection" : "on all of it", C_DIM, C_FACE);
}

/// Draws the file buttons.
static void draw_files(void) {
    static const char *const fb[FB_COUNT] = {"New", "Load", "Save", "Save as", "Format", "Undo", "Redo", "Help",
                                             "Quit"};
    for (int i = 0; i < FB_COUNT; ++i) {
        bool off = (i == FB_UNDO && !F->nundo) || (i == FB_REDO && !F->nredo);
        button(4 + i * 63, FILE_Y, 60, 26, fb[i], false, HIT_FILE, i);
        if (off)
            dim_label(4 + i * 63, FILE_Y, 60, 26, fb[i]);
    }
}

/// Draws the title strip, with the message or the file, its format and length.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    text(8, 4, "WAV Editor", C_YELLOW, C_STRIP);
    if (F->msg[0] && now_ms() < F->msg_until) {
        textf(104, 4, F->damaged ? C_RED : C_TEXT, C_STRIP, "%.66s", F->msg);
    } else {
        char t[20];
        fmt_time(t, sizeof t, F->len);
        textf(104, 4, F->damaged ? C_RED : C_LABEL, C_STRIP, "%.24s%s  %lu Hz %d-bit %s  %s  F1 keys", file_name(),
              F->modified ? "*" : "", (unsigned long)F->rate, F->bits, F->ch == 2 ? "stereo" : "mono", t);
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
    int w = 460, h = 360, x = (W - w) / 2, y = 60;
    box(x, y, w, h, F->fd_mode == FD_LOAD ? "Load a WAV file" : "Save the sound as");
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
        textf(x + 20, ry + 1, e->dir ? C_CYAN : C_TEXT, bg, "%s%.48s", e->dir ? "\x10 " : "  ", e->name);
        hit(x + 14, ry, w - 28, 18, HIT_ROW, i);
    }
    int ny = ly + FD_ROWS * 18 + 12;
    text(x + 16, ny + 3, "Name", C_LABEL, C_FACE);
    bevel(x + 60, ny, w - 76, 22, false, C_FIELD);
    textf(x + 64, ny + 3, C_CYAN, C_FIELD, "%s_", F->fd_name);
    button(x + 16, y + h - 36, 120, 26, F->fd_mode == FD_LOAD ? "Load (Enter)" : "Save (Enter)", true, HIT_FD_OK,
           0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_FD_CANCEL, 0);
}

/// The gain box's steps, in tenths of a dB.
static const int gain_steps[6] = {-60, -10, -1, 1, 10, 60};

/// Draws the gain box.
static void draw_gain_box(void) {
    int w = 460, h = 150, x = (W - w) / 2, y = 150;
    box(x, y, w, h, F->sel1 > F->sel0 ? "Gain for the selection" : "Gain for all of the sound");
    int g = F->gain_db10;
    text(x + 16, y + 43, "Gain", C_LABEL, C_FACE);
    bevel(x + 60, y + 40, 96, 22, false, C_FIELD);
    textf(x + 68, y + 43, C_CYAN, C_FIELD, "%c%d.%d dB", g < 0 ? '-' : '+', abs(g) / 10, abs(g) % 10);
    static const char *const labels[6] = {"-6", "-1", "-.1", "+.1", "+1", "+6"};
    for (int i = 0; i < 6; ++i)
        button(x + 168 + i * 46, y + 40, 42, 22, labels[i], false, HIT_GAIN, i);
    textf(x + 16, y + 72, C_DIM, C_FACE, "x%d.%03d   (louder parts clip)", (int)(powf(10.0f, g / 200.0f)),
          (int)lrintf(powf(10.0f, g / 200.0f) * 1000) % 1000);
    button(x + 16, y + h - 36, 120, 26, "OK (Enter)", true, HIT_GAIN_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Normalize (N)", false, HIT_GAIN_NORM, 0);
    button(x + 272, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_BOX_CANCEL, 0);
}

/// Draws the format box.
static void draw_format_box(void) {
    int w = 560, h = 196, x = (W - w) / 2, y = 130;
    box(x, y, w, h, "Format");
    text(x + 16, y + 43, "Rate", C_LABEL, C_FACE);
    for (int i = 0; i < NRATES; ++i) {
        char s[8];
        snprintf(s, sizeof s, "%lu", (unsigned long)rates[i]);
        button(x + 80 + i * 66, y + 40, 62, 22, s, (uint32_t)F->f_rate == rates[i], HIT_RATE, i);
    }
    text(x + 16, y + 73, "Sound", C_LABEL, C_FACE);
    button(x + 80, y + 70, 62, 22, "Mono", F->f_ch == 1, HIT_CHANS, 1);
    button(x + 146, y + 70, 62, 22, "Stereo", F->f_ch == 2, HIT_CHANS, 2);
    text(x + 230, y + 73, "Saved as", C_LABEL, C_FACE);
    button(x + 310, y + 70, 62, 22, "8-bit", F->f_bits == 8, HIT_BITS, 8);
    button(x + 376, y + 70, 62, 22, "16-bit", F->f_bits == 16, HIT_BITS, 16);
    text(x + 16, y + 103, "New rate", C_LABEL, C_FACE);
    button(x + 96, y + 100, 196, 22, F->f_resample ? "Resample (same speed)" : "Relabel (speed changes)", false,
           HIT_RESAMPLE, 0);
    if ((uint32_t)F->f_rate != F->rate)
        text(x + 304, y + 103, F->f_resample ? "the samples are worked out" : "the samples are kept", C_DIM, C_FACE);
    button(x + 16, y + h - 36, 120, 26, "OK (Enter)", true, HIT_FMT_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_BOX_CANCEL, 0);
}

/// Draws the keys.
static void draw_help(void) {
    int w = 608, h = 392, x = (W - w) / 2, y = 40;
    box(x, y, w, h, "Keys (any key or click closes this)");
    static const char *const lines[] = {
        "Left button: the cursor; drag to select. Right: move the nearer",
        "end of the selection. Middle: play from there. Wheel: zoom.",
        "Overview at the top: click or drag to move the view.",
        "",
        "Space  play / stop    L loop    Home End  the start / the end",
        "+ - or Up Down  zoom   Z  zoom to the selection   A  zoom all",
        "Left Right PgUp PgDn  move the view   Ctrl-A  select all",
        "Ctrl-X cut  Ctrl-C copy  Ctrl-V paste  Del delete  T trim",
        "S silence (none selected: put in a second)  R reverse",
        "G gain  N normalize  I fade in  O fade out (on the selection,",
        "                                            or all of it)",
        "U, Ctrl-Z  undo   Ctrl-Y  redo   F  format (rate, mono/stereo)",
        "Ctrl-S, F2 save   F3 load   Ctrl-N new   Esc  stop, then quit",
        "",
        "Plays through the HDMI sound at 48 kHz. Load reads WAV files of",
        "8 to 32 bits; Save writes 8 or 16-bit (Format chooses).",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 36 + (int)i * 21, lines[i], C_TEXT, C_FACE);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    F->t = F->g;
    F->collect = true;
    F->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_top();
    draw_over();
    bevel(4, RUL_Y - 2, W - 8, WV_Y + WV_H - RUL_Y + 4, false, C_FIELD);
    draw_ruler();
    draw_wave();
    hit(WV_X, RUL_Y, WV_W, WV_Y + WV_H - RUL_Y, HIT_WAVE, 0);
    bevel(4, INFO_Y - 3, W - 8, 38, true, C_FACE);
    draw_info();
    draw_tools();
    draw_files();
    switch (F->dlg) {
    case DLG_ASK: draw_ask(); break;
    case DLG_FILE: draw_file_box(); break;
    case DLG_GAIN: draw_gain_box(); break;
    case DLG_FORMAT: draw_format_box(); break;
    case DLG_HELP: draw_help(); break;
    }
    F->collect = false;
    F->t = dvi_framebuf;
}

/// Copies the picture onto the screen.
static void show(void) {
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
    F->dirty = true;
}

/// Shows a message now, before something slow.
/// @param s it
static void busy(const char *s) {
    message("%s", s);
    if (F->g && !F->collect) {
        F->t = dvi_framebuf;
        draw_top();
    }
}

// ---- Playing ----------------------------------------------------------------------------------------

/// The audio source while playing (core 1): the next 48 kHz frame.
/// @param left the left sample
/// @param right the right
/// @return it is playing
static bool play_src(int32_t *left, int32_t *right) {
    ed_t *e = F;
    *left = *right = 0;
    if (!e || !e->pl_on)
        return false;
    uint32_t p = e->pl_pos;
    if (p >= e->pl_end) {
        if (!e->pl_loop || e->pl_loop0 >= e->pl_end) {
            e->pl_on = false;
            return false;
        }
        p = e->pl_loop0;
        e->pl_frac = 0;
    }
    int ch = e->ch;
    const int16_t *a = e->d + p * ch, *b = p + 1 < e->pl_end ? a + ch : a;
    int64_t f = e->pl_frac;
    int32_t l = a[0] + (int32_t)(((b[0] - a[0]) * f) >> 16);
    int32_t r = ch > 1 ? a[1] + (int32_t)(((b[1] - a[1]) * f) >> 16) : l;
    *left = l;
    *right = r;
    uint32_t fr = e->pl_frac + e->pl_step;
    e->pl_pos = p + (fr >> 16);
    e->pl_frac = fr & 0xffff;
    return true;
}

/// Stops playing, and waits for core 1 to be out of the sound.
static void play_stop(void) {
    if (!F->pl_on && F->ph_x < 0)
        return;
    F->pl_on = false;
    sleep_ms(2);  // core 1 may be in the middle of a frame
    F->ph_x = -1;
    F->dirty = true;
}

/// Plays the selection, or from the cursor to the end (looping if Loop is on).
/// @param from where to start (-1: the selection or the cursor)
static void play_start(int64_t from) {
    play_stop();
    if (!F->len)
        return;
    uint32_t a, end, loop0;
    if (from < 0 && F->sel1 > F->sel0) {
        a = loop0 = F->sel0;
        end = F->sel1;
    } else {
        a = from < 0 ? F->sel0 : (uint32_t)from;
        if (a >= F->len)
            a = 0;
        end = F->len;
        loop0 = 0;
    }
    F->pl_pos = a;
    F->pl_frac = 0;
    F->pl_step = (uint32_t)(((uint64_t)F->rate << 16) / PLAY_RATE);
    F->pl_end = end;
    F->pl_loop0 = loop0;
    F->pl_loop = F->loop;
    if (audio_get_source() != play_src)
        audio_set_source(play_src);
    F->pl_on = true;
    F->dirty = true;
}

/// Moves the play position's line to where playing is (and the view, if it has gone off it).
static void playhead(void) {
    int x = -1;
    if (F->pl_on) {
        uint32_t p = F->pl_pos;
        if ((p < F->view0 || p >= F->view0 + F->vlen) && F->vlen < F->len && !F->drag) {
            F->view0 = p;  // the next page
            view_fix();
            F->ph_x = frame_x(p);
            F->dirty = true;
            return;
        }
        x = frame_x(p);
    }
    if (x == F->ph_x)
        return;
    int was = F->ph_x;
    F->ph_x = x;
    if (F->dirty || F->dlg)
        return;
    F->t = dvi_framebuf;  // straight onto the screen
    if (was >= 0)
        draw_col(was);
    if (x >= 0)
        draw_col(x);
    if (!F->pl_on)
        F->dirty = true;  // the Stop button goes back to Play
}

// ---- Undo ----------------------------------------------------------------------------------------

/// Frees an undo step's memory.
/// @param u the step
static void undo_free(undo_t *u) {
    gfree(u->d);
    u->d = NULL;
}

/// Forgets the oldest undo step (or, with none, the oldest redo step), to make room.
/// @return there was one
static bool undo_drop_oldest(void) {
    undo_t *s = F->nundo ? F->undo : F->redo;
    int *n = F->nundo ? &F->nundo : &F->nredo;
    if (!*n)
        return false;
    undo_free(&s[0]);
    memmove(&s[0], &s[1], (*n - 1) * sizeof *s);
    --*n;
    F->undo_lost = true;
    return true;
}

/// Forgets every step.
static void undo_forget(void) {
    while (F->nundo)
        undo_free(&F->undo[--F->nundo]);
    while (F->nredo)
        undo_free(&F->redo[--F->nredo]);
}

/// Puts a step on a list, dropping its oldest if it is full.
/// @param s the list
/// @param n its count
/// @param u the step
static void undo_put(undo_t *s, int *n, const undo_t *u) {
    if (*n == MAX_UNDO) {
        undo_free(&s[0]);
        memmove(&s[0], &s[1], (MAX_UNDO - 1) * sizeof *s);
        --*n;
    }
    s[(*n)++] = *u;
}

/// A copy of some frames of the sound (galloc).
/// @param pos the first
/// @param n how many
/// @param out set to the copy (NULL for none)
/// @return false if there wasn't room
static bool copy_out(uint32_t pos, uint32_t n, int16_t **out) {
    *out = NULL;
    if (!n)
        return true;
    int16_t *c = galloc((size_t)n * F->ch * sizeof *c);
    if (!c)
        return false;
    memcpy(c, F->d + (size_t)pos * F->ch, (size_t)n * F->ch * sizeof *c);
    gtouch(c, (size_t)n * F->ch * sizeof *c);
    *out = c;
    return true;
}

/// Keeps what is about to change, so it can be undone: frames pos to pos +
/// old, which the change makes into new frames.
/// @param pos where the change is
/// @param old the frames it replaces
/// @param new_len the frames it puts there
/// @param what its name
static void undo_save(uint32_t pos, uint32_t old, uint32_t new_len, const char *what) {
    while (F->nredo)
        undo_free(&F->redo[--F->nredo]);
    undo_t u = {0};
    u.pos = pos;
    u.len = new_len;
    u.n = old;
    u.sel0 = F->sel0;
    u.sel1 = F->sel1;
    snprintf(u.what, sizeof u.what, "%s", what);
    F->undo_lost = false;
    if (!copy_out(pos, old, &u.d)) {
        undo_forget();
        F->undo_lost = true;  // done all the same, without undo
        return;
    }
    undo_put(F->undo, &F->nundo, &u);
}

static bool replace(uint32_t pos, uint32_t del, const int16_t *src, uint32_t ins);

/// The sound's format and frames, as an undo step that puts them back.
/// @param u filled in (the frames themselves, not a copy)
static void format_step(undo_t *u) {
    memset(u, 0, sizeof *u);
    u->fmt = true;
    u->d = F->d;
    u->n = F->len;
    u->cap = F->cap;
    u->ch = F->ch;
    u->bits = F->bits;
    u->rate = F->rate;
    u->sel0 = F->sel0;
    u->sel1 = F->sel1;
    snprintf(u->what, sizeof u->what, "Format");
}

/// Undoes or redoes a step.
/// @param redo redo (else undo)
static void undo_step(bool redo) {
    undo_t *from = redo ? F->redo : F->undo, *to = redo ? F->undo : F->redo;
    int *nfrom = redo ? &F->nredo : &F->nundo, *nto = redo ? &F->nundo : &F->nredo;
    if (!*nfrom) {
        message("Nothing to %s", redo ? "redo" : "undo");
        return;
    }
    play_stop();
    undo_t u = from[--*nfrom], back = {0};
    if (u.fmt) {
        format_step(&back);
        if (!u.d) {  // only the format
            back.d = NULL;
        } else {
            F->d = u.d;
            F->len = u.n;
            F->cap = u.cap;
        }
        F->ch = u.ch;
        F->bits = u.bits;
        F->rate = u.rate;
        summ_update(0);
    } else {
        back.pos = u.pos;
        back.len = u.n;
        back.n = u.len;
        if (!copy_out(u.pos, u.len, &back.d)) {
            ++*nfrom;
            message("Not enough memory to %s", redo ? "redo" : "undo");
            return;
        }
        if (!replace(u.pos, u.len, u.d, u.n)) {
            gfree(back.d);
            ++*nfrom;
            message("Not enough memory to %s", redo ? "redo" : "undo");
            return;
        }
        undo_free(&u);
    }
    back.sel0 = F->sel0;
    back.sel1 = F->sel1;
    snprintf(back.what, sizeof back.what, "%s", u.what);
    undo_put(to, nto, &back);
    F->sel0 = u.sel0;
    F->sel1 = u.sel1;
    if (F->sel1 > F->len)
        F->sel1 = F->len;
    if (F->sel0 > F->sel1)
        F->sel0 = F->sel1;
    F->modified = true;
    view_fix();
    message("%s: %s", redo ? "Redone" : "Undone", u.what);
}

// ---- Changing the sound ------------------------------------------------------------------------------

/// Makes room for a number of frames.
/// @param frames them
/// @return false if there isn't the memory
static bool reserve(uint32_t frames) {
    if (frames <= F->cap && F->d)
        return true;
    uint32_t cap = frames + F->rate;  // a second more, for the next paste
    int16_t *d = F->d ? grealloc(F->d, (size_t)cap * F->ch * sizeof *d) : galloc((size_t)cap * F->ch * sizeof *d);
    if (!d && F->d && frames + 64 > F->cap) {  // without the second
        cap = frames;
        d = grealloc(F->d, (size_t)cap * F->ch * sizeof *d);
    }
    if (!d)
        return false;
    F->d = d;
    F->cap = cap;
    return true;
}

/// Replaces frames pos to pos + del with ins frames from src (NULL: silence).
/// @param pos where
/// @param del the frames taken out
/// @param src the frames put in (the sound's channels), or NULL
/// @param ins how many
/// @return false if there wasn't the memory (nothing changed)
static bool replace(uint32_t pos, uint32_t del, const int16_t *src, uint32_t ins) {
    uint32_t nl = F->len - del + ins;
    if (!reserve(nl))
        return false;
    int ch = F->ch;
    memmove(F->d + (size_t)(pos + ins) * ch, F->d + (size_t)(pos + del) * ch,
            (size_t)(F->len - pos - del) * ch * sizeof *F->d);
    if (src)
        memcpy(F->d + (size_t)pos * ch, src, (size_t)ins * ch * sizeof *F->d);
    else
        memset(F->d + (size_t)pos * ch, 0, (size_t)ins * ch * sizeof *F->d);
    F->len = nl;
    if (nl > pos)
        gtouch(F->d + (size_t)pos * ch, (size_t)(nl - pos) * ch * sizeof *F->d);
    summ_update(pos);
    F->modified = true;
    F->cols_dirty = true;
    F->dirty = true;
    return true;
}

/// The part the effects work on: the selection, or all of it.
/// @param a set to its first frame
/// @param b set to the frame after it
/// @return there is something there
static bool target(uint32_t *a, uint32_t *b) {
    *a = F->sel1 > F->sel0 ? F->sel0 : 0;
    *b = F->sel1 > F->sel0 ? F->sel1 : F->len;
    if (*b <= *a) {
        message("There is no sound to change");
        return false;
    }
    return true;
}

/// Wants a selection.
/// @return there is one (else says so)
static bool need_sel(void) {
    if (F->sel1 > F->sel0)
        return true;
    message("Select some of the sound first (drag across it)");
    return false;
}

/// Tells how the change went.
/// @param what what was done
static void done(const char *what) {
    if (F->undo_lost)
        message("%s (too big to keep for undo)", what);
    else
        message("%s", what);
}

/// Copies the selection to the clipboard.
/// @return it was copied
static bool copy_sel(void) {
    if (!need_sel())
        return false;
    gfree(F->clip);
    F->clip = NULL;
    F->clip_len = 0;
    if (!copy_out(F->sel0, F->sel1 - F->sel0, &F->clip)) {
        message("Not enough memory to copy that");
        return false;
    }
    F->clip_len = F->sel1 - F->sel0;
    F->clip_ch = F->ch;
    F->dirty = true;
    return true;
}

/// Takes out the selection.
/// @param what the change's name
/// @return it was taken out
static bool delete_sel(const char *what) {
    if (!need_sel())
        return false;
    play_stop();
    uint32_t a = F->sel0, n = F->sel1 - F->sel0;
    undo_save(a, n, 0, what);
    replace(a, n, NULL, 0);  // smaller: always room
    F->sel1 = a;
    view_fix();
    return true;
}

/// Puts the clipboard in at the cursor, in place of the selection.
static void paste(void) {
    if (!F->clip_len) {
        message("The clipboard is empty (copy or cut first)");
        return;
    }
    play_stop();
    const int16_t *src = F->clip;
    int16_t *conv = NULL;
    if (F->clip_ch != F->ch) {  // mono into stereo, or stereo into mono
        conv = galloc((size_t)F->clip_len * F->ch * sizeof *conv);
        if (!conv) {
            message("Not enough memory to paste that");
            return;
        }
        for (uint32_t i = 0; i < F->clip_len; ++i) {
            if (F->ch == 2)
                conv[i * 2] = conv[i * 2 + 1] = F->clip[i];
            else
                conv[i] = (int16_t)((F->clip[i * 2] + F->clip[i * 2 + 1]) / 2);
        }
        src = conv;
    }
    uint32_t a = F->sel0, del = F->sel1 - F->sel0;
    if (!reserve(F->len - del + F->clip_len)) {
        message("Not enough memory to paste that");
    } else {
        undo_save(a, del, F->clip_len, "Paste");
        replace(a, del, src, F->clip_len);  // the room is there
        F->sel1 = a + F->clip_len;
        view_fix();
        done("Pasted");
    }
    gfree(conv);
}

/// Keeps only the selection.
static void trim(void) {
    if (!need_sel())
        return;
    play_stop();
    // Two steps, the end then the start, undone as one.
    uint32_t a = F->sel0, b = F->sel1, len = F->len;
    undo_save(0, len, b - a, "Trim");
    memmove(F->d, F->d + (size_t)a * F->ch, (size_t)(b - a) * F->ch * sizeof *F->d);
    F->len = b - a;
    gtouch(F->d, (size_t)F->len * F->ch * sizeof *F->d);
    summ_update(0);
    F->sel0 = 0;
    F->sel1 = F->len;
    F->view0 = 0;
    F->vlen = F->len;
    F->modified = true;
    view_fix();
    done("Trimmed");
}

/// The effects done to frames in place.
enum { FX_GAIN, FX_FADEIN, FX_FADEOUT, FX_REVERSE, FX_SILENCE };

/// Does an effect to the selection (or all of it).
/// @param fx FX_*
/// @param q16 the gain (FX_GAIN), 65536 = as it is
static void effect(int fx, int32_t q16) {
    uint32_t a, b;
    if (!target(&a, &b))
        return;
    play_stop();
    static const char *const names[] = {"Gain", "Fade in", "Fade out", "Reverse", "Silence"};
    undo_save(a, b - a, b - a, names[fx]);
    int ch = F->ch;
    uint32_t n = b - a;
    int16_t *d = F->d + (size_t)a * ch;
    switch (fx) {
    case FX_GAIN:
        for (size_t i = 0; i < (size_t)n * ch; ++i) {
            int32_t v = (int32_t)(((int64_t)d[i] * q16 + 32768) >> 16);
            d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        break;
    case FX_FADEIN:
    case FX_FADEOUT:
        for (uint32_t i = 0; i < n; ++i) {
            int64_t g = n > 1 ? (int64_t)(fx == FX_FADEIN ? i : n - 1 - i) * 65536 / (n - 1) : 0;
            for (int c = 0; c < ch; ++c)
                d[i * ch + c] = (int16_t)((d[i * ch + c] * g) >> 16);
        }
        break;
    case FX_REVERSE:
        for (uint32_t i = 0, j = n - 1; i < j; ++i, --j)
            for (int c = 0; c < ch; ++c) {
                int16_t t = d[i * ch + c];
                d[i * ch + c] = d[j * ch + c];
                d[j * ch + c] = t;
            }
        break;
    case FX_SILENCE: memset(d, 0, (size_t)n * ch * sizeof *d); break;
    }
    gtouch(d, (size_t)n * ch * sizeof *d);
    summ_update(a);
    F->modified = true;
    F->cols_dirty = true;
    F->dirty = true;
    done(names[fx]);
}

/// Makes the loudest sample of the selection (or all of it) full scale.
static void normalize(void) {
    uint32_t a, b;
    if (!target(&a, &b))
        return;
    int peak = 0;
    for (size_t i = (size_t)a * F->ch; i < (size_t)b * F->ch; ++i) {
        int v = abs(F->d[i]);
        peak = v > peak ? v : peak;
    }
    if (!peak) {
        message("It is silent: nothing to normalize");
        return;
    }
    effect(FX_GAIN, (int32_t)(32767LL * 65536 / peak));
    char s[20];
    fmt_db(s, sizeof s, peak);
    message("Normalized: it peaked at %s", s);
}

/// Silence: the selection made silent, or with none a second put in at the cursor.
static void silence(void) {
    if (F->sel1 > F->sel0) {
        effect(FX_SILENCE, 0);
        return;
    }
    play_stop();
    uint32_t a = F->sel0;
    if (!reserve(F->len + F->rate)) {
        message("Not enough memory");
        return;
    }
    undo_save(a, 0, F->rate, "Silence");
    replace(a, 0, NULL, F->rate);  // the room is there
    F->sel1 = a + F->rate;
    view_fix();
    done("A second of silence put in");
}

/// Changes the format: the channels and the rate (resampled, or only relabelled), and the bits saved.
/// @param rate the new rate
/// @param chans 1 or 2
/// @param bits 8 or 16
/// @param resample work the samples out for the new rate (else they are kept, and the speed changes)
static void set_format(uint32_t rate, int chans, int bits, bool resample) {
    if (rate == F->rate && chans == F->ch && bits == F->bits)
        return;
    play_stop();
    undo_t u;
    format_step(&u);
    bool data = chans != F->ch || (rate != F->rate && resample);
    if (!data) {
        u.d = NULL;
        while (F->nredo)
            undo_free(&F->redo[--F->nredo]);
        undo_put(F->undo, &F->nundo, &u);
        F->rate = rate;
        F->bits = bits;
        F->modified = true;
        F->dirty = true;
        message("Format: %lu Hz %d-bit %s", (unsigned long)rate, bits, chans == 2 ? "stereo" : "mono");
        return;
    }
    busy("Working...");
    uint32_t step = (uint32_t)(((uint64_t)F->rate << 16) / (resample ? rate : F->rate));
    uint64_t nl64 = resample ? (uint64_t)F->len * rate / F->rate : F->len;
    uint32_t nl = (uint32_t)nl64, cap = nl + rate;
    int16_t *nd = galloc((size_t)cap * chans * sizeof *nd);
    if (!nd) {
        cap = nl;
        nd = galloc((size_t)(cap ? cap : 1) * chans * sizeof *nd);
    }
    if (!nd) {
        message("Not enough memory for that format");
        return;
    }
    // Each new frame: between two old ones; going down in rate, the average of those it covers.
    for (uint32_t j = 0; j < nl; ++j) {
        uint64_t t = (uint64_t)j * step;
        int v[2] = {0, 0};
        for (int c = 0; c < F->ch; ++c) {
            if (step > 65536) {
                uint32_t a = (uint32_t)(t >> 16), b = (uint32_t)((t + step) >> 16);
                if (b > F->len)
                    b = F->len;
                int64_t s = 0;
                for (uint32_t i = a; i < b; ++i)
                    s += F->d[(size_t)i * F->ch + c];
                v[c] = b > a ? (int)(s / (int64_t)(b - a)) : 0;
            } else {
                v[c] = value_at(t, c);
            }
        }
        if (F->ch == 1)
            v[1] = v[0];
        if (chans == 1)
            nd[j] = (int16_t)(F->ch == 2 ? (v[0] + v[1]) / 2 : v[0]);
        else
            nd[j * 2] = (int16_t)v[0], nd[j * 2 + 1] = (int16_t)v[1];
    }
    gtouch(nd, (size_t)nl * chans * sizeof *nd);
    while (F->nredo)
        undo_free(&F->redo[--F->nredo]);
    undo_put(F->undo, &F->nundo, &u);  // the old sound itself, for undo
    if (resample && rate != F->rate) {
        F->sel0 = (uint32_t)((uint64_t)F->sel0 * rate / F->rate);
        F->sel1 = (uint32_t)((uint64_t)F->sel1 * rate / F->rate);
        F->view0 = (uint32_t)((uint64_t)F->view0 * rate / F->rate);
        F->vlen = (uint32_t)((uint64_t)F->vlen * rate / F->rate);
    }
    F->d = nd;
    F->len = nl;
    F->cap = cap;
    F->ch = chans;
    F->rate = rate;
    F->bits = bits;
    if (F->sel1 > nl)
        F->sel1 = nl;
    if (F->sel0 > F->sel1)
        F->sel0 = F->sel1;
    summ_update(0);
    F->modified = true;
    view_fix();
    F->dirty = true;
    message("Format: %lu Hz %d-bit %s", (unsigned long)rate, bits, chans == 2 ? "stereo" : "mono");
}

// ---- The view ---------------------------------------------------------------------------------------

/// Zooms around a frame, keeping it where it is on the screen.
/// @param p the frame
/// @param in in (else out)
static void zoom(uint32_t p, bool in) {
    int x = frame_x(p);
    if (x < 0)
        x = WV_W / 2;
    uint32_t vlen = in ? F->vlen / 2 : (F->vlen > 0x7fffffff ? F->vlen : F->vlen * 2);
    if (vlen < WV_W / 16)
        vlen = WV_W / 16;
    uint64_t left = (uint64_t)x * vlen / WV_W;
    F->view0 = p > left ? (uint32_t)(p - left) : 0;
    F->vlen = vlen;
    view_fix();
    F->dirty = true;
}

/// Shows frames a to b.
/// @param a the first
/// @param b the one after the last
static void zoom_to(uint32_t a, uint32_t b) {
    F->view0 = a;
    F->vlen = b > a ? b - a : 1;
    view_fix();
    F->dirty = true;
}

/// Moves the view.
/// @param d frames (- earlier)
static void scroll(int64_t d) {
    int64_t v = (int64_t)F->view0 + d;
    F->view0 = v < 0 ? 0 : (uint32_t)v;
    view_fix();
    F->dirty = true;
}

// ---- Files ----------------------------------------------------------------------------------------

/// A little-endian 16-bit value.
/// @param p it
/// @return its value
static uint32_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

/// A little-endian 32-bit value.
/// @param p it
/// @return its value
static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/// Writes a little-endian 32-bit value.
/// @param p where
/// @param v the value
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/// What a WAV file holds.
typedef struct {
    int format;          ///< 1 PCM, 3 float
    int channels;        ///< its channels
    int bits;            ///< bits a sample
    uint32_t rate;       ///< frames a second
    uint32_t data_at;    ///< where the samples are
    uint32_t data_len;   ///< their bytes
    uint32_t frames;     ///< frames
} wav_info_t;

/// Reads a WAV file's chunks, as far as its format and where its samples are.
/// @param f the file, open
/// @param wi filled in
/// @return NULL, or what's wrong
static const char *wav_scan(FIL *f, wav_info_t *wi) {
    uint8_t h[40];
    UINT got;
    uint32_t size = (uint32_t)f_size(f);
    memset(wi, 0, sizeof *wi);
    if (f_read(f, h, 12, &got) != FR_OK || got != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return "not a WAV file";
    bool have_data = false;
    for (uint32_t off = 12; off + 8 <= size;) {
        if (f_lseek(f, off) != FR_OK || f_read(f, h, 8, &got) != FR_OK || got != 8)
            break;
        uint32_t n = le32(h + 4);
        if (n > size - off - 8)
            n = size - off - 8;  // cut short, or a stream's size not filled in
        if (!memcmp(h, "fmt ", 4) && n >= 16) {
            UINT want = n < sizeof h ? n : sizeof h;
            if (f_read(f, h, want, &got) != FR_OK || got != want)
                return "a broken fmt chunk";
            wi->format = (int)le16(h);
            wi->channels = (int)le16(h + 2);
            wi->rate = le32(h + 4);
            wi->bits = (int)le16(h + 14);
            if (wi->format == 0xfffe && want >= 26)
                wi->format = (int)le16(h + 24);  // extensible: the sub-format
        } else if (!memcmp(h, "data", 4) && !have_data) {
            wi->data_at = off + 8;
            wi->data_len = n;
            have_data = true;
        }
        off += 8 + n + (n & 1);
    }
    if (!wi->channels || !wi->bits)
        return "no format (fmt chunk)";
    if (!have_data)
        return "no sound (data chunk)";
    if (!(wi->format == 1 && (wi->bits == 8 || wi->bits == 16 || wi->bits == 24 || wi->bits == 32)) &&
        !(wi->format == 3 && wi->bits == 32))
        return "only PCM or 32-bit float WAV files";
    if (wi->rate < 1000 || wi->rate > 192000)
        return "a rate it can't play";
    wi->frames = wi->data_len / (uint32_t)(wi->bits / 8 * wi->channels);
    return NULL;
}

/// Loads a WAV file in place of the sound.
/// @param path the file
/// @return it loaded
static bool load(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        message("Can't open %.60s", path);
        return false;
    }
    wav_info_t wi;
    const char *err = wav_scan(&f, &wi);
    if (err) {
        f_close(&f);
        message("%.40s: %s", path, err);
        return false;
    }
    play_stop();
    int ch = wi.channels > 1 ? 2 : 1;
    size_t bytes = (size_t)wi.frames * ch * sizeof(int16_t);
    int16_t *d = galloc(bytes);
    if (!d && !F->modified) {  // the sound now is in its file: make way
        undo_forget();
        gfree(F->d);
        F->d = NULL;
        F->len = F->cap = 0;
        d = galloc(bytes);
    }
    if (!d) {
        f_close(&f);
        message("Too long: %lu KB of memory needed, %lu free", (unsigned long)(bytes >> 10),
                (unsigned long)(psram_free_total() >> 10));
        return false;
    }
    busy("Loading...");
    int bps = wi.bits / 8, fsz = bps * wi.channels;
    UINT chunk = (UINT)(sizeof F->io / fsz * fsz), got;
    f_lseek(&f, wi.data_at);
    uint32_t i = 0;
    bool ok = true;
    while (i < wi.frames) {
        UINT want = (wi.frames - i) * fsz < chunk ? (wi.frames - i) * fsz : chunk;
        if (f_read(&f, F->io, want, &got) != FR_OK || got != want) {
            ok = false;
            break;
        }
        for (UINT k = 0; k < want; k += fsz, ++i)
            for (int c = 0; c < ch; ++c) {
                const uint8_t *p = F->io + k + c * bps;
                int v;
                switch (wi.bits) {
                case 8: v = (p[0] - 128) * 256; break;
                case 16: v = (int16_t)le16(p); break;
                case 24: v = (int16_t)(p[1] | p[2] << 8); break;
                default:
                    if (wi.format == 3) {
                        float fv;
                        uint32_t u = le32(p);
                        memcpy(&fv, &u, 4);
                        fv = fv > 1.0f ? 1.0f : fv < -1.0f ? -1.0f : fv;
                        v = (int)lrintf(fv * 32767.0f);
                    } else {
                        v = (int16_t)le16(p + 2);
                    }
                }
                d[(size_t)i * ch + c] = (int16_t)v;
            }
    }
    f_close(&f);
    if (!ok) {
        gfree(d);
        message("%.40s: can't read it all", path);
        return false;
    }
    gtouch(d, bytes);
    undo_forget();
    gfree(F->d);
    F->d = d;
    F->len = F->cap = wi.frames;
    F->ch = ch;
    F->rate = wi.rate;
    F->bits = wi.bits == 8 ? 8 : 16;
    F->sel0 = F->sel1 = 0;
    F->view0 = 0;
    F->vlen = F->len;
    view_fix();
    summ_update(0);
    snprintf(F->path, sizeof F->path, "%s", path);
    F->modified = false;
    F->damaged = false;
    F->note[0] = 0;
    if (wi.bits > 16 || wi.channels > 2)
        snprintf(F->note, sizeof F->note, "%s%s", wi.bits > 16 ? "; kept as 16-bit" : "",
                 wi.channels > 2 ? "; first 2 channels" : "");
    char t[20];
    fmt_time(t, sizeof t, F->len);
    message("%.32s: %s, %lu Hz %d-bit %s%s", path, t, (unsigned long)wi.rate, wi.bits,
            wi.channels == 1 ? "mono" : wi.channels == 2 ? "stereo" : "channels", F->note);
    F->dirty = true;
    return true;
}

/// Writes the sound as a WAV file, in its bits.
/// @param path the file
/// @return it was written
static bool write_file(const char *path) {
    if (F->damaged && !strcmp(path, F->path)) {
        message("The sound may be damaged: Save as a new file");
        return false;
    }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        message("Can't write %.60s", path);
        return false;
    }
    busy("Saving...");
    int bps = F->bits / 8, ch = F->ch;
    uint32_t data = F->len * (uint32_t)(bps * ch);
    uint8_t *h = F->io;
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data + (data & 1));
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1;  // PCM
    h[22] = (uint8_t)ch;
    put32(h + 24, F->rate);
    put32(h + 28, F->rate * (uint32_t)(bps * ch));
    h[32] = (uint8_t)(bps * ch);
    h[34] = (uint8_t)F->bits;
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    UINT put;
    bool ok = f_write(&f, h, 44, &put) == FR_OK && put == 44;
    size_t n = (size_t)F->len * ch, i = 0;
    while (ok && i < n) {
        UINT k = 0;
        for (; i < n && k + 2 <= sizeof F->io; ++i) {
            int v = F->d[i];
            if (bps == 1) {
                int b = (v + 128) >> 8;
                F->io[k++] = (uint8_t)((b > 127 ? 127 : b) + 128);
            } else {
                F->io[k++] = (uint8_t)v;
                F->io[k++] = (uint8_t)(v >> 8);
            }
        }
        ok = f_write(&f, F->io, k, &put) == FR_OK && put == k;
    }
    if (ok && (data & 1)) {
        F->io[0] = 0;
        ok = f_write(&f, F->io, 1, &put) == FR_OK && put == 1;
    }
    if (f_close(&f) != FR_OK)
        ok = false;
    if (!ok) {
        message("%.40s: writing failed (is the drive full?)", path);
        return false;
    }
    snprintf(F->path, sizeof F->path, "%s", path);
    F->modified = false;
    message("Saved %.40s (%lu KB)", path, (unsigned long)((44 + data) >> 10));
    return true;
}

/// Starts afresh: an empty sound at 44100 Hz, mono, 16-bit.
static void new_sound(void) {
    play_stop();
    undo_forget();
    F->len = 0;
    F->ch = 1;
    F->bits = 16;
    F->rate = 44100;
    if (F->d && F->cap > F->rate) {
        int16_t *d = grealloc(F->d, (size_t)F->rate * sizeof *d);
        if (d)
            F->d = d, F->cap = F->rate;
    }
    if (!reserve(1))
        F->cap = 0;
    F->sel0 = F->sel1 = 0;
    F->view0 = 0;
    F->vlen = F->rate;
    F->path[0] = 0;
    F->modified = false;
    F->damaged = false;
    view_fix();
    summ_update(0);
    F->dirty = true;
}

// ---- The file box ---------------------------------------------------------------------------------

/// Whether a name ends in an extension.
/// @param name the name
/// @param ext ".WAV" or the like
/// @return it does (any case)
static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && !strcasecmp(name + n - e, ext);
}

/// Sorts folders first, then by name.
/// @param a an entry
/// @param b another
/// @return their order
static int fd_compare(const void *a, const void *b) {
    const fd_entry_t *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/// Reads the folder shown into the list (folders, and WAV files).
static void fd_read(void) {
    F->fd_n = F->fd_sel = F->fd_top = 0;
    DIR d;
    if (f_opendir(&d, F->fd_dir) != FR_OK) {
        message("Can't open %.60s", F->fd_dir);
        return;
    }
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] && F->fd_n < MAX_FILES) {
        bool dir = fi.fattrib & AM_DIR;
        if (fi.fname[0] == '.' || (!dir && !has_ext(fi.fname, ".WAV")))
            continue;
        fd_entry_t *e = &F->fd[F->fd_n++];
        snprintf(e->name, sizeof e->name, "%s", fi.fname);
        e->dir = dir;
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

/// Opens the file box.
/// @param mode FD_*
static void fd_open(int mode) {
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
        char p[160];
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
            fd_go(F->fd[F->fd_sel].name);
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
    if (F->fd_mode == FD_LOAD) {
        F->dlg = DLG_NONE;
        F->dirty = true;
        load(p);
        return;
    }
    if (!strchr(F->fd_name, '.'))
        strncat(p, ".WAV", sizeof p - strlen(p) - 1);
    snprintf(F->target, sizeof F->target, "%s", p);
    F->dlg = DLG_NONE;
    F->dirty = true;
    if (f_stat(p, &fi) == FR_OK && strcmp(p, F->path)) {
        char q[96];
        const char *n = strrchr(p, '/');
        snprintf(q, sizeof q, "%.40s is there already: replace it?", n ? n + 1 : p);
        ask(q, "Replace", ACT_WRITE, "Cancel", ACT_NONE, NULL, ACT_NONE);
        return;
    }
    act(ACT_WRITE);
}

/// Does something, asking first if changes would be lost.
/// @param a ACT_*
static void act(int a) {
    F->dlg = DLG_NONE;
    F->dirty = true;
    switch (a) {
    case ACT_NEW: new_sound(); break;
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

/// A file button.
/// @param b FB_*
static void file_button(int b) {
    switch (b) {
    case FB_NEW: act_unsaved(ACT_NEW); break;
    case FB_LOAD: act_unsaved(ACT_LOAD); break;
    case FB_SAVE: F->then = ACT_NONE; act(ACT_SAVE_THEN); break;
    case FB_SAVEAS: F->then = ACT_NONE; fd_open(FD_SAVE); break;
    case FB_FORMAT:
        F->f_rate = (int)F->rate;
        F->f_ch = F->ch;
        F->f_bits = F->bits;
        F->f_resample = true;
        F->dlg = DLG_FORMAT;
        break;
    case FB_UNDO: undo_step(false); break;
    case FB_REDO: undo_step(true); break;
    case FB_HELP: F->dlg = DLG_HELP; break;
    case FB_QUIT: act_unsaved(ACT_QUIT); break;
    }
    F->dirty = true;
}

/// A play, view, edit or effect button.
/// @param b TB_*
static void tool_button(int b) {
    F->dirty = true;
    switch (b) {
    case TB_PLAY:
        if (F->pl_on)
            play_stop();
        else
            play_start(-1);
        break;
    case TB_LOOP: F->loop = !F->loop; F->pl_loop = F->loop; break;
    case TB_ZIN: zoom(F->sel1 > F->sel0 ? F->sel0 + (F->sel1 - F->sel0) / 2 : F->sel0, true); break;
    case TB_ZOUT: zoom(F->sel1 > F->sel0 ? F->sel0 + (F->sel1 - F->sel0) / 2 : F->sel0, false); break;
    case TB_ZSEL:
        if (need_sel())
            zoom_to(F->sel0, F->sel1);
        break;
    case TB_ZALL: zoom_to(0, F->len); break;
    case TB_ALL: F->sel0 = 0; F->sel1 = F->len; break;
    case TB_CUT:
        if (copy_sel() && delete_sel("Cut"))
            done("Cut");
        break;
    case TB_COPY:
        if (copy_sel())
            message("Copied to the clipboard");
        break;
    case TB_PASTE: paste(); break;
    case TB_DELETE:
        if (delete_sel("Delete"))
            done("Deleted");
        break;
    case TB_TRIM: trim(); break;
    case TB_SILENCE: silence(); break;
    case TB_REVERSE: effect(FX_REVERSE, 0); break;
    case TB_GAIN:
        if (F->len) {
            F->gain_db10 = 0;
            F->dlg = DLG_GAIN;
        }
        break;
    case TB_NORMAL: normalize(); break;
    case TB_FADEIN: effect(FX_FADEIN, 0); break;
    case TB_FADEOUT: effect(FX_FADEOUT, 0); break;
    }
}

// ---- Keys and the mouse ------------------------------------------------------------------------------

/// A key while the file box is up.
/// @param k the key
static void fd_key(int k) {
    int n = strlen(F->fd_name);
    switch (k) {
    case '\r': case '\n': fd_ok(); break;
    case KEY_ESC: F->dlg = DLG_NONE; F->then = ACT_NONE; break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -FD_ROWS : FD_ROWS;
        F->fd_sel += d;
        if (F->fd_sel < 0)
            F->fd_sel = 0;
        if (F->fd_sel >= F->fd_n)
            F->fd_sel = F->fd_n - 1;
        if (F->fd_sel >= 0 && F->fd_sel < F->fd_n)
            snprintf(F->fd_name, sizeof F->fd_name, "%s", F->fd[F->fd_sel].dir ? "" : F->fd[F->fd_sel].name);
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

/// The gain box's gain applied.
static void gain_ok(void) {
    F->dlg = DLG_NONE;
    if (F->gain_db10)
        effect(FX_GAIN, (int32_t)lrintf(powf(10.0f, F->gain_db10 / 200.0f) * 65536.0f));
}

/// Changes the gain box's gain.
/// @param d tenths of a dB
static void gain_add(int d) {
    F->gain_db10 += d;
    F->gain_db10 = F->gain_db10 < -480 ? -480 : F->gain_db10 > 480 ? 480 : F->gain_db10;
}

/// A key while the gain box is up.
/// @param k the key
static void gain_key(int k) {
    switch (k < 0x100 ? toupper(k) : k) {
    case '\r': case '\n': gain_ok(); break;
    case KEY_ESC: F->dlg = DLG_NONE; break;
    case 'N': F->dlg = DLG_NONE; normalize(); break;
    case KEY_RIGHT: case '+': case '=': gain_add(10); break;
    case KEY_LEFT: case '-': gain_add(-10); break;
    case KEY_UP: gain_add(1); break;
    case KEY_DOWN: gain_add(-1); break;
    case KEY_PGUP: gain_add(60); break;
    case KEY_PGDN: gain_add(-60); break;
    }
}

/// The format box's choices applied.
static void format_ok(void) {
    F->dlg = DLG_NONE;
    set_format((uint32_t)F->f_rate, F->f_ch, F->f_bits, F->f_resample);
}

/// A key while the format box is up.
/// @param k the key
static void format_key(int k) {
    int r = 0;
    while (r < NRATES - 1 && rates[r] < (uint32_t)F->f_rate)
        ++r;
    switch (k) {
    case '\r': case '\n': format_ok(); break;
    case KEY_ESC: F->dlg = DLG_NONE; break;
    case KEY_LEFT: F->f_rate = (int)rates[r > 0 ? r - 1 : 0]; break;
    case KEY_RIGHT: F->f_rate = (int)rates[r < NRATES - 1 ? r + 1 : r]; break;
    case 'm': case 'M': F->f_ch = 1; break;
    case 's': case 'S': F->f_ch = 2; break;
    case '8': F->f_bits = 8; break;
    case '1': F->f_bits = 16; break;
    case 'r': case 'R': F->f_resample = !F->f_resample; break;
    }
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    F->dirty = true;
    switch (F->dlg) {
    case DLG_HELP: F->dlg = DLG_NONE; return;
    case DLG_FILE: fd_key(k); return;
    case DLG_GAIN: gain_key(k); return;
    case DLG_FORMAT: format_key(k); return;
    case DLG_ASK:
        if (k == '\r' || k == '\n')
            act(F->ask_act[0]);
        else if (k == KEY_ESC)
            F->dlg = DLG_NONE, F->then = ACT_NONE;
        return;
    }
    if (F->drag)
        return;
    int up = k < 0x100 ? toupper(k) : k;
    uint32_t mid = F->sel1 > F->sel0 ? F->sel0 + (F->sel1 - F->sel0) / 2 : F->sel0;
    switch (up) {
    case ' ': tool_button(TB_PLAY); break;
    case 'L': tool_button(TB_LOOP); break;
    case KEY_HOME: F->sel0 = F->sel1 = 0; scroll(-(int64_t)F->view0); break;
    case KEY_END: F->sel0 = F->sel1 = F->len; scroll(F->len); break;
    case KEY_LEFT: scroll(-(int64_t)(F->vlen / 8 ? F->vlen / 8 : 1)); break;
    case KEY_RIGHT: scroll(F->vlen / 8 ? F->vlen / 8 : 1); break;
    case KEY_PGUP: scroll(-(int64_t)F->vlen); break;
    case KEY_PGDN: scroll(F->vlen); break;
    case KEY_UP: case '+': case '=': zoom(mid, true); break;
    case KEY_DOWN: case '-': zoom(mid, false); break;
    case 'Z': tool_button(TB_ZSEL); break;
    case 'A': tool_button(TB_ZALL); break;
    case CTRL('a'): tool_button(TB_ALL); break;
    case CTRL('x'): tool_button(TB_CUT); break;
    case CTRL('c'): tool_button(TB_COPY); break;
    case CTRL('v'): tool_button(TB_PASTE); break;
    case KEY_DEL: case 8: case 0x7f: tool_button(TB_DELETE); break;
    case 'T': tool_button(TB_TRIM); break;
    case 'S': tool_button(TB_SILENCE); break;
    case 'R': tool_button(TB_REVERSE); break;
    case 'G': tool_button(TB_GAIN); break;
    case 'N': tool_button(TB_NORMAL); break;
    case 'I': tool_button(TB_FADEIN); break;
    case 'O': tool_button(TB_FADEOUT); break;
    case 'F': file_button(FB_FORMAT); break;
    case 'U': case CTRL('z'): undo_step(false); break;
    case CTRL('y'): undo_step(true); break;
    case CTRL('s'): case KEY_F2: file_button(FB_SAVE); break;
    case KEY_F3: case CTRL('o'): file_button(FB_LOAD); break;
    case CTRL('n'): file_button(FB_NEW); break;
    case KEY_F1: F->dlg = DLG_HELP; break;
    case KEY_ESC:
        if (F->pl_on)
            play_stop();
        else
            file_button(FB_QUIT);
        break;
    }
}

/// Updates the line about what the pointer is over, if that changed.
/// @param e the mouse event
static void hover(const mouse_event_t *e) {
    F->mx = e->x;
    F->my = e->y;
    int x = e->x >= WV_X && e->x < WV_X + WV_W && e->y >= RUL_Y && e->y < WV_Y + WV_H ? e->x - WV_X : -1;
    if (x == F->hover_x)
        return;
    F->hover_x = x;
    if (!F->dlg && !F->dirty) {
        F->t = dvi_framebuf;  // straight onto the screen
        draw_info();
    }
}

/// The selection from the anchor to a frame.
/// @param p the frame
static void select_to(uint32_t p) {
    F->sel0 = p < F->anchor ? p : F->anchor;
    F->sel1 = p < F->anchor ? F->anchor : p;
}

/// Redraws the waveform and the lines about it straight onto the screen (while dragging).
static void redraw_wave(void) {
    F->t = dvi_framebuf;
    draw_wave();
    draw_info();
}

/// Moves the view to put a frame in the middle, from a point on the overview.
/// @param sx the point's x
static void over_to(int sx) {
    if (!F->len)
        return;
    int x = sx - OV_X;
    x = x < 0 ? 0 : x >= OV_W ? OV_W - 1 : x;
    uint32_t p = (uint32_t)((uint64_t)x * F->len / OV_W);
    F->view0 = p > F->vlen / 2 ? p - F->vlen / 2 : 0;
    view_fix();
    F->dirty = true;
}

/// A click in the gain or format box.
/// @param h what was clicked
static void box_click(const hit_t *h) {
    switch (h->id) {
    case HIT_GAIN: gain_add(gain_steps[h->arg]); break;
    case HIT_GAIN_OK: gain_ok(); break;
    case HIT_GAIN_NORM: F->dlg = DLG_NONE; normalize(); break;
    case HIT_BOX_CANCEL: F->dlg = DLG_NONE; break;
    case HIT_RATE: F->f_rate = (int)rates[h->arg]; break;
    case HIT_CHANS: F->f_ch = h->arg; break;
    case HIT_BITS: F->f_bits = h->arg; break;
    case HIT_RESAMPLE: F->f_resample = !F->f_resample; break;
    case HIT_FMT_OK: format_ok(); break;
    }
}

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    if (e->type == MOUSE_UP) {
        if (F->drag && !(e->buttons & (MOUSE_LEFT | MOUSE_RIGHT))) {
            F->drag = DRAG_NONE;
            F->dirty = true;
        }
        return;
    }
    if (e->type == MOUSE_MOVE) {
        hover(e);
        if (F->drag == DRAG_SEL) {
            select_to(x_frame(e->x));
            redraw_wave();
        } else if (F->drag == DRAG_OVER) {
            over_to(e->x);
        }
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type == MOUSE_WHEEL) {
        int d = e->wheel > 0 ? -1 : 1;
        if (F->dlg == DLG_FILE) {
            F->fd_sel += d * 3;
            F->fd_sel = F->fd_sel < 0 ? 0 : F->fd_sel >= F->fd_n ? F->fd_n - 1 : F->fd_sel;
            F->dirty = true;
        } else if (F->dlg == DLG_GAIN) {
            gain_add(-d * 10);
            F->dirty = true;
        } else if (F->dlg || F->drag) {
            return;
        } else if (h && h->id == HIT_OVER) {
            scroll(d * (int64_t)(F->vlen / 4 ? F->vlen / 4 : 1));
        } else {
            zoom(h && h->id == HIT_WAVE ? x_frame(e->x) : F->sel0, d < 0);
        }
        return;
    }
    if (e->type != MOUSE_DOWN || !h || F->drag)
        return;
    F->dirty = true;
    switch (h->id) {
    case HIT_WAVE: {
        uint32_t p = x_frame(e->x);
        if (e->button == MOUSE_MIDDLE) {
            play_start(p);
        } else if (e->button == MOUSE_RIGHT) {  // the nearer end follows
            uint32_t d0 = p > F->sel0 ? p - F->sel0 : F->sel0 - p, d1 = p > F->sel1 ? p - F->sel1 : F->sel1 - p;
            F->anchor = d0 < d1 ? F->sel1 : F->sel0;
            select_to(p);
            F->drag = DRAG_SEL;
        } else if (e->double_click) {
            F->sel0 = 0;
            F->sel1 = F->len;
        } else {
            F->anchor = F->sel0 = F->sel1 = p;
            F->drag = DRAG_SEL;
        }
        break;
    }
    case HIT_OVER:
        over_to(e->x);
        F->drag = DRAG_OVER;
        break;
    case HIT_TOOL: tool_button(h->arg); break;
    case HIT_FILE: file_button(h->arg); break;
    case HIT_ASK: act(F->ask_act[h->arg]); break;
    case HIT_ROW: {
        F->fd_sel = h->arg;
        const fd_entry_t *fe = &F->fd[h->arg];
        if (fe->dir) {  // folders open at a click
            F->fd_name[0] = 0;
            fd_go(fe->name);
        } else {
            snprintf(F->fd_name, sizeof F->fd_name, "%s", fe->name);
            if (e->double_click)
                fd_ok();
        }
        break;
    }
    case HIT_FD_OK: fd_ok(); break;
    case HIT_FD_CANCEL: F->dlg = DLG_NONE; F->then = ACT_NONE; break;
    case HIT_FD_UP: fd_go(".."); break;
    case HIT_SWALLOW:
        if (F->dlg == DLG_HELP)
            F->dlg = DLG_NONE;
        break;
    default: box_click(h); break;
    }
}

/// While dragging a selection past either end of the waveform: the view follows.
static void drag_scroll(void) {
    if (F->drag != DRAG_SEL || (F->mx >= WV_X && F->mx < WV_X + WV_W))
        return;
    uint32_t now = now_ms();
    if ((int32_t)(now - F->next_scroll) < 0)
        return;
    F->next_scroll = now + 30;
    uint32_t d = F->vlen / 20 ? F->vlen / 20 : 1;
    if (F->mx < WV_X)
        F->view0 = F->view0 > d ? F->view0 - d : 0;
    else
        F->view0 += d;
    view_fix();
    select_to(F->mx < WV_X ? F->view0 : (F->view0 + F->vlen < F->len ? F->view0 + F->vlen : F->len));
    redraw_wave();
    F->t = dvi_framebuf;
    draw_ruler();
}

// ---- The editor ----------------------------------------------------------------------------------

/// Starts the state (PSRAM): an empty sound.
/// @return false if there is no memory for it
static bool start(void) {
    F = psram_malloc(sizeof *F);
    if (!F)
        return false;
    memset(F, 0, sizeof *F);
    F->fd = psram_malloc(MAX_FILES * sizeof *F->fd);
    if (!F->fd) {
        psram_free(F);
        F = NULL;
        return false;
    }
    F->ph_x = F->hover_x = -1;
    F->t = dvi_framebuf;
    F->refresh_at = now_ms() + REFRESH_MS;
    new_sound();
    return true;
}

/// Frees the state.
static void stop(void) {
    F->pl_on = false;
    if (audio_get_source() == play_src) {
        audio_set_source(NULL);
        sleep_ms(5);  // core 1 may be in the middle of a frame
    }
    undo_forget();
    while (F->nreg)
        gfree(F->reg[0].p);
    psram_free(F->g);
    psram_free(F->fd);
    psram_free(F);
    F = NULL;
}

/// The editor.
static void editor(void) {
    F->g = psram_malloc(W * H);
    if (!F->g) {
        printf("wav: not enough memory\n");
        return;
    }
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
        mouse_pointer_update();
        uint32_t now = now_ms();
        drag_scroll();
        if ((int32_t)(now - F->next_ph) >= 0) {
            F->next_ph = now + 20;
            playhead();
        }
        if (!F->drag)
            refresh_step();
        bool msg_on = F->msg[0] && now < F->msg_until;
        if (msg_on != msg_shown) {
            msg_shown = msg_on;
            F->dirty = true;
        }
        if (F->dirty && !F->quit && !F->drag) {
            F->dirty = false;
            draw();
            show();
        }
        sleep_ms(F->drag ? 1 : 5);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
}

// ---- The command ----------------------------------------------------------------------------------

/// Prints how to use the command.
static void usage(void) {
    printf("usage: wav [file.wav]         the editor (wavedit too)\n"
           "       wav info file.wav      its format and length\n");
}

/// `wav info`: a WAV file's format and length.
/// @param path the file
static void info(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("wav: can't open %s\n", path);
        return;
    }
    wav_info_t wi;
    const char *err = wav_scan(&f, &wi);
    f_close(&f);
    if (err) {
        printf("wav: %s: %s\n", path, err);
        return;
    }
    uint64_t ms = (uint64_t)wi.frames * 1000 / wi.rate;
    printf("%s: %lu Hz, %d-bit%s, %d channel%s, %u:%02u.%03u (%lu frames)\n", path, (unsigned long)wi.rate, wi.bits,
           wi.format == 3 ? " float" : "", wi.channels, wi.channels == 1 ? "" : "s", (unsigned)(ms / 60000),
           (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000), (unsigned long)wi.frames);
}

void wav_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        usage();
        return;
    }
    if (argc > 2 && !strcmp(argv[1], "info")) {
        info(argv[2]);
        return;
    }
    if (!start()) {
        printf("wav: not enough memory\n");
        return;
    }
    if (argc > 1) {
        FILINFO fi;
        if (f_stat(argv[1], &fi) == FR_OK) {
            load(argv[1]);
        } else {
            snprintf(F->path, sizeof F->path, "%s%s", argv[1], strchr(argv[1], '.') ? "" : ".WAV");
            message("%s is new", F->path);
        }
    }
    editor();
    stop();
}
