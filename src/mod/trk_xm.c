/**
 * @file trk_xm.c
 * @brief The tracker with an XM (FastTracker 2) song.
 *
 * the same screen and keys as for MODs, with the XM's own pattern fields
 * (note, instrument, volume column, effect), any number of channels (the
 * view scrolls sideways), key off, patterns of any length, and its
 * instruments. The song plays through xm.c. See trk.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvi.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "trk.h"
#include "xm.h"

// ---- Layout (as the MOD screen's) -------------------------------------------------------------
#define TOP_Y     16
#define TOP_H     144
#define SONG_X    0
#define SONG_W    236
#define ORD_X     236
#define ORD_W     112
#define INS_X     348
#define INS_W     292
#define METER_Y   160
#define METER_H   48
#define TRANS_Y   208
#define HEAD_Y    232
#define PAT_Y     248
#define PAT_ROWS  15
#define ROW_H     14
#define PAT_X     28
#define CH_W      96
#define VIS_CH    6      ///< channels on the screen at once
#define STATUS_Y  464
#define ORD_ROWS  6
#define INS_ROWS  7

/// Fields in a channel's column: note, instrument (2), volume (2), effect, parameter (2).
static const uint8_t col_x[8] = {2, 30, 38, 50, 58, 70, 78, 86};

enum { XF_TITLE, XF_SPEED, XF_BPM, XF_CHANNELS, XF_ROWS, XF_RESTART, XF_OCTAVE, XF_STEP, XF_COUNT };

enum {
    XH_FIELD = 300, XH_DEC, XH_INC, XH_ORDER, XH_ORD_INS, XH_ORD_DEL, XH_ORD_DEC, XH_ORD_INC,
    XH_INSTR, XH_INS_UP, XH_INS_DOWN, XH_METER, XH_PATTERN, XH_LEFT, XH_RIGHT,
    XH_SONG, XH_PATT, XH_STOP, XH_REC, XH_FOLLOW, XH_NEWPAT, XH_CLONE, XH_HELP,
};

static struct {
    int pos, pattern, row, ch, col, first;  ///< the cursor; the first channel shown
    int octave, field, focus;   ///< focus 0 pattern, 1 orders, 2 song (the instrument: T->sample)
    int order_top, ins_top, pat_top;
    bool marking, block;
    int bc0, br0, bc1, br1;
    xm_cell_t *clip;
    int clip_w, clip_h;
} E = {.octave = 4};

static xm_song_t *S(void) {
    return T->xm;
}

static xm_pattern_t *pat(void) {
    return &T->xm->patterns[E.pattern];
}

static xm_cell_t *cell(int r, int c) {
    return &pat()->cells[r * S()->channels + c];
}

// ---- Song changes ----------------------------------------------------------------------------------------

static bool make_pattern(int p, int rows) {
    xm_song_t *s = S();
    if (s->patterns[p].cells)
        return true;
    s->patterns[p].cells = xm_alloc((size_t)rows * s->channels * sizeof(xm_cell_t));
    if (!s->patterns[p].cells)
        return false;
    memset(s->patterns[p].cells, 0, (size_t)rows * s->channels * sizeof(xm_cell_t));
    s->patterns[p].rows = rows;
    if (p >= s->npatterns)
        s->npatterns = p + 1;
    return true;
}

static void go_pos(int pos) {
    xm_song_t *s = S();
    pos = pos < 0 ? 0 : pos >= s->length ? s->length - 1 : pos;
    E.pos = pos;
    E.pattern = s->orders[pos];
    make_pattern(E.pattern, 64);
    if (E.row >= pat()->rows)
        E.row = pat()->rows - 1;
}

static bool set_rows(int rows) {
    xm_song_t *s = S();
    rows = rows < 1 ? 1 : rows > 256 ? 256 : rows;
    xm_pattern_t *p = pat();
    if (rows == p->rows)
        return true;
    xm_cell_t *c = xm_alloc((size_t)rows * s->channels * sizeof *c);
    if (!c)
        return false;
    memset(c, 0, (size_t)rows * s->channels * sizeof *c);
    int keep = rows < p->rows ? rows : p->rows;
    memcpy(c, p->cells, (size_t)keep * s->channels * sizeof *c);
    xm_stop();
    xm_free(p->cells);
    p->cells = c;
    p->rows = rows;
    if (E.row >= rows)
        E.row = rows - 1;
    T->dirty = true;
    return true;
}

static bool set_channels(int n) {
    xm_song_t *s = S();
    n = n < 2 ? 2 : n > XM_MAX_CH ? XM_MAX_CH : n;
    if (n == s->channels)
        return true;
    xm_stop();
    for (int p = 0; p < XM_MAX_PATTERNS; ++p) {
        xm_pattern_t *pt = &s->patterns[p];
        if (!pt->cells)
            continue;
        xm_cell_t *c = xm_alloc((size_t)pt->rows * n * sizeof *c);
        if (!c) {
            trk_message("Not enough memory.");
            return false;
        }
        memset(c, 0, (size_t)pt->rows * n * sizeof *c);
        int keep = n < s->channels ? n : s->channels;
        for (int r = 0; r < pt->rows; ++r)
            memcpy(c + r * n, pt->cells + r * s->channels, keep * sizeof *c);
        xm_free(pt->cells);
        pt->cells = c;
    }
    s->channels = n;
    if (E.ch >= n)
        E.ch = n - 1;
    E.block = E.marking = false;
    T->dirty = true;
    T->full = true;
    return true;
}

static void field_change(int f, int d) {
    xm_song_t *s = S();
    int step = d > 0 ? 1 : -1;
    switch (f) {
    case XF_SPEED: s->speed = s->speed + step < 1 ? 1 : s->speed + step > 31 ? 31 : s->speed + step; break;
    case XF_BPM: s->bpm = s->bpm + d < 32 ? 32 : s->bpm + d > 255 ? 255 : s->bpm + d; break;
    case XF_CHANNELS: set_channels(s->channels + (d > 0 ? 2 : -2)); return;
    case XF_ROWS: set_rows(pat()->rows + (abs(d) >= 10 ? d : step * 16)); return;
    case XF_RESTART: s->restart = s->restart + step < 0 ? 0 : s->restart + step >= s->length ? s->length - 1 : s->restart + step; break;
    case XF_OCTAVE: E.octave = E.octave + step < 0 ? 0 : E.octave + step > 7 ? 7 : E.octave + step; return;
    case XF_STEP: T->step = T->step + step < 0 ? 0 : T->step + step > 16 ? 16 : T->step + step; return;
    default: return;
    }
    T->dirty = true;
}

static void field_enter(int f) {
    xm_song_t *s = S();
    char buf[24];
    trk_dialog_begin();
    if (f == XF_TITLE) {
        snprintf(buf, sizeof buf, "%.20s", s->title);
        if (gui_input("Song", "Title (20 letters):", buf, 21)) {
            memset(s->title, 0, sizeof s->title);
            memcpy(s->title, buf, strnlen(buf, 20));
            T->dirty = true;
        }
    } else if (f == XF_ROWS || f == XF_SPEED || f == XF_BPM) {
        snprintf(buf, sizeof buf, "%d", f == XF_ROWS ? pat()->rows : f == XF_SPEED ? s->speed : s->bpm);
        if (gui_input("Song", f == XF_ROWS ? "Rows in this pattern (1-256):" : f == XF_SPEED ? "Speed (1-31):" : "Tempo (BPM, 32-255):",
                      buf, 4)) {
            int v = atoi(buf);
            if (f == XF_ROWS)
                set_rows(v);
            else if (f == XF_SPEED && v >= 1 && v <= 31)
                s->speed = v, T->dirty = true;
            else if (f == XF_BPM && v >= 32 && v <= 255)
                s->bpm = v, T->dirty = true;
        }
    }
    trk_dialog_end();
}

// ---- Editing ------------------------------------------------------------------------------------------------

static void advance(void) {
    E.row = (E.row + T->step) % pat()->rows;
}

static void block_range(int *c0, int *r0, int *c1, int *r1) {
    if (E.block) {
        *c0 = E.bc0 < E.bc1 ? E.bc0 : E.bc1;
        *c1 = E.bc0 < E.bc1 ? E.bc1 : E.bc0;
        *r0 = E.br0 < E.br1 ? E.br0 : E.br1;
        *r1 = E.br0 < E.br1 ? E.br1 : E.br0;
        if (*r1 >= pat()->rows)
            *r1 = pat()->rows - 1;
    } else {
        *c0 = *c1 = E.ch;
        *r0 = *r1 = E.row;
    }
}

static bool in_block(int r, int c) {
    if (!E.block)
        return false;
    int c0, r0, c1, r1;
    block_range(&c0, &r0, &c1, &r1);
    return r >= r0 && r <= r1 && c >= c0 && c <= c1;
}

static void copy_block(bool cut) {
    int c0, r0, c1, r1;
    block_range(&c0, &r0, &c1, &r1);
    int w = c1 - c0 + 1, h = r1 - r0 + 1;
    xm_free(E.clip);
    E.clip = xm_alloc((size_t)w * h * sizeof(xm_cell_t));
    if (!E.clip) {
        trk_message("Not enough memory.");
        E.clip_w = 0;
        return;
    }
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            E.clip[r * w + c] = *cell(r0 + r, c0 + c);
            if (cut)
                *cell(r0 + r, c0 + c) = (xm_cell_t){0};
        }
    E.clip_w = w;
    E.clip_h = h;
    if (cut)
        T->dirty = true;
    trk_message("%s %d rows x %d channels.", cut ? "Cut" : "Copied", h, w);
}

static void paste_block(bool mix) {
    if (!E.clip_w) {
        trk_message("Nothing copied yet (Alt-C copies the block).");
        return;
    }
    for (int r = 0; r < E.clip_h && E.row + r < pat()->rows; ++r)
        for (int c = 0; c < E.clip_w && E.ch + c < S()->channels; ++c) {
            xm_cell_t v = E.clip[r * E.clip_w + c], *d = cell(E.row + r, E.ch + c);
            if (mix) {
                if (d->note || d->instrument)
                    v.note = d->note, v.instrument = d->instrument;
                if (d->volume)
                    v.volume = d->volume;
                if (d->effect || d->param)
                    v.effect = d->effect, v.param = d->param;
            }
            *d = v;
        }
    T->dirty = true;
}

static void transpose(int by) {
    int c0, r0, c1, r1;
    block_range(&c0, &r0, &c1, &r1);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            xm_cell_t *v = cell(r, c);
            if (v->note >= 1 && v->note <= 96 && v->note + by >= 1 && v->note + by <= 96)
                v->note += by;
        }
    T->dirty = true;
}

static void shift_rows(int c0, int c1, bool insert) {
    int rows = pat()->rows;
    for (int c = c0; c <= c1; ++c) {
        if (insert) {
            for (int r = rows - 1; r > E.row; --r)
                *cell(r, c) = *cell(r - 1, c);
            *cell(E.row, c) = (xm_cell_t){0};
        } else {
            for (int r = E.row; r < rows - 1; ++r)
                *cell(r, c) = *cell(r + 1, c);
            *cell(rows - 1, c) = (xm_cell_t){0};
        }
    }
    T->dirty = true;
}

static void new_pattern(bool clone) {
    xm_song_t *s = S();
    int p = 0;
    while (p < XM_MAX_PATTERNS && s->patterns[p].cells)
        ++p;
    if (p >= XM_MAX_PATTERNS || s->length >= 256) {
        trk_message("The song is full.");
        return;
    }
    if (!make_pattern(p, clone ? pat()->rows : 64)) {
        trk_message("Not enough memory.");
        return;
    }
    if (clone)
        memcpy(s->patterns[p].cells, pat()->cells, (size_t)pat()->rows * s->channels * sizeof(xm_cell_t));
    memmove(s->orders + E.pos + 2, s->orders + E.pos + 1, 254 - E.pos);
    s->orders[E.pos + 1] = p;
    s->length++;
    go_pos(E.pos + 1);
    T->dirty = true;
    trk_message("Pattern %02X %s at position %02d.", p, clone ? "cloned" : "made", E.pos);
}

static void order_change(int d) {
    xm_song_t *s = S();
    int p = s->orders[E.pos] + d;
    if (p < 0 || p >= XM_MAX_PATTERNS)
        return;
    s->orders[E.pos] = p;
    go_pos(E.pos);
    T->dirty = true;
}

static void order_insert(void) {
    xm_song_t *s = S();
    if (s->length >= 256)
        return;
    memmove(s->orders + E.pos + 1, s->orders + E.pos, 255 - E.pos);
    s->length++;
    T->dirty = true;
}

static void order_delete(void) {
    xm_song_t *s = S();
    if (s->length <= 1)
        return;
    memmove(s->orders + E.pos, s->orders + E.pos + 1, 255 - E.pos);
    s->length--;
    if (s->restart >= s->length)
        s->restart = 0;
    go_pos(E.pos);
    T->dirty = true;
}

/// A piano key's note, 1..96, or -1; 97 for key off (the key left of 1, or 1).
static int note_key(int k) {
    static const char lower[] = "zsxdcvgbhnjm,l.;/";
    static const char upper[] = "q2w3er5t6y7ui9o0p";
    if (k == '`' || k == '1')
        return 97;
    if (k < 0x20 || k >= 0x7f)
        return -1;
    int c = k >= 'A' && k <= 'Z' ? k + 32 : k;
    const char *p;
    int n;
    if ((p = strchr(lower, c)))
        n = p - lower;
    else if ((p = strchr(upper, c)))
        n = 12 + (p - upper);
    else
        return -1;
    n += E.octave * 12 + 1;
    return n <= 96 ? n : -1;
}

static int hex_digit(int k) {
    if (k >= '0' && k <= '9')
        return k - '0';
    if (k >= 'a' && k <= 'f')
        return k - 'a' + 10;
    if (k >= 'A' && k <= 'F')
        return k - 'A' + 10;
    return -1;
}

/// An effect's letter as FT2 shows it: 0-9, then A-Z.
static int effect_of_key(int k) {
    if (k >= '0' && k <= '9')
        return k - '0';
    if (k >= 'a' && k <= 'z')
        return k - 'a' + 10;
    if (k >= 'A' && k <= 'Z')
        return k - 'A' + 10;
    return -1;
}

static void move(int dr, int dc) {
    int rows = pat()->rows;
    if (dr) {
        E.row = (E.row + dr) % rows;
        if (E.row < 0)
            E.row += rows;
    }
    if (dc) {
        int n = S()->channels * 8, k = E.ch * 8 + E.col + dc;
        k = (k + n) % n;
        E.ch = k / 8;
        E.col = k % 8;
    }
    if (E.marking) {
        E.bc1 = E.ch;
        E.br1 = E.row;
    }
}

static void pattern_key(int k) {
    switch (k) {
    case KEY_UP: move(-1, 0); return;
    case KEY_DOWN: move(1, 0); return;
    case KEY_LEFT: move(0, -1); return;
    case KEY_RIGHT: move(0, 1); return;
    case KEY_PGUP: move(E.row >= 16 ? -16 : -E.row, 0); return;
    case KEY_PGDN: move(E.row + 16 < pat()->rows ? 16 : pat()->rows - 1 - E.row, 0); return;
    case KEY_HOME: move(-E.row, 0); return;
    case KEY_END: move(pat()->rows - 1 - E.row, 0); return;
    case '\t': E.ch = (E.ch + 1) % S()->channels; E.col = 0; move(0, 0); return;
    case KEY_INS: shift_rows(E.ch, E.ch, true); return;
    case '\b': shift_rows(E.ch, E.ch, false); return;
    case '\r':
        for (int c = 0; c < S()->channels; ++c) {
            xm_cell_t *v = cell(E.row, c);
            if (v->note)
                xm_jam(S(), c, v->instrument ? v->instrument : T->sample, v->note);
        }
        move(1, 0);
        return;
    case KEY_DEL: {
        if (!T->record)
            return;
        xm_cell_t *v = cell(E.row, E.ch);
        if (E.col == 0)
            v->note = v->instrument = 0;
        else if (E.col <= 2)
            v->instrument = 0;
        else if (E.col <= 4)
            v->volume = 0;
        else
            v->effect = v->param = 0;
        T->dirty = true;
        advance();
        return;
    }
    }
    xm_cell_t *v = cell(E.row, E.ch);
    if (E.col == 0) {
        int n = note_key(k);
        if (n < 0)
            return;
        if (T->record) {
            v->note = n;
            if (n != 97)
                v->instrument = T->sample;
            T->dirty = true;
        }
        if (n != 97)
            xm_jam(S(), E.ch, T->sample, n);
        if (T->record)
            advance();
        return;
    }
    if (!T->record)
        return;
    if (E.col == 3) {
        // The volume column: 0-4 for a volume (00-40); - + d u s v p l r m for its commands.
        static const char cmds[] = "-+dusvplrm";
        int c = k >= 'A' && k <= 'Z' ? k + 32 : k;
        const char *p = strchr(cmds, c);
        if (p && c) {
            v->volume = (0x60 + (p - cmds) * 0x10) | (v->volume >= 0x60 ? v->volume & 15 : 0);
        } else if (k >= '0' && k <= '4') {
            int lo = v->volume >= 0x10 && v->volume <= 0x50 ? (v->volume - 0x10) & 15 : 0;
            v->volume = 0x10 + ((k - '0') << 4 | lo);
            if (v->volume > 0x50)
                v->volume = 0x50;
        } else {
            return;
        }
    } else if (E.col == 5) {
        int e = effect_of_key(k);
        if (e < 0 || e > 35)
            return;
        v->effect = e;
    } else {
        int d = hex_digit(k);
        if (d < 0)
            return;
        switch (E.col) {
        case 1: v->instrument = (d << 4 | (v->instrument & 15)); break;
        case 2: v->instrument = (v->instrument & 0xf0) | d; break;
        case 4:
            if (v->volume >= 0x60)
                v->volume = (v->volume & 0xf0) | d;
            else if (v->volume >= 0x10)
                v->volume = ((v->volume - 0x10) & 0xf0) + 0x10 + d > 0x50 ? 0x50 : ((v->volume - 0x10) & 0xf0) + 0x10 + d;
            else
                v->volume = 0x10 + d;
            break;
        case 6: v->param = d << 4 | (v->param & 15); break;
        case 7: v->param = (v->param & 0xf0) | d; break;
        }
        if (v->instrument > 128)
            v->instrument = 128;
    }
    T->dirty = true;
    advance();
}

static void orders_key(int k) {
    switch (k) {
    case KEY_UP: go_pos(E.pos - 1); break;
    case KEY_DOWN: go_pos(E.pos + 1); break;
    case KEY_PGUP: go_pos(E.pos - ORD_ROWS); break;
    case KEY_PGDN: go_pos(E.pos + ORD_ROWS); break;
    case KEY_HOME: go_pos(0); break;
    case KEY_END: go_pos(S()->length - 1); break;
    case KEY_LEFT: order_change(-1); break;
    case KEY_RIGHT: order_change(1); break;
    case KEY_INS: order_insert(); break;
    case KEY_DEL: order_delete(); break;
    case '\r': E.focus = 0; break;
    }
}

static void song_key(int k) {
    switch (k) {
    case KEY_UP: E.field = (E.field + XF_COUNT - 1) % XF_COUNT; break;
    case KEY_DOWN: E.field = (E.field + 1) % XF_COUNT; break;
    case KEY_LEFT: field_change(E.field, -1); break;
    case KEY_RIGHT: field_change(E.field, 1); break;
    case KEY_PGDN: field_change(E.field, -10); break;
    case KEY_PGUP: field_change(E.field, 10); break;
    case '\r': field_enter(E.field); break;
    }
}

static void play(int how) {
    xm_set_loop(true);
    xm_pause(false);
    if (how == 0)
        xm_play(S(), E.pos, 0);
    else if (how == 1)
        xm_play_pattern(S(), E.pattern, 0);
    else
        xm_play(S(), E.pos, E.row);
}

/// The tracker's commands (menus, buttons, keys) for an XM song. True if done.
bool xed_command(int id) {
    switch (id) {
    case HIT_PLAY_SONG: play(0); return true;
    case HIT_PLAY_PATTERN: play(1); return true;
    case HIT_PLAY_POS: play(2); return true;
    case HIT_STOP: xm_stop(); return true;
    case HIT_PAT_NEW: new_pattern(false); return true;
    case HIT_PAT_CLONE: new_pattern(true); return true;
    case HIT_PAT_CLEAR:
        memset(pat()->cells, 0, (size_t)pat()->rows * S()->channels * sizeof(xm_cell_t));
        T->dirty = true;
        return true;
    case HIT_ORDER_INS: order_insert(); return true;
    case HIT_ORDER_DEL: order_delete(); return true;
    case XED_BEGIN:
        E.block = E.marking = true;
        E.bc0 = E.bc1 = E.ch;
        E.br0 = E.br1 = E.row;
        return true;
    case XED_END:
        if (!E.block) {
            E.block = true;
            E.bc0 = E.ch;
            E.br0 = E.row;
        }
        E.bc1 = E.ch;
        E.br1 = E.row;
        E.marking = false;
        return true;
    case XED_MARK: {
        bool channel = E.block && E.bc0 == E.ch && E.bc1 == E.ch && E.br0 == 0 && E.br1 == pat()->rows - 1;
        E.block = true;
        E.marking = false;
        E.br0 = 0;
        E.br1 = pat()->rows - 1;
        E.bc0 = channel ? 0 : E.ch;
        E.bc1 = channel ? S()->channels - 1 : E.ch;
        return true;
    }
    case XED_UNMARK: E.block = E.marking = false; return true;
    case XED_CUT: copy_block(true); return true;
    case XED_COPY: copy_block(false); return true;
    case XED_PASTE: paste_block(false); return true;
    case XED_MIX: paste_block(true); return true;
    case XED_UP1: transpose(1); return true;
    case XED_DOWN1: transpose(-1); return true;
    case XED_UP12: transpose(12); return true;
    case XED_DOWN12: transpose(-12); return true;
    case XED_INS_ROW: shift_rows(0, S()->channels - 1, true); return true;
    case XED_DEL_ROW: shift_rows(0, S()->channels - 1, false); return true;
    case XED_MUTE: xm_mute(E.ch, !xm_muted(E.ch)); return true;
    case XED_SOLO: {
        bool solo = !xm_muted(E.ch);
        for (int i = 0; i < S()->channels; ++i)
            solo = solo && (i == E.ch || xm_muted(i));
        for (int i = 0; i < XM_MAX_CH; ++i)
            xm_mute(i, !solo && i != E.ch);
        return true;
    }
    case XED_UNMUTE:
        for (int i = 0; i < XM_MAX_CH; ++i)
            xm_mute(i, false);
        return true;
    }
    return false;
}

/// Keys for an XM song; false if the tracker should take it (F-keys, Ctrl-S ...).
bool xed_key(int k) {
    if (k >= KEY_ALT) {
        static const struct {
            char key;
            int id;
        } alts[] = {{'b', XED_BEGIN}, {'e', XED_END}, {'l', XED_MARK}, {'u', XED_UNMARK},
                    {'x', XED_CUT}, {'c', XED_COPY}, {'v', XED_PASTE}, {'m', XED_MIX},
                    {'q', XED_UP1}, {'a', XED_DOWN1}, {'w', XED_UP12}, {'s', XED_DOWN12},
                    {'i', XED_INS_ROW}, {'d', XED_DEL_ROW}, {'n', HIT_PAT_NEW},
                    {'k', HIT_PAT_CLONE}, {'r', HIT_PAT_CLEAR}};
        for (unsigned i = 0; i < sizeof alts / sizeof *alts; ++i)
            if (k - KEY_ALT == alts[i].key)
                return xed_command(alts[i].id);
        return false;
    }
    switch (k) {
    case KEY_F4:
        E.focus = (E.focus + 1) % 3;
        T->full = true;
        return true;
    case KEY_F11: field_change(XF_OCTAVE, -1); return true;
    case KEY_F12: field_change(XF_OCTAVE, 1); return true;
    case '[': field_change(XF_STEP, -1); return true;
    case ']': field_change(XF_STEP, 1); return true;
    case '-': if (T->sample > 1) T->sample--; return true;
    case '=': case '+': if (T->sample < 128) T->sample++; return true;
    case '<': go_pos(E.pos - 1); return true;
    case '>': go_pos(E.pos + 1); return true;
    case CTRL('t'): return xed_command(XED_MUTE);
    case CTRL('l'): return xed_command(XED_SOLO);
    case CTRL('u'): return xed_command(XED_UNMUTE);
    case KEY_F5: return xed_command(HIT_PLAY_SONG);
    case KEY_F6: return xed_command(HIT_PLAY_PATTERN);
    case KEY_F7: return xed_command(HIT_PLAY_POS);
    case KEY_F8: return xed_command(HIT_STOP);
    case ' ': {
        xm_status_t st;
        xm_get_status(&st);
        if (st.playing)
            xm_stop();
        else
            T->record = !T->record;
        return true;
    }
    }
    if (k >= KEY_UP || k == '\r' || k == '\t' || k == '\b' || (k >= 0x20 && k < 0x7f)) {
        if (E.focus == 1)
            orders_key(k);
        else if (E.focus == 2)
            song_key(k);
        else
            pattern_key(k);
        return k < KEY_F1 || k > KEY_F12;
    }
    return false;
}

// ---- Drawing ---------------------------------------------------------------------------------------------

static void field_box(int x, int y, int chars, bool focus, uint8_t fg, const char *text) {
    uint8_t bg = focus ? C_FOCUS : C_FIELD;
    gfx_fill(x, y, 2, 16, bg);
    int end = gfx_text(x + 2, y, text, chars, fg, bg);
    gfx_fill(end, y, 2, 16, bg);
}

static void draw_song(xm_status_t *st) {
    static const char *const labels[XF_COUNT] = {"Title", "Speed", "Tempo", "Chans",
                                                 "Rows", "Restart", "Octave", "Step"};
    xm_song_t *s = S();
    if (T->full)
        gfx_bevel(SONG_X, TOP_Y, SONG_W, TOP_H, true, C_FACE);
    for (int f = 0; f < XF_COUNT; ++f) {
        int y = TOP_Y + 8 + f * 16;
        bool focus = E.focus == 2 && E.field == f;
        gfx_text(SONG_X + 6, y, labels[f], 7, focus ? C_YELLOW : C_LABEL, C_FACE);
        char v[24];
        switch (f) {
        case XF_TITLE: snprintf(v, sizeof v, "%.20s", s->title); break;
        case XF_SPEED: snprintf(v, sizeof v, "%d", st->playing ? st->speed : s->speed); break;
        case XF_BPM: snprintf(v, sizeof v, "%d", st->playing ? st->bpm : s->bpm); break;
        case XF_CHANNELS: snprintf(v, sizeof v, "%d", s->channels); break;
        case XF_ROWS: snprintf(v, sizeof v, "%d", pat()->rows); break;
        case XF_RESTART: snprintf(v, sizeof v, "%02d", s->restart); break;
        case XF_OCTAVE: snprintf(v, sizeof v, "%d", E.octave); break;
        case XF_STEP: snprintf(v, sizeof v, "%d", T->step); break;
        }
        if (f == XF_TITLE) {
            field_box(SONG_X + 64, y, 20, focus, C_TEXT, v);
            hit_add(SONG_X + 64, y, 164, 16, XH_FIELD, f);
            continue;
        }
        field_box(SONG_X + 64, y, 5, focus, C_TEXT, v);
        hit_add(SONG_X + 64, y, 44, 16, XH_FIELD, f);
        gfx_button(SONG_X + 112, y, 18, 16, "-", XH_DEC, f, false);
        gfx_button(SONG_X + 132, y, 18, 16, "+", XH_INC, f, false);
        const char *info = f == XF_SPEED ? "XM" : f == XF_BPM ? (s->linear ? "linear" : "Amiga")
                         : f == XF_OCTAVE ? "" : "";
        if (f == XF_OCTAVE) {
            char keys[12];
            snprintf(keys, sizeof keys, "Z=C-%d", E.octave);
            gfx_text(SONG_X + 156, y, keys, 9, C_DIM, C_FACE);
        } else {
            gfx_text(SONG_X + 156, y, info, 9, C_DIM, C_FACE);
        }
    }
}

static void draw_orders(void) {
    xm_song_t *s = S();
    if (T->full)
        gfx_bevel(ORD_X, TOP_Y, ORD_W, TOP_H, true, C_FACE);
    if (E.pos < E.order_top)
        E.order_top = E.pos;
    if (E.pos >= E.order_top + ORD_ROWS)
        E.order_top = E.pos - ORD_ROWS + 1;
    gfx_printf(ORD_X + 6, TOP_Y + 8, 12, C_LABEL, C_FACE, "Pos Pat  %3d", s->length);
    for (int i = 0; i < ORD_ROWS; ++i) {
        int p = E.order_top + i, y = TOP_Y + 24 + i * 16;
        bool cur = p == E.pos;
        uint8_t bg = cur ? (E.focus == 1 ? C_FOCUS : C_CURROW) : C_FIELD;
        if (p < s->length)
            gfx_printf(ORD_X + 6, y, 12, cur ? C_YELLOW : C_TEXT, bg, "%02d  %02X", p, s->orders[p]);
        else
            gfx_text(ORD_X + 6, y, "", 12, C_TEXT, C_FIELD);
        hit_add(ORD_X + 6, y, 96, 16, p < s->length ? XH_ORDER : HIT_NONE, p);
    }
    int y = TOP_Y + TOP_H - 22;
    gfx_button(ORD_X + 4, y, 26, 18, "In", XH_ORD_INS, 0, false);
    gfx_button(ORD_X + 30, y, 26, 18, "De", XH_ORD_DEL, 0, false);
    gfx_button(ORD_X + 56, y, 26, 18, "-", XH_ORD_DEC, 0, false);
    gfx_button(ORD_X + 82, y, 26, 18, "+", XH_ORD_INC, 0, false);
}

static void draw_instruments(void) {
    xm_song_t *s = S();
    if (T->full)
        gfx_bevel(INS_X, TOP_Y, INS_W, TOP_H, true, C_FACE);
    if (T->sample < E.ins_top + 1)
        E.ins_top = T->sample - 1;
    if (T->sample > E.ins_top + INS_ROWS)
        E.ins_top = T->sample - INS_ROWS;
    for (int i = 0; i < INS_ROWS; ++i) {
        int n = E.ins_top + 1 + i, y = TOP_Y + 8 + i * 16;
        bool cur = n == T->sample;
        uint8_t bg = cur ? C_CURROW : C_FIELD;
        const xm_instrument_t *in = n <= s->ninstruments ? &s->instruments[n - 1] : NULL;
        char name[23] = "";
        if (in)
            for (int k = 0; k < 22 && in->name[k]; ++k)
                name[k] = in->name[k] < 0x20 || in->name[k] > 0x7e ? '.' : in->name[k];
        gfx_printf(INS_X + 6, y, 3, C_YELLOW, bg, "%02X", n);
        gfx_text(INS_X + 30, y, name, 22, in && in->nsamples ? C_TEXT : C_DIM, bg);
        gfx_printf(INS_X + 206, y, 9, C_CYAN, bg, in && in->nsamples ? " %2d smp" : "", in ? in->nsamples : 0);
        hit_add(INS_X + 6, y, 272, 16, XH_INSTR, n);
    }
    int y = TOP_Y + TOP_H - 22;
    gfx_button(INS_X + 4, y, 34, 18, "Up", XH_INS_UP, 0, false);
    gfx_button(INS_X + 38, y, 34, 18, "Dn", XH_INS_DOWN, 0, false);
    gfx_printf(INS_X + 80, y + 1, 26, C_DIM, C_FACE, "%d instruments", s->ninstruments);
}

static void draw_meters(xm_status_t *st) {
    int n = S()->channels, w = DVI_WIDTH / n;
    for (int c = 0; c < n; ++c) {
        int x = c * w;
        if (T->full)
            gfx_bevel(x, METER_Y, w, METER_H, false, C_BG);
        bool muted = xm_muted(c);
        int h = METER_H - 20, len = st->level[c] * h / 64;
        for (int r = 0; r < h; ++r) {
            uint8_t col = r >= len ? C_DARK : r < h * 6 / 10 ? C_GREEN : r < h * 85 / 100 ? C_YELLOW : C_RED;
            gfx_hline(x + 3, METER_Y + METER_H - 3 - r, w - 6, muted ? C_DARK : col);
        }
        char label[4];
        snprintf(label, sizeof label, "%d", c + 1);
        gfx_text(x + 2, METER_Y + 1, label, w >= 24 ? 2 : 1, muted ? C_RED : C_YELLOW, C_BG);
        hit_add(x, METER_Y, w, METER_H, XH_METER, c);
    }
}

static void draw_transport(xm_status_t *st) {
    static const struct {
        const char *label;
        int id;
    } b[] = {
        {"Song F5", XH_SONG}, {"Patt F6", XH_PATT}, {"Stop F8", XH_STOP}, {"Rec", XH_REC},
        {"Follow", XH_FOLLOW}, {"NewPat", XH_NEWPAT}, {"Clone", XH_CLONE}, {"Help F1", XH_HELP},
    };
    if (T->full)
        gfx_fill(0, TRANS_Y, DVI_WIDTH, 24, C_FACE);
    for (int i = 0; i < 8; ++i) {
        bool lit = (b[i].id == XH_SONG && st->playing) || (b[i].id == XH_REC && T->record) ||
                   (b[i].id == XH_FOLLOW && T->follow);
        gfx_button(2 + i * 66, TRANS_Y + 2, 64, 20, b[i].label, b[i].id, 0, lit);
    }
    gfx_printf(2 + 8 * 66, TRANS_Y + 4, 13, T->load > 700 ? C_RED : C_LABEL, C_FACE, "CPU%3u.%u%% %s",
               T->load / 10, T->load % 10, T->underruns ? "!" : "");
}

static void vol_text(uint8_t v, char out[3]) {
    static const char sym[10] = {'-', '+', 'D', 'U', 'S', 'V', 'P', 'L', 'R', 'M'};
    if (!v)
        strcpy(out, "..");
    else if (v >= 0x10 && v <= 0x50)
        snprintf(out, 3, "%02X", v - 0x10);
    else if (v >= 0x60)
        snprintf(out, 3, "%c%X", sym[(v >> 4) - 6], v & 15);
    else
        strcpy(out, "??");
}

static void draw_pattern(xm_status_t *st) {
    xm_song_t *s = S();
    int nch = s->channels, rows = pat()->rows;
    // Keep the cursor's channel on the screen.
    if (E.ch < E.first)
        E.first = E.ch;
    if (E.ch >= E.first + VIS_CH)
        E.first = E.ch - VIS_CH + 1;
    if (E.first > nch - VIS_CH)
        E.first = nch > VIS_CH ? nch - VIS_CH : 0;
    int vis = nch < VIS_CH ? nch : VIS_CH;
    gfx_printf(0, HEAD_Y, 3, C_YELLOW, C_FACE, "%02X", E.pattern);
    for (int i = 0; i < vis; ++i) {
        int c = E.first + i, x = PAT_X + i * CH_W;
        gfx_printf(x, HEAD_Y, 11, xm_muted(c) ? C_RED : C_LABEL, C_FACE, " Ch %d%s", c + 1, xm_muted(c) ? " mute" : "");
        gfx_vline(x + CH_W - 1, HEAD_Y, 16, C_DARK);
        hit_add(x, HEAD_Y, CH_W, 16, XH_METER, c);
    }
    int rx = PAT_X + vis * CH_W;
    gfx_fill(rx, HEAD_Y, DVI_WIDTH - rx, 16, C_FACE);
    // Scrolling buttons when not all channels fit.
    if (nch > VIS_CH) {
        gfx_button(rx + 2, HEAD_Y, 18, 16, "\x1b", XH_LEFT, 0, false);
        gfx_button(rx + 22, HEAD_Y, 18, 16, "\x1a", XH_RIGHT, 0, false);
    }
    bool here = st->playing && st->pattern == E.pattern;
    if (here && T->follow)
        E.pat_top = E.row - PAT_ROWS / 2;
    else if (E.row < E.pat_top + 1)
        E.pat_top = E.row - 1;
    else if (E.row > E.pat_top + PAT_ROWS - 2)
        E.pat_top = E.row - PAT_ROWS + 2;
    if (E.pat_top < -PAT_ROWS / 2)
        E.pat_top = -PAT_ROWS / 2;
    if (E.pat_top > rows - PAT_ROWS / 2 - 1)
        E.pat_top = rows - PAT_ROWS / 2 - 1;
    for (int i = 0; i < PAT_ROWS; ++i) {
        int r = E.pat_top + i, y = PAT_Y + i * ROW_H;
        if (r < 0 || r >= rows) {
            gfx_fill(0, y, DVI_WIDTH, ROW_H, C_DARK);
            continue;
        }
        uint8_t bg = here && r == st->row ? C_PLAYROW : r == E.row ? C_CURROW
                   : r % 16 == 0 ? C_ROW16 : r % 4 == 0 ? C_ROW4 : C_BG;
        char num[4];
        snprintf(num, sizeof num, "%02X", r);
        gfx_fill(0, y, 2, ROW_H, bg);
        gfx_text14(2, y, num, 2, r % 4 ? C_DIM : C_YELLOW, bg);
        gfx_fill(18, y, PAT_X - 18, ROW_H, bg);
        for (int i2 = 0; i2 < vis; ++i2) {
            int c = E.first + i2, x = PAT_X + i2 * CH_W;
            uint8_t cb = in_block(r, c) ? C_BLOCK : bg;
            xm_cell_t v = *cell(r, c);
            char note[4], ins[3], vol[3], fx[4];
            if (v.note == 97)
                strcpy(note, "===");
            else if (v.note >= 1 && v.note <= 96) {
                static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                                      "F#", "G-", "G#", "A-", "A#", "B-"};
                snprintf(note, sizeof note, "%s%d", names[(v.note - 1) % 12], (v.note - 1) / 12);
            } else
                strcpy(note, "...");
            if (v.instrument)
                snprintf(ins, sizeof ins, "%02X", v.instrument);
            else
                strcpy(ins, "..");
            vol_text(v.volume, vol);
            if (v.effect || v.param)
                snprintf(fx, sizeof fx, "%c%02X", v.effect < 10 ? '0' + v.effect : 'A' + v.effect - 10, v.param);
            else
                strcpy(fx, "...");
            gfx_fill(x, y, 2, ROW_H, cb);
            gfx_fill(x + 26, y, 4, ROW_H, cb);
            gfx_fill(x + 46, y, 4, ROW_H, cb);
            gfx_fill(x + 66, y, 4, ROW_H, cb);
            gfx_fill(x + 94, y, 1, ROW_H, cb);
            gfx_vline(x + 95, y, ROW_H, C_FACE);
            bool cur = r == E.row && c == E.ch && E.focus == 0;
            const char *texts[8] = {note, ins, ins + 1, vol, vol + 1, fx, fx + 1, fx + 2};
            int lens[8] = {3, 1, 1, 1, 1, 1, 1, 1};
            uint8_t fgs[8] = {v.note ? C_TEXT : C_DIM, v.instrument ? C_CYAN : C_DIM, v.instrument ? C_CYAN : C_DIM,
                              v.volume ? C_GREEN : C_DIM, v.volume ? C_GREEN : C_DIM,
                              v.effect || v.param ? C_ORANGE : C_DIM, v.effect || v.param ? C_ORANGE : C_DIM,
                              v.effect || v.param ? C_ORANGE : C_DIM};
            for (int k = 0; k < 8; ++k) {
                bool on = cur && E.col == k;
                gfx_text14(x + col_x[k], y, texts[k], lens[k], on ? C_BG : fgs[k], on ? C_CURSOR : cb);
            }
        }
        gfx_fill(rx, y, DVI_WIDTH - rx, ROW_H, C_DARK);
        hit_add(0, y, DVI_WIDTH, ROW_H, XH_PATTERN, r);
    }
    gfx_fill(0, PAT_Y + PAT_ROWS * ROW_H, DVI_WIDTH, STATUS_Y - PAT_Y - PAT_ROWS * ROW_H, C_FACE);
}

static void draw_status(xm_status_t *st) {
    char line[81];
    if (T->msg[0] && to_ms_since_boot(get_absolute_time()) < T->msg_until) {
        snprintf(line, sizeof line, " %s", T->msg);
    } else {
        T->msg[0] = 0;
        if (st->playing)
            snprintf(line, sizeof line, " Playing: position %02d pattern %02X row %02X  voices %d  %s",
                     st->order, st->pattern, st->row, st->active, T->record ? "RECORDING" : "");
        else
            snprintf(line, sizeof line, " XM  %s  Instrument %02X  Octave %d  Step %d  Ch %d/%d  F1 keys F9 menu",
                     T->record ? "RECORD" : "Edit off", T->sample, E.octave, T->step, E.ch + 1, S()->channels);
    }
    gfx_text(0, STATUS_Y, line, 80, T->msg[0] ? C_YELLOW : C_LABEL, C_FACE);
}

void xed_draw(void) {
    xm_status_t st;
    xm_get_status(&st);
    if (T->follow && st.playing) {
        if (st.order != E.pos && st.order < S()->length && st.pattern == S()->orders[st.order])
            go_pos(st.order);
        if (st.pattern == E.pattern)
            E.row = st.row;
    }
    draw_song(&st);
    draw_orders();
    draw_instruments();
    draw_meters(&st);
    draw_transport(&st);
    draw_pattern(&st);
    draw_status(&st);
}

// ---- The mouse -----------------------------------------------------------------------------------------

static void pattern_click(int x, int y, bool drag) {
    int i = (y - PAT_Y) / ROW_H, r = E.pat_top + i;
    if (r < 0 || r >= pat()->rows || x < PAT_X)
        return;
    int vis = S()->channels < VIS_CH ? S()->channels : VIS_CH;
    int ci = (x - PAT_X) / CH_W, off = (x - PAT_X) % CH_W;
    if (ci >= vis)
        return;
    int c = E.first + ci, k = 0;
    for (int f = 0; f < 8; ++f)
        if (off >= col_x[f])
            k = f;
    if (drag) {
        if (!E.marking) {
            E.block = E.marking = true;
            E.bc0 = E.ch;
            E.br0 = E.row;
        }
        if (i <= 0 && r > 0)
            --r;
        else if (i >= PAT_ROWS - 1 && r < pat()->rows - 1)
            ++r;
        E.ch = c;
        E.row = r;
        E.bc1 = c;
        E.br1 = r;
        return;
    }
    E.focus = 0;
    E.ch = c;
    E.col = k;
    E.row = r;
    E.marking = E.block = false;
}

/// A click (or wheel, or drag) on the XM screen; false if it isn't one of its places.
bool xed_click(const hit_t *h, const mouse_event_t *e) {
    if (!h)
        return false;
    if (e->type == MOUSE_WHEEL) {
        int d = -e->wheel;
        if (h->id == XH_ORDER)
            go_pos(E.pos + d);
        else if (h->id == XH_INSTR)
            T->sample = T->sample + d < 1 ? 1 : T->sample + d > 128 ? 128 : T->sample + d;
        else if (h->id == XH_FIELD || h->id == XH_DEC || h->id == XH_INC)
            field_change(h->arg, -d);
        else if (h->id == XH_METER || h->id == XH_LEFT || h->id == XH_RIGHT)
            E.ch = E.ch + d < 0 ? 0 : E.ch + d >= S()->channels ? S()->channels - 1 : E.ch + d;
        else
            move(d * 2, 0);
        return true;
    }
    if (e->type == MOUSE_MOVE) {
        if (h->id == XH_PATTERN && (e->buttons & MOUSE_LEFT))
            pattern_click(e->x, e->y, true);
        return h->id == XH_PATTERN;
    }
    if (e->type != MOUSE_DOWN)
        return false;
    if (e->button == MOUSE_RIGHT && h->id == XH_METER) {
        E.ch = h->arg;
        return xed_command(XED_SOLO);
    }
    switch (h->id) {
    case XH_FIELD: E.focus = 2; E.field = h->arg; field_enter(h->arg); return true;
    case XH_DEC: field_change(h->arg, -1); return true;
    case XH_INC: field_change(h->arg, 1); return true;
    case XH_ORDER: go_pos(h->arg); E.focus = 1; return true;
    case XH_ORD_INS: order_insert(); return true;
    case XH_ORD_DEL: order_delete(); return true;
    case XH_ORD_DEC: order_change(-1); return true;
    case XH_ORD_INC: order_change(1); return true;
    case XH_INSTR: T->sample = h->arg; return true;
    case XH_INS_UP: if (T->sample > 1) T->sample--; return true;
    case XH_INS_DOWN: if (T->sample < 128) T->sample++; return true;
    case XH_METER: xm_mute(h->arg, !xm_muted(h->arg)); return true;
    case XH_LEFT: if (E.first > 0) E.first--, E.ch = E.first; return true;
    case XH_RIGHT: if (E.first + VIS_CH < S()->channels) E.first++, E.ch = E.first + VIS_CH - 1; return true;
    case XH_PATTERN:
        pattern_click(e->x, e->y, false);
        if (e->double_click)
            T->record = !T->record;
        T->full = true;
        return true;
    case XH_SONG: return xed_command(HIT_PLAY_SONG);
    case XH_PATT: return xed_command(HIT_PLAY_PATTERN);
    case XH_STOP: return xed_command(HIT_STOP);
    case XH_REC: T->record = !T->record; return true;
    case XH_FOLLOW: T->follow = !T->follow; return true;
    case XH_NEWPAT: return xed_command(HIT_PAT_NEW);
    case XH_CLONE: return xed_command(HIT_PAT_CLONE);
    case XH_HELP: help_show(); return true;
    }
    return false;
}

// ---- The song coming and going -------------------------------------------------------------------------

int xed_pattern(void) {
    return E.pattern;
}

void xed_refresh(void) {
    go_pos(E.pos);
    if (E.ch >= S()->channels)
        E.ch = S()->channels - 1;
    T->full = true;
}

void xed_take(xm_song_t *s) {
    xed_close();
    T->xm = s;
    xmi_reset();
    T->screen = SCREEN_PATTERN;
    memset(&E, 0, sizeof E);
    T->sample = 1;
    E.octave = 4;
    go_pos(0);
    T->full = true;
    undo_reset();
}

bool xed_new(int channels) {
    xm_song_t *s = psram_malloc(sizeof *s);
    if (!s)
        return false;
    memset(s, 0, sizeof *s);
    s->channels = channels;
    s->length = 1;
    s->speed = 6;
    s->bpm = 125;
    s->linear = true;
    s->version = 0x104;
    memcpy(s->tracker, "Pico Tracker", 12);
    s->instruments = xm_alloc(sizeof *s->instruments);
    if (!s->instruments) {
        psram_free(s);
        return false;
    }
    memset(s->instruments, 0, sizeof *s->instruments);
    xed_take(s);
    if (!make_pattern(0, 64)) {
        xed_close();
        return false;
    }
    return true;
}

void xed_close(void) {
    if (!T->xm)
        return;
    xm_stop();
    sleep_ms(2);
    xm_clear(T->xm);
    psram_free(T->xm);
    T->xm = NULL;
    xm_free(E.clip);
    E.clip = NULL;
    E.clip_w = 0;
    for (int i = 0; i < XM_MAX_CH; ++i)
        xm_mute(i, false);
}

bool xed_save(const char *path) {
    size_t n = xm_save_size(T->xm);
    uint8_t *data = psram_malloc(n);
    if (!data) {
        trk_message("Not enough memory to save.");
        return false;
    }
    xm_save(T->xm, data);
    bool ok = file_write_all(path, data, n);
    psram_free(data);
    if (ok)
        trk_message("Saved %s (%lu bytes).", path, (unsigned long)n);
    return ok;
}
