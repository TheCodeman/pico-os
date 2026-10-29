/**
 * @file sprite_ui.c
 * @brief The `sprite` command: a graphical sprite editor in the tracker's style (bevelled panels drawn off the screen, the scanout mouse arrow), for RGB332 sprites up to 32x32 with up to 64 animation frames.
 *
 * The screen: the sprite, magnified, to draw on (the left mouse button draws
 * in the left colour, the right in the right colour); all 256 RGB332
 * colours and "none" (see-through); the tools (pencil, line, rectangle, box,
 * fill, oval, disc, picker) and flips, rotation and clearing; the frames as
 * thumbnails with the animation playing beside them; and the file buttons.
 * Files are .SPR (spr.h), which BASIC's SPRITE LOAD reads; PNG Export writes
 * the frames side by side as a PNG, and Load reads PNGs too.
 *
 * Memory: the editor's state, frames, undo and off-screen picture are in
 * PSRAM while it runs; nothing stays in SRAM.
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
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "lodepng.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "spr.h"
#include "sprite.h"

#define W DVI_WIDTH   ///< the screen's width
#define H DVI_HEIGHT  ///< its height
#define NONE 256      ///< the "colour" of a see-through pixel
#define MAX_UNDO 32   ///< drawing steps kept for undo (and as many for redo)
#define MAX_FILES 128 ///< entries the file box lists
#define PNG_MAX (1024 * 1024)  ///< the biggest PNG Load reads

/// LodePNG's free (image.c gives LodePNG its allocators).
/// @param ptr the memory
void lodepng_free(void *ptr);

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles, the frame chosen
#define C_CYAN   0x1f  ///< typed text
#define C_ORANGE 0xf0  ///< warnings
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< pressed buttons, the selected row
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip
#define C_CHK1   0x49  ///< see-through: the checks
#define C_CHK2   0x6d  ///< and the others
#define C_GRID   0x00  ///< the grid between pixels
#define C_GRID8  0x92  ///< every eighth line

/// Layout.
#define TOP_H   24     ///< the title strip
#define CAN_X   12     ///< where the sprite's magnified pixels may go
#define CAN_Y   35     ///< its top
#define CAN_S   320    ///< its width and height
#define RP_X    344    ///< the right panel
#define PAL_X   352    ///< the palette
#define PAL_Y   36     ///< its top
#define SW      14     ///< a colour's square
#define COL_X   584    ///< the left and right colours
#define INFO_Y  264    ///< the line about the colour or pixel pointed at
#define TOOL_Y  284    ///< the tools
#define BOT_Y   362    ///< the frames and the preview
#define THUMB_X 28     ///< the first thumbnail
#define THUMB_Y 366    ///< their top
#define THUMB_N 11     ///< thumbnails shown
#define PREV_X  504    ///< the preview panel
#define FILE_Y  444    ///< the file buttons

/// Tools.
enum { T_PENCIL, T_LINE, T_RECT, T_BOX, T_FILL, T_OVAL, T_DISC, T_PICK, T_COUNT };

/// What a click hits.
enum {
    HIT_CANVAS = 1, HIT_PAL, HIT_NONE_COL, HIT_SWAP, HIT_COLBOX, HIT_TOOL, HIT_OP, HIT_THUMB, HIT_SCROLL,
    HIT_FRAMEOP, HIT_FPS, HIT_PLAY, HIT_FILE, HIT_PREVIEW,
    HIT_ASK, HIT_ROW, HIT_FD_OK, HIT_FD_CANCEL, HIT_FD_UP, HIT_SZ, HIT_SZ_OK, HIT_SZ_CANCEL, HIT_SWALLOW,
};

/// The frame tools (HIT_OP).
enum { OP_FLIPH, OP_FLIPV, OP_ROTATE, OP_CLEAR };
/// The frame list's buttons (HIT_FRAMEOP).
enum { FO_ADD, FO_COPY, FO_DELETE, FO_LEFT, FO_RIGHT };
/// The file buttons (HIT_FILE).
enum { FB_NEW, FB_LOAD, FB_SAVE, FB_SAVEAS, FB_PNG, FB_SIZE, FB_UNDO, FB_REDO, FB_HELP, FB_QUIT, FB_COUNT };

/// Things to do, after a question or a save.
enum { ACT_NONE, ACT_NEW, ACT_LOAD, ACT_QUIT, ACT_DELETE, ACT_SAVE_THEN, ACT_WRITE };

/// The boxes over the editor.
enum { DLG_NONE, DLG_ASK, DLG_FILE, DLG_SIZE, DLG_HELP };
/// What the file box is for.
enum { FD_LOAD, FD_SAVE, FD_PNG };

/// A place to click.
typedef struct {
    int16_t x,  ///< left
        y,      ///< top
        w,      ///< width
        h,      ///< height
        id,     ///< HIT_*
        arg;    ///< which
} hit_t;

/// A frame: colours and, a byte a pixel, 1 where drawn (rows of the sprite's width).
typedef struct {
    uint8_t px[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< colours (RGB332)
    uint8_t on[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< 1 drawn, 0 see-through
} frame_t;

/// A step to undo: a frame as it was.
typedef struct {
    int frame;   ///< which frame
    frame_t f;   ///< it, before
} undo_t;

/// An entry in the file box.
typedef struct {
    char name[40];  ///< its name
    bool dir;       ///< a folder
} fd_entry_t;

/// Everything the editor keeps (PSRAM, while it runs).
typedef struct {
    frame_t *fr;           ///< the frames (SPR_MAX_FRAMES of room)
    int nfr;               ///< how many
    int cur;               ///< the one being drawn
    /// the sprite's width and height
    ///@{
    int w, h;
    ///@}
    int col[2];            ///< the left and right colours (0-255, or NONE)
    int tool;              ///< T_*
    bool grid;             ///< lines between the pixels
    char path[160];        ///< the file, or ""
    bool modified;         ///< changed since saved
    undo_t undo[MAX_UNDO]; ///< steps to undo, oldest first
    int nundo;             ///< their count
    undo_t redo[MAX_UNDO]; ///< and to redo
    int nredo;             ///< their count
    // Drawing with the mouse.
    int drag;              ///< the button drawing (0: none)
    /// where the line or shape began, and the pencil's last pixel
    ///@{
    int ax, ay, lx, ly;
    ///@}
    frame_t before;        ///< the frame before the shape being dragged
    int16_t stack[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< the fill's pixels to look at
    // The view.
    int cell;              ///< a sprite pixel's size on the screen
    /// where the sprite's top-left pixel is on the screen
    ///@{
    int ox, oy;
    ///@}
    uint16_t shown[SPR_MAX_SIZE * SPR_MAX_SIZE];  ///< what each magnified pixel shows (0xffff: not drawn)
    int thumb_top;         ///< the first frame in the thumbnails
    bool playing;          ///< the preview animates
    int fps;               ///< at this speed
    int anim;              ///< the frame it shows
    uint32_t next_anim;    ///< when it steps on
    bool thumbs_dirty;     ///< draw the thumbnail and preview again soon
    uint32_t next_thumbs;  ///< not before then
    /// the pixel pointed at (-1: none), and the colour (-1: none)
    ///@{
    int hx, hy, hcol;
    ///@}
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
    int target_mode;       ///< FD_SAVE or FD_PNG
    /// the size box's width and height
    ///@{
    int new_w, new_h;
    ///@}
    char msg[96];          ///< a message for the title strip
    uint32_t msg_until;    ///< until when
    uint8_t *g;            ///< the off-screen picture (PSRAM)
    uint8_t *t;            ///< where drawing goes: g, or the screen itself for small changes
    hit_t hits[160];       ///< places to click
    int nhits;             ///< their count
    bool collect;          ///< drawing the whole picture: places to click are kept
    bool dirty;            ///< draw everything again
    bool quit;             ///< leaving
} ed_t;

static ed_t *F;  ///< the state, while the editor or a command runs

static const char *const tool_names[T_COUNT] = {"Pencil", "Line", "Rect", "Box", "Fill", "Oval", "Disc", "Picker"};
static const char tool_keys[T_COUNT] = {'P', 'L', 'R', 'B', 'F', 'O', 'D', 'I'};

/// Milliseconds since boot.
/// @return them
static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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

/// A colour's square: the colour, or checks for none.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c 0-255, or NONE
static void swatch(int x, int y, int w, int h, int c) {
    if (c != NONE) {
        fill(x, y, w, h, (uint8_t)c);
        return;
    }
    for (int r = 0; r < h; r += 4)
        for (int i = 0; i < w; i += 4)
            fill(x + i, y + r, w - i < 4 ? w - i : 4, h - r < 4 ? h - r : 4, ((r + i) / 4) & 1 ? C_CHK2 : C_CHK1);
}

/// The colour a pixel of a frame has.
/// @param f the frame
/// @param k the pixel (y * width + x)
/// @return 0-255, or NONE
static int px_col(const frame_t *f, int k) {
    return f->on[k] ? f->px[k] : NONE;
}

/// A frame at its real size, see-through pixels left alone.
/// @param f the frame
/// @param x left
/// @param y top
/// @param scale how big a pixel is
static void sprite_at(const frame_t *f, int x, int y, int scale) {
    for (int r = 0; r < F->h; ++r)
        for (int i = 0; i < F->w; ++i)
            if (f->on[r * F->w + i])
                fill(x + i * scale, y + r * scale, scale, scale, f->px[r * F->w + i]);
}

// ---- The sprite, magnified ---------------------------------------------------------------------

/// Works out the magnification (the biggest that fits) and where the sprite goes.
static void layout(void) {
    int big = F->w > F->h ? F->w : F->h;
    F->cell = CAN_S / big;
    F->ox = CAN_X + (CAN_S - F->w * F->cell) / 2;
    F->oy = CAN_Y + (CAN_S - F->h * F->cell) / 2;
}

/// Draws one magnified pixel.
/// @param x the sprite pixel's x
/// @param y its y
static void draw_cell(int x, int y) {
    int k = y * F->w + x, c = px_col(&F->fr[F->cur], k), s = F->cell;
    int cx = F->ox + x * s, cy = F->oy + y * s;
    bool grid = F->grid && s >= 6;
    int in = grid ? s - 1 : s;
    if (c == NONE) {
        int a = in / 2;
        fill(cx, cy, a, a, C_CHK1);
        fill(cx + a, cy, in - a, a, C_CHK2);
        fill(cx, cy + a, a, in - a, C_CHK2);
        fill(cx + a, cy + a, in - a, in - a, C_CHK1);
    } else {
        fill(cx, cy, in, in, (uint8_t)c);
    }
    if (grid) {
        fill(cx + in, cy, 1, s, (x & 7) == 7 && x < F->w - 1 ? C_GRID8 : C_GRID);
        fill(cx, cy + in, s, 1, (y & 7) == 7 && y < F->h - 1 ? C_GRID8 : C_GRID);
    }
    F->shown[k] = (uint16_t)c;
}

/// Draws the magnified pixels that changed since they were drawn.
static void canvas_refresh(void) {
    for (int y = 0; y < F->h; ++y)
        for (int x = 0; x < F->w; ++x) {
            int k = y * F->w + x;
            if (F->shown[k] != (uint16_t)px_col(&F->fr[F->cur], k))
                draw_cell(x, y);
        }
}

/// Draws the canvas panel and every magnified pixel.
static void draw_canvas(void) {
    bevel(4, 28, 336, 330, true, C_FACE);
    bevel(10, 33, 324, 324, false, C_FIELD);
    hit(CAN_X, CAN_Y, CAN_S, CAN_S, HIT_CANVAS, 0);
    memset(F->shown, 0xff, sizeof F->shown);
    canvas_refresh();
}

// ---- The right panel: colours and tools ---------------------------------------------------------

/// The line about the pixel or colour pointed at (or the left colour).
static void draw_info(void) {
    fill(PAL_X, INFO_Y, 280, 16, C_FACE);
    int c = F->hcol >= 0 ? F->hcol : F->col[0];
    int x = PAL_X;
    if (F->hx >= 0)
        x = textf(x, INFO_Y, C_LABEL, C_FACE, "%2d,%-2d ", F->hx, F->hy);
    if (c == NONE)
        text(x, INFO_Y, "none (see-through)", C_DIM, C_FACE);
    else
        textf(x, INFO_Y, C_TEXT, C_FACE, "&H%02X %3d  R%d G%d B%d", c, c, c >> 5, c >> 2 & 7, c & 3);
}

/// Draws the palette, the left and right colours, the tools and the frame tools.
static void draw_right(void) {
    bevel(RP_X, 28, 292, 330, true, C_FACE);
    // All 256 colours, in order: &H00-&H0F along the top row.
    bevel(PAL_X - 2, PAL_Y - 2, 16 * SW + 4, 16 * SW + 4, false, C_FIELD);
    for (int c = 0; c < 256; ++c)
        fill(PAL_X + (c & 15) * SW, PAL_Y + (c >> 4) * SW, SW, SW, (uint8_t)c);
    for (int b = 1; b >= 0; --b) {  // the left colour's mark on top
        int c = F->col[b];
        if (c != NONE) {
            int x = PAL_X + (c & 15) * SW, y = PAL_Y + (c >> 4) * SW;
            frame_box(x, y, SW, SW, C_BG);
            frame_box(x + 1, y + 1, SW - 2, SW - 2, b ? C_YELLOW : C_TEXT);
        }
    }
    hit(PAL_X, PAL_Y, 16 * SW, 16 * SW, HIT_PAL, 0);
    // The left and right colours.
    static const char *const side[2] = {"Left", "Right"};
    for (int b = 0; b < 2; ++b) {
        int y = PAL_Y + b * 54;
        text(COL_X, y, side[b], C_LABEL, C_FACE);
        bevel(COL_X, y + 18, 46, 30, false, C_FIELD);
        swatch(COL_X + 2, y + 20, 42, 26, F->col[b]);
        hit(COL_X, y + 18, 46, 30, HIT_COLBOX, b);
    }
    button(COL_X, PAL_Y + 108, 46, 22, "None", false, HIT_NONE_COL, 0);
    button(COL_X, PAL_Y + 134, 46, 22, "Swap", false, HIT_SWAP, 0);
    draw_info();
    // The tools, then the frame tools.
    for (int i = 0; i < T_COUNT; ++i)
        button(PAL_X + (i & 3) * 70, TOOL_Y + (i >> 2) * 24, 66, 22, tool_names[i], F->tool == i, HIT_TOOL, i);
    static const char *const ops[4] = {"Flip H", "Flip V", "Rotate", "Clear"};
    for (int i = 0; i < 4; ++i)
        button(PAL_X + i * 70, TOOL_Y + 50, 66, 22, ops[i], false, HIT_OP, i);
}

// ---- The frames and the preview -----------------------------------------------------------------

/// Draws one frame's thumbnail (if it is among those shown).
/// @param i the frame
static void draw_thumb(int i) {
    int slot = i - F->thumb_top;
    if (slot < 0 || slot >= THUMB_N)
        return;
    int x = THUMB_X + slot * 40, y = THUMB_Y;
    if (i >= F->nfr) {
        fill(x, y, 38, 40, C_FACE);
        return;
    }
    bool cur = i == F->cur;
    fill(x, y, 38, 38, cur ? C_YELLOW : C_DARK);
    fill(x + 1, y + 1, 36, 36, C_FIELD);
    sprite_at(&F->fr[i], x + 1 + (36 - F->w) / 2, y + 1 + (36 - F->h) / 2, 1);
    fill(x, y + 38, 38, 2, C_FACE);
}

/// Draws the preview: the frame (or the animation's) at twice and at its real size.
static void draw_preview_boxes(void) {
    const frame_t *f = &F->fr[F->playing ? F->anim : F->cur];
    bevel(PREV_X, THUMB_Y, 68, 68, false, C_FIELD);
    sprite_at(f, PREV_X + 2 + (64 - 2 * F->w) / 2, THUMB_Y + 2 + (64 - 2 * F->h) / 2, 2);
    bevel(PREV_X + 74, THUMB_Y, 36, 36, false, C_FIELD);
    sprite_at(f, PREV_X + 76 + (32 - F->w) / 2, THUMB_Y + 2 + (32 - F->h) / 2, 1);
}

/// Draws the frames panel and the preview panel.
static void draw_bottom(void) {
    bevel(4, BOT_Y, 494, 78, true, C_FACE);
    button(8, THUMB_Y, 16, 38, "\x11", false, HIT_SCROLL, -1);
    button(THUMB_X + THUMB_N * 40, THUMB_Y, 16, 38, "\x10", false, HIT_SCROLL, 1);
    if (F->cur < F->thumb_top)
        F->thumb_top = F->cur;
    if (F->cur >= F->thumb_top + THUMB_N)
        F->thumb_top = F->cur - THUMB_N + 1;
    for (int s = 0; s < THUMB_N; ++s) {
        draw_thumb(F->thumb_top + s);
        if (F->thumb_top + s < F->nfr)
            hit(THUMB_X + s * 40, THUMB_Y, 38, 38, HIT_THUMB, F->thumb_top + s);
    }
    static const struct { const char *label; int w; } fo[] = {
        {"Add", 44}, {"Copy", 48}, {"Delete", 60}, {"\x1b Move", 60}, {"Move \x1a", 60}};
    int x = 8;
    for (int i = 0; i < 5; ++i) {
        button(x, 412, fo[i].w, 22, fo[i].label, false, HIT_FRAMEOP, i);
        x += fo[i].w + 4;
    }
    textf(x + 4, 415, C_LABEL, C_FACE, "%d/%d", F->cur + 1, F->nfr);
    // Playing, and how fast.
    button(352, 412, 48, 22, F->playing ? "Stop" : "Play", F->playing, HIT_PLAY, 0);
    button(404, 412, 16, 22, "-", false, HIT_FPS, -1);
    textf(422, 415, C_TEXT, C_FACE, "%2d", F->fps);
    button(440, 412, 16, 22, "+", false, HIT_FPS, 1);
    text(460, 415, "fps", C_LABEL, C_FACE);
    // The preview.
    bevel(PREV_X - 4, BOT_Y, 136, 78, true, C_FACE);
    draw_preview_boxes();
    hit(PREV_X, THUMB_Y, 110, 68, HIT_PREVIEW, 0);
}

/// Draws the file buttons.
static void draw_files(void) {
    static const char *const fb[FB_COUNT] = {"New", "Load", "Save", "Save as", "PNG", "Size", "Undo", "Redo",
                                             "Help", "Quit"};
    for (int i = 0; i < FB_COUNT; ++i) {
        bool off = (i == FB_UNDO && !F->nundo) || (i == FB_REDO && !F->nredo);
        button(4 + i * 63, FILE_Y, 60, 26, fb[i], false, HIT_FILE, i);
        if (off)
            text(4 + i * 63 + (60 - (int)strlen(fb[i]) * 8) / 2, FILE_Y + 5, fb[i], C_DIM, C_FACE);
    }
}

/// The file's name without its folder.
/// @return it, or "untitled"
static const char *file_name(void) {
    if (!F->path[0])
        return "untitled";
    const char *s = strrchr(F->path, '/');
    return s ? s + 1 : F->path;
}

/// Draws the title strip, with the message or the file, size and frame.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    text(8, 4, "Sprite Editor", C_YELLOW, C_STRIP);
    if (F->msg[0] && now_ms() < F->msg_until)
        textf(128, 4, C_TEXT, C_STRIP, "%.62s", F->msg);
    else
        textf(128, 4, C_LABEL, C_STRIP, "%.24s%s  %dx%d  frame %d of %d  %s  F1 keys", file_name(),
              F->modified ? "*" : "", F->w, F->h, F->cur + 1, F->nfr, tool_names[F->tool]);
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
    static const char *const titles[3] = {"Load a sprite (.SPR or .PNG)", "Save the sprite as", "Export a PNG"};
    box(x, y, w, h, titles[F->fd_mode]);
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

/// Draws the size box.
static void draw_size_box(void) {
    int w = 460, h = 150, x = (W - w) / 2, y = 150;
    box(x, y, w, h, "Sprite size (every frame keeps its top left)");
    static const char *const label[2] = {"Width", "Height"};
    static const int presets[4] = {8, 16, 24, 32};
    for (int d = 0; d < 2; ++d) {
        int ry = y + 40 + d * 30, v = d ? F->new_h : F->new_w;
        text(x + 16, ry + 3, label[d], C_LABEL, C_FACE);
        button(x + 80, ry, 22, 22, "-", false, HIT_SZ, d * 16 + 0);
        textf(x + 108, ry + 3, C_TEXT, C_FACE, "%2d", v);
        button(x + 130, ry, 22, 22, "+", false, HIT_SZ, d * 16 + 1);
        for (int p = 0; p < 4; ++p) {
            char s[4];
            snprintf(s, sizeof s, "%d", presets[p]);
            button(x + 170 + p * 50, ry, 46, 22, s, v == presets[p], HIT_SZ, d * 16 + 2 + p);
        }
    }
    button(x + 16, y + h - 36, 120, 26, "OK (Enter)", true, HIT_SZ_OK, 0);
    button(x + 144, y + h - 36, 120, 26, "Cancel (Esc)", false, HIT_SZ_CANCEL, 0);
}

/// Draws the keys.
static void draw_help(void) {
    int w = 600, h = 372, x = (W - w) / 2, y = 50;
    box(x, y, w, h, "Keys (any key or click closes this)");
    static const char *const lines[] = {
        "Left button: draw in the left colour; right button: the right",
        "colour; middle button: pick the colour under the pointer.",
        "",
        "P pencil  L line  R rectangle  B box  F fill  O oval  D disc",
        "I picker  N left colour none  X swap the colours  G grid",
        "H flip across  V flip up and down  T rotate  C clear the frame",
        "Arrows      shift the frame a pixel (round the edge)",
        "PgUp/PgDn   the frame before / after (the wheel too)",
        "Ins         a copy of the frame after it   Del  delete it",
        "Space       play / stop the animation   + -  its speed",
        "U, Ctrl-Z   undo   Ctrl-Y  redo (drawing and the frame tools)",
        "Ctrl-S, F2  save   F3 load   Ctrl-N new   E export a PNG",
        "Esc         quit (asks if there are changes to save)",
        "",
        "Files are .SPR; BASIC loads one with SPRITE LOAD \"file\", s:",
        "its frames become shapes s, s+1, ... for SPRITE n, x, y, s.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof *lines; ++i)
        text(x + 16, y + 40 + (int)i * 20, lines[i], C_TEXT, C_FACE);
}

/// Draws everything into the off-screen picture.
static void draw(void) {
    F->t = F->g;
    F->collect = true;
    F->nhits = 0;
    fill(0, 0, W, H, C_BG);
    draw_top();
    draw_canvas();
    draw_right();
    draw_bottom();
    draw_files();
    switch (F->dlg) {
    case DLG_ASK: draw_ask(); break;
    case DLG_FILE: draw_file_box(); break;
    case DLG_SIZE: draw_size_box(); break;
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

// ---- Undo ----------------------------------------------------------------------------------------

/// Keeps the frame as it is now, before it is changed.
static void undo_push(void) {
    if (F->nundo == MAX_UNDO) {
        memmove(&F->undo[0], &F->undo[1], (MAX_UNDO - 1) * sizeof *F->undo);
        --F->nundo;
    }
    F->undo[F->nundo].frame = F->cur;
    F->undo[F->nundo++].f = F->fr[F->cur];
}

/// After a change: forgets the step if nothing changed after all, else the redo steps.
static void undo_commit(void) {
    if (!F->nundo)
        return;
    undo_t *u = &F->undo[F->nundo - 1];
    if (!memcmp(&u->f, &F->fr[u->frame], sizeof u->f)) {
        --F->nundo;
        return;
    }
    F->nredo = 0;
    F->modified = true;
    F->thumbs_dirty = true;
}

/// Forgets every step (the frames have been added, deleted, moved or resized).
static void undo_forget(void) {
    F->nundo = F->nredo = 0;
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
    undo_t *u = &from[--*nfrom];
    if (u->frame >= F->nfr)
        return;
    if (*nto == MAX_UNDO) {
        memmove(&to[0], &to[1], (MAX_UNDO - 1) * sizeof *to);
        --*nto;
    }
    to[*nto].frame = u->frame;
    to[(*nto)++].f = F->fr[u->frame];
    F->fr[u->frame] = u->f;
    F->cur = u->frame;
    F->modified = true;
    F->dirty = true;
}

// ---- Drawing on the sprite ------------------------------------------------------------------------

/// Sets a pixel of the frame being drawn (outside it: nothing).
/// @param x its x
/// @param y its y
/// @param c 0-255, or NONE
static void set_px(int x, int y, int c) {
    if (x < 0 || y < 0 || x >= F->w || y >= F->h)
        return;
    frame_t *f = &F->fr[F->cur];
    int k = y * F->w + x;
    f->on[k] = c != NONE;
    f->px[k] = c == NONE ? 0 : (uint8_t)c;
}

/// A line (Bresenham's).
/// @param x0 one end's x
/// @param y0 its y
/// @param x1 the other's x
/// @param y1 its y
/// @param c the colour
static void line(int x0, int y0, int x1, int y1, int c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        set_px(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
            err += dy, x0 += sx;
        if (e2 <= dx)
            err += dx, y0 += sy;
    }
}

/// A row of pixels.
/// @param x0 the left
/// @param x1 the right
/// @param y the row
/// @param c the colour
static void hline(int x0, int x1, int y, int c) {
    for (int x = x0; x <= x1; ++x)
        set_px(x, y, c);
}

/// A rectangle, filled or not, between two corners.
/// @param x0 a corner's x
/// @param y0 its y
/// @param x1 the other corner's x
/// @param y1 its y
/// @param c the colour
/// @param filled filled
static void rect(int x0, int y0, int x1, int y1, int c, bool filled) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y) {
        if (filled || y == y0 || y == y1)
            hline(x0, x1, y, c);
        else
            set_px(x0, y, c), set_px(x1, y, c);
    }
}

/// An ellipse that fits the rectangle between two corners, filled or not
/// (Zingl's Bresenham ellipse, which handles even sizes too).
/// @param x0 a corner's x
/// @param y0 its y
/// @param x1 the other corner's x
/// @param y1 its y
/// @param c the colour
/// @param filled filled
static void oval(int x0, int y0, int x1, int y1, int c, bool filled) {
    long a = abs(x1 - x0), b = abs(y1 - y0), b1 = b & 1;
    long dx = 4 * (1 - a) * b * b, dy = 4 * (b1 + 1) * a * a, err = dx + dy + b1 * a * a, e2;
    if (x0 > x1) { x0 = x1; x1 += (int)a; }
    if (y0 > y1) y0 = y1;
    y0 += (int)((b + 1) / 2);
    y1 = y0 - (int)b1;
    a *= 8 * a;
    b1 = 8 * b * b;
    do {
        if (filled) {
            hline(x0, x1, y0, c);
            hline(x0, x1, y1, c);
        } else {
            set_px(x1, y0, c);
            set_px(x0, y0, c);
            set_px(x0, y1, c);
            set_px(x1, y1, c);
        }
        e2 = 2 * err;
        if (e2 <= dy) { y0++; y1--; err += dy += a; }
        if (e2 >= dx || 2 * err > dy) { x0++; x1--; err += dx += b1; }
    } while (x0 <= x1);
    while (y0 - y1 <= b) {  // the tips of a very flat one
        hline(x0 - 1, x1 + 1, y0++, c);
        hline(x0 - 1, x1 + 1, y1--, c);
    }
}

/// Fills the area of the same colour as (x, y), its four neighbours at a time.
/// @param x where
/// @param y where
/// @param c the colour
static void flood(int x, int y, int c) {
    frame_t *f = &F->fr[F->cur];
    int was = px_col(f, y * F->w + x);
    if (was == c)
        return;
    int n = 0;
    F->stack[n++] = (int16_t)(y * F->w + x);
    set_px(x, y, c);
    while (n) {
        int k = F->stack[--n], px = k % F->w, py = k / F->w;
        static const int8_t d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int i = 0; i < 4; ++i) {
            int nx = px + d[i][0], ny = py + d[i][1];
            if (nx < 0 || ny < 0 || nx >= F->w || ny >= F->h || px_col(f, ny * F->w + nx) != was)
                continue;
            set_px(nx, ny, c);  // marked as it goes on the stack: each pixel goes on once
            F->stack[n++] = (int16_t)(ny * F->w + nx);
        }
    }
}

/// Draws the line or shape being dragged from the anchor to (x, y), over the frame as it was.
/// @param x the pointer's pixel x
/// @param y its y
static void shape_to(int x, int y) {
    int c = F->col[F->drag == MOUSE_RIGHT];
    F->fr[F->cur] = F->before;
    switch (F->tool) {
    case T_LINE: line(F->ax, F->ay, x, y, c); break;
    case T_RECT: rect(F->ax, F->ay, x, y, c, false); break;
    case T_BOX: rect(F->ax, F->ay, x, y, c, true); break;
    case T_OVAL: oval(F->ax, F->ay, x, y, c, false); break;
    case T_DISC: oval(F->ax, F->ay, x, y, c, true); break;
    }
}

/// The sprite pixel under a screen point (may be outside the sprite).
/// @param sx the screen x
/// @param sy its y
/// @param x set to the pixel's x
/// @param y set to its y
/// @return it is inside the sprite
static bool cell_at(int sx, int sy, int *x, int *y) {
    *x = (sx - F->ox + F->cell * 64) / F->cell - 64;  // rounding down left of the sprite too
    *y = (sy - F->oy + F->cell * 64) / F->cell - 64;
    return *x >= 0 && *y >= 0 && *x < F->w && *y < F->h;
}

/// Picks the colour of a pixel as the left or right colour.
/// @param x the pixel's x
/// @param y its y
/// @param side 0 left, 1 right
static void pick(int x, int y, int side) {
    if (x < 0 || y < 0 || x >= F->w || y >= F->h)
        return;
    F->col[side] = px_col(&F->fr[F->cur], y * F->w + x);
    F->dirty = true;
}

/// A button went down on the sprite: starts drawing.
/// @param x the pixel's x
/// @param y its y
/// @param btn MOUSE_LEFT, _RIGHT or _MIDDLE
static void stroke_start(int x, int y, int btn) {
    if (btn == MOUSE_MIDDLE || F->tool == T_PICK) {
        pick(x, y, btn == MOUSE_RIGHT);
        F->drag = btn == MOUSE_MIDDLE ? 0 : btn;
        return;
    }
    int c = F->col[btn == MOUSE_RIGHT];
    F->drag = btn;
    undo_push();
    switch (F->tool) {
    case T_PENCIL:
        set_px(x, y, c);
        F->lx = x;
        F->ly = y;
        break;
    case T_FILL:
        flood(x, y, c);
        F->drag = 0;
        undo_commit();
        F->dirty = true;  // the undo button
        break;
    default:
        F->before = F->fr[F->cur];
        F->ax = x;
        F->ay = y;
        shape_to(x, y);
        break;
    }
    canvas_refresh();
}

/// The pointer moved while drawing.
/// @param x the pixel's x (may be outside the sprite)
/// @param y its y
static void stroke_move(int x, int y) {
    if (F->tool == T_PICK) {
        pick(x, y, F->drag == MOUSE_RIGHT);
        return;
    }
    if (F->tool == T_PENCIL) {
        if (x == F->lx && y == F->ly)
            return;
        line(F->lx, F->ly, x, y, F->col[F->drag == MOUSE_RIGHT]);
        F->lx = x;
        F->ly = y;
    } else {
        shape_to(x, y);
    }
    canvas_refresh();
    F->thumbs_dirty = true;
}

/// The button came up: the drawing is done.
static void stroke_end(void) {
    bool drew = F->drag && F->tool != T_PICK;
    F->drag = 0;
    if (drew) {
        undo_commit();
        F->dirty = true;  // the title's "*" and the undo button
    }
}

// ---- The frame tools -------------------------------------------------------------------------

/// Flips, rotates or clears the frame.
/// @param op OP_*
static void frame_op(int op) {
    if (op == OP_ROTATE && F->w != F->h) {
        message("Rotate needs a square sprite (Size makes it one)");
        return;
    }
    undo_push();
    frame_t *f = &F->fr[F->cur], *was = &F->before;
    *was = *f;
    int w = F->w, h = F->h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int from = op == OP_FLIPH   ? y * w + (w - 1 - x)
                       : op == OP_FLIPV ? (h - 1 - y) * w + x
                                        : (w - 1 - x) * w + y;  // rotate a quarter turn clockwise
            f->px[y * w + x] = op == OP_CLEAR ? 0 : was->px[from];
            f->on[y * w + x] = op == OP_CLEAR ? 0 : was->on[from];
        }
    undo_commit();
    F->dirty = true;
}

/// Shifts the frame a pixel, round the edges.
/// @param dx -1, 0 or 1
/// @param dy -1, 0 or 1
static void frame_shift(int dx, int dy) {
    undo_push();
    frame_t *f = &F->fr[F->cur], *was = &F->before;
    *was = *f;
    for (int y = 0; y < F->h; ++y)
        for (int x = 0; x < F->w; ++x) {
            int from = ((y - dy + F->h) % F->h) * F->w + (x - dx + F->w) % F->w;
            f->px[y * F->w + x] = was->px[from];
            f->on[y * F->w + x] = was->on[from];
        }
    undo_commit();
    F->dirty = true;
}

/// Adds, copies, deletes or moves a frame.
/// @param op FO_*
static void frames_op(int op) {
    int c = F->cur;
    switch (op) {
    case FO_ADD:
    case FO_COPY:
        if (F->nfr == SPR_MAX_FRAMES) {
            message("%d frames is the most", SPR_MAX_FRAMES);
            return;
        }
        memmove(&F->fr[c + 2], &F->fr[c + 1], (F->nfr - c - 1) * sizeof *F->fr);
        if (op == FO_ADD)
            memset(&F->fr[c + 1], 0, sizeof *F->fr);
        else
            F->fr[c + 1] = F->fr[c];
        ++F->nfr;
        F->cur = c + 1;
        break;
    case FO_DELETE:
        if (F->nfr == 1) {
            memset(&F->fr[0], 0, sizeof *F->fr);
        } else {
            memmove(&F->fr[c], &F->fr[c + 1], (F->nfr - c - 1) * sizeof *F->fr);
            --F->nfr;
            if (F->cur >= F->nfr)
                F->cur = F->nfr - 1;
        }
        break;
    case FO_LEFT:
    case FO_RIGHT: {
        int o = op == FO_LEFT ? c - 1 : c + 1;
        if (o < 0 || o >= F->nfr)
            return;
        F->before = F->fr[c];
        F->fr[c] = F->fr[o];
        F->fr[o] = F->before;
        F->cur = o;
        break;
    }
    }
    if (F->anim >= F->nfr)
        F->anim = 0;
    undo_forget();
    F->modified = true;
    F->dirty = true;
}

/// Changes the sprite's size, keeping each frame's top left.
/// @param w the new width
/// @param h the new height
static void resize(int w, int h) {
    if (w == F->w && h == F->h)
        return;
    frame_t *t = psram_malloc(sizeof *t);
    if (!t) {
        message("Not enough memory");
        return;
    }
    for (int i = 0; i < F->nfr; ++i) {
        memset(t, 0, sizeof *t);
        for (int y = 0; y < h && y < F->h; ++y)
            for (int x = 0; x < w && x < F->w; ++x) {
                t->px[y * w + x] = F->fr[i].px[y * F->w + x];
                t->on[y * w + x] = F->fr[i].on[y * F->w + x];
            }
        F->fr[i] = *t;
    }
    psram_free(t);
    F->w = w;
    F->h = h;
    layout();
    undo_forget();
    F->modified = true;
    F->dirty = true;
}

/// Goes to a frame.
/// @param i the frame (kept within those there are)
static void go_frame(int i) {
    if (i < 0)
        i = 0;
    if (i >= F->nfr)
        i = F->nfr - 1;
    if (i != F->cur) {
        F->cur = i;
        F->dirty = true;
    }
}

// ---- Files ----------------------------------------------------------------------------------------

/// For spr_save: a frame.
/// @param ctx unused
/// @param i the frame
/// @param px its colours
/// @param on where it is drawn
static void get_frame(void *ctx, int i, uint8_t *px, uint8_t *on) {
    (void)ctx;
    memcpy(px, F->fr[i].px, F->w * F->h);
    memcpy(on, F->fr[i].on, F->w * F->h);
}

/// For spr_load: a frame, into the frames being loaded.
/// @param ctx the frames
/// @param i the frame
/// @param px its colours
/// @param on where it is drawn
/// @return true (go on)
static bool put_frame(void *ctx, int i, const uint8_t *px, const uint8_t *on) {
    frame_t *fr = ctx;
    memset(&fr[i], 0, sizeof *fr);
    memcpy(fr[i].px, px, sizeof fr[i].px);
    memcpy(fr[i].on, on, sizeof fr[i].on);
    return true;
}

/// Whether a name ends in an extension.
/// @param name the name
/// @param ext ".SPR" or the like
/// @return it does (any case)
static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && !strcasecmp(name + n - e, ext);
}

/// Reads a PNG into frames: if it is as tall as the sprite and a whole number
/// of its widths across, frames of the sprite's size; else, if it is up to 32
/// tall and a whole number of its heights across, square frames; else one
/// frame of its top left (up to 32x32). Colours are rounded to RGB332;
/// pixels less than half opaque are see-through.
/// @param path the file
/// @param fr where the frames go
/// @param info set to the size and frame count
/// @param err set to why, on failure
/// @return it was read
static bool load_png(const char *path, frame_t *fr, spr_info_t *info, const char **err) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        *err = "can't open it";
        return false;
    }
    UINT size = (UINT)f_size(&f), got;
    uint8_t *buf = size <= PNG_MAX ? psram_malloc(size) : NULL;
    if (!buf) {
        f_close(&f);
        *err = size > PNG_MAX ? "it is too big (1 MB at most)" : "not enough memory";
        return false;
    }
    bool ok = f_read(&f, buf, size, &got) == FR_OK && got == size;
    f_close(&f);
    unsigned char *rgba = NULL;
    unsigned iw = 0, ih = 0;
    if (!ok || lodepng_decode32(&rgba, &iw, &ih, buf, size)) {
        psram_free(buf);
        *err = "not a PNG it can read";
        return false;
    }
    psram_free(buf);
    int fw, fh, n;
    if ((int)ih == F->h && iw % F->w == 0 && iw / F->w <= SPR_MAX_FRAMES) {
        fw = F->w, fh = F->h, n = iw / F->w;
    } else if (ih <= SPR_MAX_SIZE && iw % ih == 0 && iw / ih <= SPR_MAX_FRAMES) {
        fw = fh = ih, n = iw / ih;
    } else {
        fw = iw < SPR_MAX_SIZE ? iw : SPR_MAX_SIZE;
        fh = ih < SPR_MAX_SIZE ? ih : SPR_MAX_SIZE;
        n = 1;
    }
    for (int i = 0; i < n; ++i) {
        memset(&fr[i], 0, sizeof *fr);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                const uint8_t *p = rgba + ((size_t)y * iw + (size_t)i * fw + x) * 4;
                if (p[3] < 128)
                    continue;
                int k = y * fw + x;
                fr[i].px[k] = (uint8_t)(((p[0] * 7 + 127) / 255) << 5 | ((p[1] * 7 + 127) / 255) << 2 |
                                        (p[2] * 3 + 127) / 255);
                fr[i].on[k] = 1;
            }
    }
    lodepng_free(rgba);
    info->w = fw;
    info->h = fh;
    info->frames = n;
    return true;
}

/// Loads a .SPR or .PNG in place of the frames.
/// @param path the file
/// @return it loaded
static bool load(const char *path) {
    frame_t *fr = psram_malloc(SPR_MAX_FRAMES * sizeof *fr);
    if (!fr) {
        message("Not enough memory");
        return false;
    }
    spr_info_t info;
    const char *err = NULL;
    bool png = has_ext(path, ".PNG");
    bool ok = png ? load_png(path, fr, &info, &err) : spr_load(path, &info, put_frame, fr, &err);
    if (!ok) {
        psram_free(fr);
        message("%.40s: %s", path, err ? err : "?");
        return false;
    }
    psram_free(F->fr);
    F->fr = fr;
    F->nfr = info.frames;
    F->w = info.w;
    F->h = info.h;
    F->cur = F->anim = F->thumb_top = 0;
    layout();
    undo_forget();
    if (png) {  // saved as a .SPR of the same name
        snprintf(F->path, sizeof F->path, "%.*s.SPR", (int)strlen(path) - 4, path);
        F->modified = true;
    } else {
        snprintf(F->path, sizeof F->path, "%s", path);
        F->modified = false;
    }
    message("%s: %dx%d, %d frame%s", path, info.w, info.h, info.frames, info.frames == 1 ? "" : "s");
    return true;
}

/// Writes the sprite as a .SPR or a PNG.
/// @param path the file
/// @param mode FD_SAVE or FD_PNG
/// @return it was written
static bool write_file(const char *path, int mode) {
    spr_info_t info = {F->w, F->h, F->nfr};
    const char *err = NULL;
    bool ok = mode == FD_PNG ? spr_save_png(path, &info, get_frame, NULL, &err)
                             : spr_save(path, &info, get_frame, NULL, &err);
    if (!ok) {
        message("%.40s: %s", path, err ? err : "?");
        return false;
    }
    if (mode == FD_SAVE) {
        snprintf(F->path, sizeof F->path, "%s", path);
        F->modified = false;
    }
    message("Saved %s", path);
    return true;
}

/// Starts afresh: one empty frame of the size now.
static void new_sprite(void) {
    memset(F->fr, 0, sizeof *F->fr);
    F->nfr = 1;
    F->cur = F->anim = F->thumb_top = 0;
    F->path[0] = 0;
    F->modified = false;
    undo_forget();
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
    return strcasecmp(x->name, y->name);
}

/// Reads the folder shown into the list (folders, and the files of the kind wanted).
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
        if (fi.fname[0] == '.')
            continue;
        if (!dir && !has_ext(fi.fname, F->fd_mode == FD_PNG ? ".PNG" : ".SPR") &&
            !(F->fd_mode == FD_LOAD && has_ext(fi.fname, ".PNG")))
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
    if (mode != FD_LOAD && F->path[0]) {
        const char *n = file_name();
        snprintf(F->fd_name, sizeof F->fd_name, mode == FD_PNG ? "%.*s.PNG" : "%.*s.SPR",
                 has_ext(n, ".SPR") ? (int)strlen(n) - 4 : (int)strlen(n), n);
    }
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
        strncat(p, F->fd_mode == FD_PNG ? ".PNG" : ".SPR", sizeof p - strlen(p) - 1);
    snprintf(F->target, sizeof F->target, "%s", p);
    F->target_mode = F->fd_mode;
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
    case ACT_NEW: new_sprite(); break;
    case ACT_LOAD: fd_open(FD_LOAD); break;
    case ACT_QUIT: F->quit = true; break;
    case ACT_DELETE: frames_op(FO_DELETE); break;
    case ACT_SAVE_THEN:
        if (!F->path[0] || has_ext(F->path, ".PNG")) {
            fd_open(FD_SAVE);  // then F->then, after the save
            return;
        }
        if (write_file(F->path, FD_SAVE))
            act(F->then);
        F->then = ACT_NONE;
        break;
    case ACT_WRITE:
        if (write_file(F->target, F->target_mode) && F->target_mode == FD_SAVE && F->then != ACT_NONE) {
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
    case FB_PNG: fd_open(FD_PNG); break;
    case FB_SIZE:
        F->new_w = F->w;
        F->new_h = F->h;
        F->dlg = DLG_SIZE;
        break;
    case FB_UNDO: undo_step(false); break;
    case FB_REDO: undo_step(true); break;
    case FB_HELP: F->dlg = DLG_HELP; break;
    case FB_QUIT: act_unsaved(ACT_QUIT); break;
    }
    F->dirty = true;
}

/// Asks before deleting the frame (one with nothing drawn goes at once).
static void ask_delete(void) {
    const frame_t *f = &F->fr[F->cur];
    bool empty = true;
    for (int k = 0; k < F->w * F->h; ++k)
        if (f->on[k])
            empty = false;
    if (empty || F->nfr == 1) {
        frames_op(FO_DELETE);
        return;
    }
    char q[64];
    snprintf(q, sizeof q, "Delete frame %d? (Undo can't bring it back)", F->cur + 1);
    ask(q, "Delete", ACT_DELETE, "Cancel", ACT_NONE, NULL, ACT_NONE);
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

/// A key while the size box is up.
/// @param k the key
static void size_key(int k) {
    if (k == '\r' || k == '\n') {
        F->dlg = DLG_NONE;
        resize(F->new_w, F->new_h);
    } else if (k == KEY_ESC) {
        F->dlg = DLG_NONE;
    } else if (k == KEY_LEFT || k == KEY_RIGHT) {
        F->new_w += k == KEY_LEFT ? -1 : 1;
    } else if (k == KEY_UP || k == KEY_DOWN) {
        F->new_h += k == KEY_DOWN ? -1 : 1;
    }
    F->new_w = F->new_w < 1 ? 1 : F->new_w > SPR_MAX_SIZE ? SPR_MAX_SIZE : F->new_w;
    F->new_h = F->new_h < 1 ? 1 : F->new_h > SPR_MAX_SIZE ? SPR_MAX_SIZE : F->new_h;
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    F->dirty = true;
    switch (F->dlg) {
    case DLG_HELP: F->dlg = DLG_NONE; return;
    case DLG_FILE: fd_key(k); return;
    case DLG_SIZE: size_key(k); return;
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
    for (int t = 0; t < T_COUNT; ++t)
        if (up == tool_keys[t]) {
            F->tool = t;
            return;
        }
    switch (up) {
    case KEY_LEFT: frame_shift(-1, 0); break;
    case KEY_RIGHT: frame_shift(1, 0); break;
    case KEY_UP: frame_shift(0, -1); break;
    case KEY_DOWN: frame_shift(0, 1); break;
    case KEY_PGUP: case ',': go_frame(F->cur - 1); break;
    case KEY_PGDN: case '.': go_frame(F->cur + 1); break;
    case KEY_HOME: go_frame(0); break;
    case KEY_END: go_frame(F->nfr - 1); break;
    case KEY_INS: frames_op(FO_COPY); break;
    case KEY_DEL: ask_delete(); break;
    case 'H': frame_op(OP_FLIPH); break;
    case 'V': frame_op(OP_FLIPV); break;
    case 'T': frame_op(OP_ROTATE); break;
    case 'C': frame_op(OP_CLEAR); break;
    case 'N': F->col[0] = NONE; break;
    case 'X': { int t = F->col[0]; F->col[0] = F->col[1]; F->col[1] = t; break; }
    case 'G': F->grid = !F->grid; break;
    case ' ': F->playing = !F->playing; F->anim = F->cur; break;
    case '+': case '=': F->fps = F->fps < 30 ? F->fps + 1 : 30; break;
    case '-': F->fps = F->fps > 1 ? F->fps - 1 : 1; break;
    case 'U': case CTRL('z'): undo_step(false); break;
    case CTRL('y'): undo_step(true); break;
    case CTRL('s'): case KEY_F2: file_button(FB_SAVE); break;
    case KEY_F3: case CTRL('o'): file_button(FB_LOAD); break;
    case CTRL('n'): file_button(FB_NEW); break;
    case 'E': file_button(FB_PNG); break;
    case KEY_F1: F->dlg = DLG_HELP; break;
    case KEY_ESC: file_button(FB_QUIT); break;
    }
}

/// Updates the line about what the pointer is over, if that changed.
/// @param e the mouse event
static void hover(const mouse_event_t *e) {
    int hx = -1, hy = -1, hc = -1, x, y;
    if (cell_at(e->x, e->y, &x, &y) && e->x >= CAN_X && e->x < CAN_X + CAN_S && e->y >= CAN_Y &&
        e->y < CAN_Y + CAN_S) {
        hx = x, hy = y, hc = px_col(&F->fr[F->cur], y * F->w + x);
    } else if (e->x >= PAL_X && e->x < PAL_X + 16 * SW && e->y >= PAL_Y && e->y < PAL_Y + 16 * SW) {
        hc = (e->y - PAL_Y) / SW * 16 + (e->x - PAL_X) / SW;
    }
    if (hx == F->hx && hy == F->hy && hc == F->hcol)
        return;
    F->hx = hx, F->hy = hy, F->hcol = hc;
    if (!F->dlg && !F->dirty)
        draw_info();  // straight onto the screen
}

/// A click in the size box.
/// @param arg which value (16 for the height) and button: 0 -, 1 +, 2-5 the presets
static void size_click(int arg) {
    int *v = arg >= 16 ? &F->new_h : &F->new_w, b = arg & 15;
    static const int presets[4] = {8, 16, 24, 32};
    if (b == 0 && *v > 1)
        --*v;
    else if (b == 1 && *v < SPR_MAX_SIZE)
        ++*v;
    else if (b >= 2)
        *v = presets[b - 2];
}

/// Handles a mouse event.
/// @param e the event
static void mouse(const mouse_event_t *e) {
    if (e->type == MOUSE_UP) {
        if (F->drag && e->button == F->drag)
            stroke_end();
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (F->drag) {
            int x, y;
            cell_at(e->x, e->y, &x, &y);
            stroke_move(x, y);
        }
        hover(e);
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (e->type == MOUSE_WHEEL) {
        int d = e->wheel > 0 ? -1 : 1;
        if (F->dlg == DLG_FILE) {
            F->fd_sel += d * 3;
            F->fd_sel = F->fd_sel < 0 ? 0 : F->fd_sel >= F->fd_n ? F->fd_n - 1 : F->fd_sel;
            F->dirty = true;
        } else if (F->dlg || F->drag) {
            return;
        } else if (h && h->id == HIT_FPS) {
            key(d < 0 ? '+' : '-');
        } else if (h && h->id == HIT_PAL && F->col[0] != NONE) {
            F->col[0] = (F->col[0] - d) & 255;
            F->dirty = true;
        } else {
            go_frame(F->cur + d);
        }
        return;
    }
    if (e->type != MOUSE_DOWN || !h)
        return;
    if (F->drag)  // a second button while drawing
        return;
    F->dirty = true;
    int side = e->button == MOUSE_RIGHT;
    switch (h->id) {
    case HIT_CANVAS: {
        int x, y;
        F->dirty = false;  // drawn straight onto the screen
        if (cell_at(e->x, e->y, &x, &y))
            stroke_start(x, y, e->button);
        break;
    }
    case HIT_PAL:
        F->col[side] = (e->y - PAL_Y) / SW * 16 + (e->x - PAL_X) / SW;
        break;
    case HIT_NONE_COL: F->col[side] = NONE; break;
    case HIT_SWAP: key('x'); break;
    case HIT_COLBOX:
        if (e->double_click)
            F->col[h->arg] = NONE;
        break;
    case HIT_TOOL: F->tool = h->arg; break;
    case HIT_OP: frame_op(h->arg); break;
    case HIT_THUMB:
        go_frame(h->arg);
        if (side)
            ask_delete();
        break;
    case HIT_SCROLL:
        F->thumb_top += h->arg * THUMB_N;
        if (F->thumb_top > F->nfr - THUMB_N)
            F->thumb_top = F->nfr - THUMB_N;
        if (F->thumb_top < 0)
            F->thumb_top = 0;
        go_frame(F->cur < F->thumb_top ? F->thumb_top
                 : F->cur >= F->thumb_top + THUMB_N ? F->thumb_top + THUMB_N - 1 : F->cur);
        break;
    case HIT_FRAMEOP:
        if (h->arg == FO_DELETE)
            ask_delete();
        else
            frames_op(h->arg);
        break;
    case HIT_FPS: key(h->arg > 0 ? '+' : '-'); break;
    case HIT_PLAY:
    case HIT_PREVIEW: key(' '); break;
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
    case HIT_SZ: size_click(h->arg); break;
    case HIT_SZ_OK: key('\r'); break;
    case HIT_SZ_CANCEL: F->dlg = DLG_NONE; break;
    case HIT_SWALLOW:
        if (F->dlg == DLG_HELP)
            F->dlg = DLG_NONE;
        break;
    }
}

// ---- The editor ----------------------------------------------------------------------------------

/// Starts the state (PSRAM): one empty 16x16 frame.
/// @return false if there is no memory for it
static bool start(void) {
    F = psram_malloc(sizeof *F);
    if (!F)
        return false;
    memset(F, 0, sizeof *F);
    F->fr = psram_malloc(SPR_MAX_FRAMES * sizeof *F->fr);
    F->fd = psram_malloc(MAX_FILES * sizeof *F->fd);
    if (!F->fr || !F->fd) {
        psram_free(F->fr);
        psram_free(F->fd);
        psram_free(F);
        F = NULL;
        return false;
    }
    memset(F->fr, 0, sizeof *F->fr);
    F->nfr = 1;
    F->w = F->h = 16;
    F->col[0] = 0xff;
    F->col[1] = NONE;
    F->grid = true;
    F->fps = 8;
    F->hx = F->hy = F->hcol = -1;
    F->t = dvi_framebuf;
    layout();
    return true;
}

/// Frees the state.
static void stop(void) {
    psram_free(F->g);
    psram_free(F->fd);
    psram_free(F->fr);
    psram_free(F);
    F = NULL;
}

/// The thumbnail and preview, drawn straight onto the screen.
static void refresh_frames(void) {
    draw_thumb(F->cur);
    draw_preview_boxes();
}

/// The editor.
static void editor(void) {
    F->g = psram_malloc(W * H);
    if (!F->g) {
        printf("sprite: not enough memory\n");
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
        // The animation, and the thumbnail after drawing (at most 10 times a second).
        bool frames = false;
        if (F->playing && (int32_t)(now - F->next_anim) >= 0) {
            F->anim = (F->anim + 1) % F->nfr;
            F->next_anim = now + 1000 / F->fps;
            frames = true;
        }
        if (F->thumbs_dirty && (int32_t)(now - F->next_thumbs) >= 0) {
            F->thumbs_dirty = false;
            F->next_thumbs = now + 100;
            frames = true;
        }
        if (frames && !F->dirty && !F->dlg)
            refresh_frames();
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
    printf("usage: sprite [file.spr]         the editor (spredit too)\n"
           "       sprite info file.spr      its size and frames\n"
           "       sprite png file.spr [out.png]  the frames side by side as a PNG\n");
}

void sprite_command(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
        usage();
        return;
    }
    if (!start()) {
        printf("sprite: not enough memory\n");
        return;
    }
    if (argc > 2 && (!strcmp(argv[1], "info") || !strcmp(argv[1], "png"))) {
        spr_info_t info;
        const char *err = NULL;
        if (!spr_load(argv[2], &info, put_frame, F->fr, &err)) {
            printf("sprite: %s: %s\n", argv[2], err);
        } else if (!strcmp(argv[1], "info")) {
            printf("%s: %dx%d, %d frame%s\n", argv[2], info.w, info.h, info.frames, info.frames == 1 ? "" : "s");
        } else {
            F->w = info.w, F->h = info.h, F->nfr = info.frames;
            char out[200];
            if (argc > 3)
                snprintf(out, sizeof out, "%s", argv[3]);
            else
                snprintf(out, sizeof out, "%.*s.PNG",
                         has_ext(argv[2], ".SPR") ? (int)strlen(argv[2]) - 4 : (int)strlen(argv[2]), argv[2]);
            if (spr_save_png(out, &info, get_frame, NULL, &err))
                printf("%s: %d frame%s of %dx%d\n", out, info.frames, info.frames == 1 ? "" : "s", info.w, info.h);
            else
                printf("sprite: %s: %s\n", out, err);
        }
        stop();
        return;
    }
    if (argc > 1) {
        FILINFO fi;
        if (f_stat(argv[1], &fi) == FR_OK) {
            load(argv[1]);
        } else {
            snprintf(F->path, sizeof F->path, "%s%s", argv[1], strchr(argv[1], '.') ? "" : ".SPR");
            message("%s is new", F->path);
        }
    }
    editor();
    stop();
}
