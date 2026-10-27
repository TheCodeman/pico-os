/**
 * @file trk.c
 * @brief The tracker's main loop and its pattern screen: song settings, the order list, the sample list, a scope and level meter for each channel, the transport buttons and the pattern editor.
 *
 * See trk.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "dvi.h"
#include "hdmi_audio.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "trk.h"
#include "xm.h"

trk_t *T;

// ---- Layout of the pattern screen (pixels) -------------------------------------------------------
#define TOP_Y      16   ///< the panels under the menu bar
#define TOP_H      144  ///< their height
#define SONG_X     0  ///< the song panel: left
#define SONG_W     236  ///< width
#define ORD_X      236  ///< the order list: left
#define ORD_W      112  ///< width
#define SMP_X      348  ///< the sample list: left
#define SMP_W      292  ///< width
#define SCOPE_Y    160  ///< the scopes' top
#define SCOPE_H    48  ///< their height
#define TRANS_Y    208  ///< the transport buttons' row
#define HEAD_Y     232  ///< the channel heads' row
#define PAT_Y      248  ///< the pattern's top
#define PAT_ROWS   15  ///< pattern rows shown
#define ROW_H      14  ///< a row's height
#define PAT_X      28   ///< the first channel
#define CH_W       76  ///< a channel's width
#define STATUS_Y   464  ///< the status line's top
#define ORD_ROWS   6  ///< order rows shown
#define SMP_ROWS   7  ///< sample rows shown

/// Field x offsets in a channel's column: note, sample (2 digits), effect, parameter (2).
static const uint8_t col_x[6] = {2, 30, 38, 50, 58, 66};

/// More commands (menus, keys) beside the HIT_ and TOOL_ ids.
enum {
    CMD_SAVE_AS = 200, CMD_BEGIN, CMD_END, CMD_MARK, CMD_UNMARK, CMD_CUT, CMD_COPY, CMD_PASTE,
    CMD_MIX, CMD_UP1, CMD_DOWN1, CMD_UP12, CMD_DOWN12, CMD_INS_ROW, CMD_DEL_ROW, CMD_WRITE_SPEED,
    CMD_WRITE_BPM, CMD_MUTE, CMD_SOLO, CMD_UNMUTE, CMD_ABOUT, CMD_ORDERS, CMD_SONG, CMD_UNDO,
    CMD_REDO, CMD_TO_XM,
};

void trk_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(T->msg, sizeof T->msg, fmt, ap);
    va_end(ap);
    T->msg_until = to_ms_since_boot(get_absolute_time()) + 4000;
}

void trk_dialog_begin(void) {
    // The dialogs (trk_ui.c) keep and put back what they cover themselves.
}

void trk_dialog_end(void) {
    T->full = true;
    mouse_flush();
}

void trk_stop(void) {
    mod_stop();
}

// ---- Notes from the keyboard -------------------------------------------------------------------------

int trk_note_key(int key) {
    // Two rows of piano keys, as in most trackers: Z S X D C V ... for an
    // octave from the base, and Q 2 W 3 E R ... for the one above.
    static const char lower[] = "zsxdcvgbhnjm,l.;/";
    static const char upper[] = "q2w3er5t6y7ui9o0p";
    if (key < 0x20 || key >= 0x7f)
        return -1;
    int c = key >= 'A' && key <= 'Z' ? key + 32 : key;
    const char *p;
    int n;
    if ((p = strchr(lower, c)))
        n = p - lower;
    else if ((p = strchr(upper, c)))
        n = 12 + (p - upper);
    else
        return -1;
    n += (T->octave - 1) * 12;
    return n <= 35 ? n : -1;
}

void trk_jam(int sample, int note) {
    if (sample >= 1 && sample <= 31 && note >= 0)
        mod_jam(&T->song, T->ch, sample, note);
}

// ---- The song ----------------------------------------------------------------------------------------------

/// Counts a pattern as used.
/// @param p the pattern
static void touch_pattern(int p) {
    if (p >= T->song.npatterns)
        T->song.npatterns = p + 1;
}

/// Goes to a song position.
/// @param pos the position
static void go_pos(int pos) {
    mod_song_t *s = &T->song;
    if (pos < 0)
        pos = 0;
    if (pos >= s->length)
        pos = s->length - 1;
    T->pos = pos;
    T->pattern = s->orders[pos];
    touch_pattern(T->pattern);
}

/// Changes the channel count (patterns kept).
/// @param n the channels
/// @return done
static bool set_channels(int n) {
    mod_song_t *s = &T->song;
    if (n == s->channels)
        return true;
    trk_stop();
    uint8_t *p = mod_alloc((size_t)MOD_MAX_PATTERNS * MOD_ROWS * n * 4);
    if (!p) {
        trk_message("Not enough memory.");
        return false;
    }
    memset(p, 0, (size_t)MOD_MAX_PATTERNS * MOD_ROWS * n * 4);
    int keep = n < s->channels ? n : s->channels;
    for (int pat = 0; pat < MOD_MAX_PATTERNS; ++pat)
        for (int r = 0; r < MOD_ROWS; ++r)
            memcpy(p + ((size_t)(pat * MOD_ROWS + r) * n) * 4, mod_cell_ptr(s, pat, r, 0),
                   keep * 4);
    mod_free(s->patterns);
    s->patterns = p;
    s->channels = n;
    strcpy(s->sig, n == 4 ? "M.K." : n == 6 ? "6CHN" : "8CHN");
    if (s->nsamples == 15 && n != 4)
        s->nsamples = 31;
    if (T->ch >= n)
        T->ch = n - 1;
    T->block = T->marking = false;
    T->dirty = true;
    T->full = true;
    return true;
}

/// Changes a song panel field by a step.
/// @param f the field (SF_*)
/// @param d the step
static void song_field_change(int f, int d) {
    mod_song_t *s = &T->song;
    switch (f) {
    case SF_BPM:
        T->bpm += d;
        T->bpm = T->bpm < 32 ? 32 : T->bpm > 255 ? 255 : T->bpm;
        mod_set_tempo(0, T->bpm);
        break;
    case SF_SPEED:
        T->speed += d > 0 ? 1 : -1;
        T->speed = T->speed < 1 ? 1 : T->speed > 31 ? 31 : T->speed;
        mod_set_tempo(T->speed, 0);
        break;
    case SF_OCTAVE:
        T->octave += d > 0 ? 1 : -1;
        T->octave = T->octave < 1 ? 1 : T->octave > 3 ? 3 : T->octave;
        break;
    case SF_STEP:
        T->step += d > 0 ? 1 : -1;
        T->step = T->step < 0 ? 0 : T->step > 16 ? 16 : T->step;
        break;
    case SF_CHANNELS: {
        int n = s->channels + (d > 0 ? 2 : -2);
        if (n >= 4 && n <= 8) {
            if (n < s->channels) {
                trk_dialog_begin();
                int a = gui_message("Channels", "Fewer channels: the last ones' notes are lost.",
                                    "OK|Cancel", false);
                trk_dialog_end();
                if (a != 0)
                    break;
            }
            set_channels(n);
        }
        break;
    }
    case SF_STEREO:
        T->separation += d > 0 ? 10 : -10;
        T->separation = T->separation < 0 ? 0 : T->separation > 100 ? 100 : T->separation;
        mod_set_separation(T->separation);
        break;
    case SF_RESTART:
        // Past the end is "off" (ProTracker writes 127): the song starts over.
        if (s->restart >= s->length)
            s->restart = d > 0 ? 0 : s->length - 1;
        else
            s->restart += d > 0 ? 1 : -1;
        if (s->restart < 0)
            s->restart = 127;
        T->dirty = true;
        break;
    }
}

/// Edits a song panel field by typing.
/// @param f the field
static void song_field_enter(int f) {
    mod_song_t *s = &T->song;
    char buf[24];
    trk_dialog_begin();
    if (f == SF_TITLE) {
        snprintf(buf, sizeof buf, "%.20s", s->title);
        if (gui_input("Song", "Title (20 letters):", buf, 21)) {
            memset(s->title, 0, sizeof s->title);
            memcpy(s->title, buf, strnlen(buf, 20));
            T->dirty = true;
        }
    } else if (f == SF_BPM || f == SF_SPEED) {
        snprintf(buf, sizeof buf, "%d", f == SF_BPM ? T->bpm : T->speed);
        if (gui_input("Song", f == SF_BPM ? "Tempo (BPM, 32-255):" : "Speed (ticks a row, 1-31):",
                      buf, 4)) {
            int v = atoi(buf);
            if (f == SF_BPM && v >= 32 && v <= 255)
                mod_set_tempo(0, T->bpm = v);
            if (f == SF_SPEED && v >= 1 && v <= 31)
                mod_set_tempo(T->speed = v, 0);
        }
    }
    trk_dialog_end();
}

// ---- Pattern editing ----------------------------------------------------------------------------------

/// A cell of the pattern on screen.
/// @param r the row
/// @param c the channel
/// @return the cell
static mod_cell_t get(int r, int c) {
    return mod_get(&T->song, T->pattern, r, c);
}

/// Sets a cell.
/// @param r the row
/// @param c the channel
/// @param v the cell
static void put(int r, int c, mod_cell_t v) {
    mod_put(&T->song, T->pattern, r, c, v);
    T->dirty = true;
}

/// Moves down by the step after entering a note.
static void advance(void) {
    T->row = (T->row + T->step) % MOD_ROWS;
}

/// The block's corners, in order.
/// @param c0 set to its first channel
/// @param r0 its first row
/// @param c1 its last channel
/// @param r1 its last row
static void block_range(int *c0, int *r0, int *c1, int *r1) {
    *c0 = T->bl_ch0 < T->bl_ch1 ? T->bl_ch0 : T->bl_ch1;
    *c1 = T->bl_ch0 < T->bl_ch1 ? T->bl_ch1 : T->bl_ch0;
    *r0 = T->bl_row0 < T->bl_row1 ? T->bl_row0 : T->bl_row1;
    *r1 = T->bl_row0 < T->bl_row1 ? T->bl_row1 : T->bl_row0;
}

/// Whether a cell is in the block.
/// @param r the row
/// @param c the channel
/// @return it is
static bool in_block(int r, int c) {
    if (!T->block)
        return false;
    int c0, r0, c1, r1;
    block_range(&c0, &r0, &c1, &r1);
    return r >= r0 && r <= r1 && c >= c0 && c <= c1;
}

/// The block, or the cursor's cell if none is marked.
/// @param c0 set to the first channel
/// @param r0 the first row
/// @param c1 the last channel
/// @param r1 the last row
static void work_range(int *c0, int *r0, int *c1, int *r1) {
    if (T->block) {
        block_range(c0, r0, c1, r1);
    } else {
        *c0 = *c1 = T->ch;
        *r0 = *r1 = T->row;
    }
}

/// Copies (or cuts) the block.
/// @param cut cut it
static void copy_block(bool cut) {
    int c0, r0, c1, r1;
    work_range(&c0, &r0, &c1, &r1);
    T->clip_w = c1 - c0 + 1;
    T->clip_h = r1 - r0 + 1;
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            T->clip[(r - r0) * T->clip_w + (c - c0)] = get(r, c);
            if (cut)
                put(r, c, (mod_cell_t){0});
        }
    trk_message("%s %d row%s x %d channel%s.", cut ? "Cut" : "Copied", T->clip_h,
                T->clip_h == 1 ? "" : "s", T->clip_w, T->clip_w == 1 ? "" : "s");
}

/// Pastes at the cursor.
/// @param mix only into empty cells
static void paste_block(bool mix) {
    if (!T->clip_w) {
        trk_message("Nothing copied yet (Alt-C copies the block).");
        return;
    }
    for (int r = 0; r < T->clip_h && T->row + r < MOD_ROWS; ++r)
        for (int c = 0; c < T->clip_w && T->ch + c < T->song.channels; ++c) {
            mod_cell_t v = T->clip[r * T->clip_w + c];
            if (mix) {
                mod_cell_t old = get(T->row + r, T->ch + c);
                if (old.period || old.sample) {
                    v.period = old.period;
                    v.sample = old.sample;
                }
                if (old.effect || old.param) {
                    v.effect = old.effect;
                    v.param = old.param;
                }
            }
            put(T->row + r, T->ch + c, v);
        }
}

/// Transposes the block's notes.
/// @param d semitones
static void transpose(int d) {
    int c0, r0, c1, r1, lost = 0;
    work_range(&c0, &r0, &c1, &r1);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            mod_cell_t v = get(r, c);
            if (!v.period)
                continue;
            int n = mod_period_note(v.period) + d;
            if (n < 0 || n > 35) {
                ++lost;
                continue;  // ProTracker's range ends there: left as it is
            }
            v.period = mod_note_period(n, 0);
            put(r, c, v);
        }
    if (lost)
        trk_message("%d note%s would leave C-1..B-3: not moved.", lost, lost == 1 ? "" : "s");
}

/// Inserts or deletes a row at the cursor.
/// @param c0 the first channel
/// @param c1 the last
/// @param insert insert (else delete)
static void shift_rows(int c0, int c1, bool insert) {
    for (int c = c0; c <= c1; ++c) {
        if (insert) {
            for (int r = MOD_ROWS - 1; r > T->row; --r)
                put(r, c, get(r - 1, c));
            put(T->row, c, (mod_cell_t){0});
        } else {
            for (int r = T->row; r < MOD_ROWS - 1; ++r)
                put(r, c, get(r + 1, c));
            put(MOD_ROWS - 1, c, (mod_cell_t){0});
        }
    }
}

/// A new pattern at the next position.
/// @param clone a copy of this one
static void new_pattern(bool clone) {
    mod_song_t *s = &T->song;
    if (s->npatterns >= MOD_MAX_PATTERNS || s->length >= 128) {
        trk_message("The song is full (%d patterns, 128 positions).", MOD_MAX_PATTERNS);
        return;
    }
    int p = s->npatterns;
    touch_pattern(p);
    size_t bytes = (size_t)MOD_ROWS * s->channels * 4;
    if (clone)
        memcpy(mod_cell_ptr(s, p, 0, 0), mod_cell_ptr(s, T->pattern, 0, 0), bytes);
    else
        memset(mod_cell_ptr(s, p, 0, 0), 0, bytes);
    memmove(s->orders + T->pos + 2, s->orders + T->pos + 1, 127 - (T->pos + 1));
    s->orders[T->pos + 1] = p;
    s->length++;
    go_pos(T->pos + 1);
    T->dirty = true;
    trk_message("Pattern %02d %s at position %02d.", p, clone ? "cloned" : "made", T->pos);
}

/// Clears the pattern (after asking).
static void clear_pattern(void) {
    trk_dialog_begin();
    char text[48];
    snprintf(text, sizeof text, "Clear all of pattern %02d?", T->pattern);
    int a = gui_message("Clear pattern", text, "Clear|Cancel", false);
    trk_dialog_end();
    if (a == 0) {
        memset(mod_cell_ptr(&T->song, T->pattern, 0, 0), 0, (size_t)MOD_ROWS * T->song.channels * 4);
        T->dirty = true;
    }
}

/// Writes Fxx: the speed or tempo, at the cursor.
/// @param bpm the tempo (else the speed)
static void write_tempo(bool bpm) {
    mod_cell_t v = get(T->row, T->ch);
    v.effect = 15;
    v.param = bpm ? T->bpm : T->speed;
    put(T->row, T->ch, v);
    trk_message("F%02X: %s %d from this row.", v.param, bpm ? "tempo" : "speed", v.param);
}

/// Inserts a position in the order list.
static void order_insert(void) {
    mod_song_t *s = &T->song;
    if (s->length >= 128)
        return;
    memmove(s->orders + T->pos + 1, s->orders + T->pos, 127 - T->pos);
    s->length++;
    T->dirty = true;
}

/// Deletes a position.
static void order_delete(void) {
    mod_song_t *s = &T->song;
    if (s->length <= 1)
        return;
    memmove(s->orders + T->pos, s->orders + T->pos + 1, 127 - T->pos);
    s->orders[127] = 0;
    s->length--;
    if (s->restart >= s->length)
        s->restart = 0;
    go_pos(T->pos);
    T->dirty = true;
}

/// Changes the position's pattern.
/// @param d the step
static void order_change(int d) {
    mod_song_t *s = &T->song;
    int p = s->orders[T->pos] + d;
    if (p < 0 || p >= MOD_MAX_PATTERNS)
        return;
    s->orders[T->pos] = p;
    go_pos(T->pos);
    T->dirty = true;
}

/// Mutes or solos a channel.
/// @param ch the channel
/// @param how 0 toggles, 1 solo, 2 all on
static void mute(int ch, int how) {  // how: 0 toggle, 1 solo, 2 all on
    mod_status_t st;
    mod_get_status(&st);
    if (how == 0) {
        mod_mute(ch, !st.muted[ch]);
        return;
    }
    bool solo = !st.muted[ch];
    for (int i = 0; i < T->song.channels; ++i)
        solo = solo && (i == ch || st.muted[i]);
    for (int i = 0; i < MOD_MAX_CH; ++i)
        mod_mute(i, how == 1 && !solo && i != ch);
}

// ---- Commands (menus, keys and buttons) ------------------------------------------------------------

/// Plays the song, the pattern or from the row.
/// @param how HIT_PLAY_SONG and the like
static void play(int how) {
    mod_set_tempo(T->speed, T->bpm);
    if (how == HIT_PLAY_SONG)
        mod_play(&T->song, T->pos, 0, false);
    else if (how == HIT_PLAY_PATTERN)
        mod_play_pattern(&T->song, T->pattern, 0);
    else
        mod_play(&T->song, T->pos, T->row, false);
}

/// Leaving.
static bool quit_wanted;
static void undo_key(bool redo);

/// Does a menu command or button.
/// @param id the command
/// @param arg its argument
static void command(int id, int arg) {
    if (T->xm) {
        static const int map[][2] = {
            {CMD_BEGIN, XED_BEGIN}, {CMD_END, XED_END}, {CMD_MARK, XED_MARK}, {CMD_UNMARK, XED_UNMARK},
            {CMD_CUT, XED_CUT}, {CMD_COPY, XED_COPY}, {CMD_PASTE, XED_PASTE}, {CMD_MIX, XED_MIX},
            {CMD_UP1, XED_UP1}, {CMD_DOWN1, XED_DOWN1}, {CMD_UP12, XED_UP12}, {CMD_DOWN12, XED_DOWN12},
            {CMD_INS_ROW, XED_INS_ROW}, {CMD_DEL_ROW, XED_DEL_ROW}, {CMD_MUTE, XED_MUTE},
            {CMD_SOLO, XED_SOLO}, {CMD_UNMUTE, XED_UNMUTE},
        };
        for (unsigned i = 0; i < sizeof map / sizeof *map; ++i)
            if (map[i][0] == id)
                id = map[i][1];
        if (xed_command(id))
            return;
        if (id >= TOOL_LOAD && id < CMD_SAVE_AS) {
            xmi_tool(id);
            return;
        }
    }
    if (id >= TOOL_LOAD && id < CMD_SAVE_AS) {
        smp_tool(id);
        return;
    }
    switch (id) {
    case HIT_PLAY_SONG:
    case HIT_PLAY_PATTERN:
    case HIT_PLAY_POS: play(id); break;
    case HIT_STOP: trk_stop(); break;
    case HIT_RECORD: T->record = !T->record; break;
    case HIT_FOLLOW: T->follow = !T->follow; break;
    case HIT_SCREEN_PATTERN: T->screen = SCREEN_PATTERN; T->full = true; break;
    case HIT_SCREEN_SAMPLE: T->screen = SCREEN_SAMPLE; T->full = true; T->wave_dirty = true; break;
    case HIT_HELP: help_show(); break;
    case HIT_NEW:
        if (song_check_saved("starting a new song")) {
            trk_dialog_begin();
            int kind = gui_message("New song", "A ProTracker MOD (4 channels) or a FastTracker XM (8)?",
                                   "MOD|XM|Cancel", false);
            trk_dialog_end();
            if (kind < 0 || kind == 2)
                break;
            trk_stop();
            xed_close();
            if (kind == 1) {
                if (!xed_new(8))
                    trk_message("Not enough memory.");
                T->path[0] = 0;
                T->dirty = false;
                break;
            }
            mod_clear(&T->song);
            if (!mod_new(&T->song, 4))
                trk_message("Not enough memory.");
            undo_reset();
            T->path[0] = 0;
            T->dirty = false;
            T->row = T->ch = T->col = 0;
            go_pos(0);
            T->full = true;
        }
        break;
    case HIT_LOAD: {
        if (!song_check_saved("loading another song"))
            break;
        char path[256];
        trk_dialog_begin();
        bool picked = file_pick("Load a song", ".MOD.XM.", false, path, sizeof path);
        trk_dialog_end();
        if (picked)
            song_load(path);
        break;
    }
    case HIT_SAVE:
        if (T->path[0])
            song_save(T->path);
        else
            song_save_as();
        break;
    case CMD_SAVE_AS: song_save_as(); break;
    case HIT_QUIT:
        if (song_check_saved("leaving"))
            quit_wanted = true;
        break;
    case HIT_PAT_NEW: new_pattern(false); break;
    case HIT_PAT_CLONE: new_pattern(true); break;
    case HIT_PAT_CLEAR: clear_pattern(); break;
    case HIT_PATTERN_UP: order_change(1); break;
    case HIT_PATTERN_DOWN: order_change(-1); break;
    case HIT_ORDER_INS: order_insert(); break;
    case HIT_ORDER_DEL: order_delete(); break;
    case HIT_ORDER_INC: order_change(1); break;
    case HIT_ORDER_DEC: order_change(-1); break;
    case HIT_ORDER_UP: go_pos(T->pos - 1); break;
    case HIT_ORDER_DOWN: go_pos(T->pos + 1); break;
    case HIT_ORDER: go_pos(arg); T->focus = FOCUS_ORDERS; break;
    case HIT_SAMPLE: T->sample = arg; T->wave_dirty = true; break;
    case HIT_SAMPLE_UP: if (T->sample > 1) T->sample--; T->wave_dirty = true; break;
    case HIT_SAMPLE_DOWN: if (T->sample < 31) T->sample++; T->wave_dirty = true; break;
    case HIT_SCOPE: mute(arg, 0); break;
    case HIT_SONG_FIELD: T->focus = FOCUS_SONG; T->song_field = arg; song_field_enter(arg); break;
    case HIT_SONG_DEC: song_field_change(arg, -1); break;
    case HIT_SONG_INC: song_field_change(arg, 1); break;
    case CMD_BEGIN:
        T->block = T->marking = true;
        T->bl_ch0 = T->bl_ch1 = T->ch;
        T->bl_row0 = T->bl_row1 = T->row;
        break;
    case CMD_END:
        if (!T->block) {
            T->block = true;
            T->bl_ch0 = T->ch;
            T->bl_row0 = T->row;
        }
        T->bl_ch1 = T->ch;
        T->bl_row1 = T->row;
        T->marking = false;
        break;
    case CMD_MARK: {
        // The channel; again: the whole pattern.
        bool channel = T->block && T->bl_ch0 == T->ch && T->bl_ch1 == T->ch &&
                       ((T->bl_row0 == 0 && T->bl_row1 == 63) || (T->bl_row0 == 63 && T->bl_row1 == 0));
        T->block = true;
        T->marking = false;
        T->bl_row0 = 0;
        T->bl_row1 = 63;
        T->bl_ch0 = channel ? 0 : T->ch;
        T->bl_ch1 = channel ? T->song.channels - 1 : T->ch;
        break;
    }
    case CMD_UNMARK: T->block = T->marking = false; break;
    case CMD_CUT: copy_block(true); break;
    case CMD_COPY: copy_block(false); break;
    case CMD_PASTE: paste_block(false); break;
    case CMD_MIX: paste_block(true); break;
    case CMD_UP1: transpose(1); break;
    case CMD_DOWN1: transpose(-1); break;
    case CMD_UP12: transpose(12); break;
    case CMD_DOWN12: transpose(-12); break;
    case CMD_INS_ROW: shift_rows(0, T->song.channels - 1, true); break;
    case CMD_DEL_ROW: shift_rows(0, T->song.channels - 1, false); break;
    case CMD_WRITE_SPEED: write_tempo(false); break;
    case CMD_WRITE_BPM: write_tempo(true); break;
    case CMD_MUTE: mute(T->ch, 0); break;
    case CMD_SOLO: mute(T->ch, 1); break;
    case CMD_UNMUTE: mute(T->ch, 2); break;
    case CMD_ORDERS: T->screen = SCREEN_PATTERN; T->focus = FOCUS_ORDERS; T->full = true; break;
    case CMD_SONG: T->screen = SCREEN_PATTERN; T->focus = FOCUS_SONG; T->full = true; break;
    case CMD_ABOUT:
        trk_dialog_begin();
        gui_message("About",
                    "Pico Tracker: a ProTracker-style MOD editor.\n"
                    "4, 6 or 8 channels; 31 samples; all effects.\n"
                    "Mixed at 48 kHz on core 1, out through HDMI.",
                    "OK", false);
        trk_dialog_end();
        break;
    case HIT_MENU: trk_menu_open(arg); break;
    case CMD_UNDO: undo_key(false); break;
    case CMD_REDO: undo_key(true); break;
    case CMD_TO_XM:
        if (song_to_xm())
            trk_message("Converted to XM: Save writes a .XM file now.");
        break;
    }
}

// ---- Menus ---------------------------------------------------------------------------------------------

/// The File menu.
static menu_item_t file_items[] = {
    {"New song", HIT_NEW, "Ctrl-N", false},
    {"Load song...", HIT_LOAD, "Ctrl-O", false},
    {"Save", HIT_SAVE, "Ctrl-S", false},
    {"Save as...", CMD_SAVE_AS, "", false},
    {"Convert to XM", CMD_TO_XM, "", false},
    {"-", 0, NULL, false},
    {"Quit", HIT_QUIT, "Ctrl-Q", false},
    {NULL, 0, NULL, false},
};
/// The Edit menu.
static menu_item_t edit_items[] = {
    {"Undo", CMD_UNDO, "Ctrl-Z", false},
    {"Redo", CMD_REDO, "Ctrl-Y", false},
    {"-", 0, NULL, false},
    {"Begin block", CMD_BEGIN, "Alt-B", false},
    {"End block", CMD_END, "Alt-E", false},
    {"Mark channel/pattern", CMD_MARK, "Alt-L", false},
    {"Unmark", CMD_UNMARK, "Alt-U", false},
    {"-", 0, NULL, false},
    {"Cut", CMD_CUT, "Alt-X", false},
    {"Copy", CMD_COPY, "Alt-C", false},
    {"Paste", CMD_PASTE, "Alt-V", false},
    {"Mix paste", CMD_MIX, "Alt-M", false},
    {"-", 0, NULL, false},
    {"Transpose up", CMD_UP1, "Alt-Q", false},
    {"Transpose down", CMD_DOWN1, "Alt-A", false},
    {"Octave up", CMD_UP12, "Alt-W", false},
    {"Octave down", CMD_DOWN12, "Alt-S", false},
    {"-", 0, NULL, false},
    {"Insert row (all)", CMD_INS_ROW, "Alt-I", false},
    {"Delete row (all)", CMD_DEL_ROW, "Alt-D", false},
    {NULL, 0, NULL, false},
};
/// The Pattern menu.
static menu_item_t pattern_items[] = {
    {"New pattern", HIT_PAT_NEW, "Alt-N", false},
    {"Clone pattern", HIT_PAT_CLONE, "Alt-K", false},
    {"Clear pattern", HIT_PAT_CLEAR, "Alt-R", false},
    {"-", 0, NULL, false},
    {"Order list", CMD_ORDERS, "F4", false},
    {"Song settings", CMD_SONG, "F4 F4", false},
    {"Insert position", HIT_ORDER_INS, "", false},
    {"Delete position", HIT_ORDER_DEL, "", false},
    {"-", 0, NULL, false},
    {"Write speed here", CMD_WRITE_SPEED, "Alt-F", false},
    {"Write tempo here", CMD_WRITE_BPM, "Alt-G", false},
    {NULL, 0, NULL, false},
};
/// The Sample menu.
static menu_item_t sample_items[] = {
    {"Sample editor", HIT_SCREEN_SAMPLE, "F3", false},
    {"Load sample...", TOOL_LOAD, "Ctrl-L", false},
    {"Save sample...", TOOL_SAVE, "Ctrl-W", false},
    {"Name...", TOOL_NAME, "", false},
    {"Copy to...", TOOL_COPY_TO, "", false},
    {"Clear", TOOL_CLEAR, "", false},
    {"-", 0, NULL, false},
    {"Generate...", TOOL_GENERATE, "", false},
    {"Normalize", TOOL_NORMALIZE, "", false},
    {"Amplify...", TOOL_AMPLIFY, "", false},
    {"Reverse", TOOL_REVERSE, "", false},
    {"Fade in", TOOL_FADE_IN, "", false},
    {"Fade out", TOOL_FADE_OUT, "", false},
    {"Remove DC offset", TOOL_CENTRE, "", false},
    {"Halve rate", TOOL_HALVE, "", false},
    {"Double rate", TOOL_DOUBLE, "", false},
    {"-", 0, NULL, false},
    {"Select all", TOOL_SEL_ALL, "Alt-L", false},
    {"Crop to selection", TOOL_CROP, "", false},
    {"Cut selection", TOOL_CUT, "", false},
    {"Silence selection", TOOL_SILENCE, "", false},
    {"Loop the selection", TOOL_LOOP_SEL, "", false},
    {NULL, 0, NULL, false},
};
/// The Sample menu with an XM song: its instruments.
static menu_item_t instrument_items[] = {
    {"Instrument editor", HIT_SCREEN_SAMPLE, "F3", false},
    {"Load sample (WAV)...", TOOL_LOAD, "Ctrl-L", false},
    {"Save sample (WAV)...", TOOL_SAVE, "Ctrl-W", false},
    {"Copy sample to...", TOOL_COPY_TO, "", false},
    {"Delete sample", TOOL_CLEAR, "", false},
    {"-", 0, NULL, false},
    {"Generate...", TOOL_GENERATE, "", false},
    {"Normalize", TOOL_NORMALIZE, "", false},
    {"Amplify...", TOOL_AMPLIFY, "", false},
    {"Reverse", TOOL_REVERSE, "", false},
    {"Fade in", TOOL_FADE_IN, "", false},
    {"Fade out", TOOL_FADE_OUT, "", false},
    {"Remove DC offset", TOOL_CENTRE, "", false},
    {"Halve rate", TOOL_HALVE, "", false},
    {"Double rate", TOOL_DOUBLE, "", false},
    {"Select all", TOOL_SEL_ALL, "Alt-L", false},
    {"Crop to selection", TOOL_CROP, "Alt-K", false},
    {"Cut selection", TOOL_CUT, "Alt-X", false},
    {"Silence selection", TOOL_SILENCE, "", false},
    {"Loop the selection", TOOL_LOOP_SEL, "", false},
    {"-", 0, NULL, false},
    {"Load instrument (XI)...", TOOL_XI_LOAD, "", false},
    {"Save instrument (XI)...", TOOL_XI_SAVE, "", false},
    {"Copy instrument to...", TOOL_INS_COPY, "", false},
    {"Swap instrument with...", TOOL_INS_SWAP, "", false},
    {"Clear instrument", TOOL_INS_CLEAR, "", false},
    {"Envelope preset...", TOOL_ENV_PRESET, "", false},
    {NULL, 0, NULL, false},
};
/// The Play menu.
static menu_item_t play_items[] = {
    {"Play song", HIT_PLAY_SONG, "F5", false},
    {"Play pattern", HIT_PLAY_PATTERN, "F6", false},
    {"Play from cursor", HIT_PLAY_POS, "F7", false},
    {"Stop", HIT_STOP, "F8", false},
    {"-", 0, NULL, false},
    {"Record (edit) mode", HIT_RECORD, "Space", false},
    {"Follow the song", HIT_FOLLOW, "Ctrl-F", false},
    {"-", 0, NULL, false},
    {"Mute channel", CMD_MUTE, "Ctrl-T", false},
    {"Solo channel", CMD_SOLO, "Ctrl-L", false},
    {"Unmute all", CMD_UNMUTE, "Ctrl-U", false},
    {NULL, 0, NULL, false},
};
/// The Help menu.
static menu_item_t help_items[] = {
    {"Keys", HIT_HELP, "F1", false},
    {"About", CMD_ABOUT, "", false},
    {NULL, 0, NULL, false},
};
/// The menus.
static menu_t menus[] = {
    {"File", file_items},       {"Edit", edit_items}, {"Pattern", pattern_items},
    {"Sample", sample_items},   {"Play", play_items}, {"Help", help_items},
};
/// Their count.
#define NMENUS 6

/// What can't be done now is greyed.
static void menu_states(void) {
    play_items[5].check = T->record;
    play_items[6].check = T->follow;
    edit_items[0].text = undo_can(false) ? "Undo" : "~Undo";
    edit_items[1].text = undo_can(true) ? "Redo" : "~Redo";
    file_items[4].text = T->xm ? "~Convert to XM" : "Convert to XM";
    menus[3].title = T->xm ? "Instrument" : "Sample";
    menus[3].items = T->xm ? instrument_items : sample_items;
}

void trk_menu_open(int open) {
    menu_states();
    trk_dialog_begin();
    int id = gui_menu(menus, NMENUS, open);
    trk_dialog_end();
    if (id >= 0)
        command(id, 0);
}

/// The right button: the Edit menu over the pattern, the Sample menu over
/// the sample screen, where the pointer is.
/// @param x the pointer's x
/// @param y its y
static void context_menu(int x, int y) {
    menu_states();
    int id = gui_popup(T->screen != SCREEN_SAMPLE ? edit_items : T->xm ? instrument_items : sample_items, x, y);
    trk_dialog_end();
    if (id >= 0)
        command(id, 0);
}

// ---- Drawing the pattern screen -------------------------------------------------------------------

/// Draws the menu bar (when it changed).
static void draw_menu_bar(void) {
    // As the menus draw it when open: two spaces, the title, two spaces.
    // Drawn only when something on it changed: drawing it again each frame
    // made its lines and labels flicker.
    if (T->full)
        menu_states();
    int x = 16;
    for (int i = 0; i < NMENUS; ++i) {
        int w = (strlen(menus[i].title) + 4) * 8;
        hit_add(x, 0, w, 16, HIT_MENU, i);
        x += w;
    }
    char name[40];
    const char *base = strrchr(T->path, '/');
    snprintf(name, sizeof name, "%s%s  Pico Tracker", base ? base + 1 : T->path[0] ? T->path : "(new)",
             T->dirty ? "*" : "");
    // Cut from the left to fit after the menus (the tracker's name goes first).
    int n = strlen(name), room = (DVI_WIDTH - x - 16) / 8;
    if (n > room && strstr(name, "  Pico Tracker")) {
        *strstr(name, "  Pico Tracker") = 0;
        n = strlen(name);
    }
    const char *shown = n > room ? name + (n - room) : name;
    static char last[40];
    if (!T->full && !strcmp(last, shown))
        return;
    snprintf(last, sizeof last, "%s", shown);
    n = strlen(shown);
    gfx_fill(0, 0, DVI_WIDTH, 16, C_FACE);
    x = 16;
    for (int i = 0; i < NMENUS; ++i) {
        gfx_text(x + 16, 0, menus[i].title, -1, C_TEXT, C_FACE);
        x += (strlen(menus[i].title) + 4) * 8;
    }
    gfx_text(DVI_WIDTH - 8 - n * 8, 0, shown, n, C_YELLOW, C_FACE);
    // Raised: lit along the top, shaded along the bottom; the menus' key letters.
    gfx_hline(0, 0, DVI_WIDTH, C_LIGHT);
    gfx_hline(0, 15, DVI_WIDTH, C_DARK);
    for (int i = 0, mx = 16; i < NMENUS; mx += (strlen(menus[i].title) + 4) * 8, ++i)
        gfx_hline(mx + 16, 14, 8, C_LABEL);
}

/// A field: text in a sunken box.
/// @param x left
/// @param y top
/// @param chars its width
/// @param focus it has the keyboard
/// @param fg the text's colour
/// @param text the text
static void field_box(int x, int y, int chars, bool focus, uint8_t fg, const char *text) {
    uint8_t bg = focus ? C_FOCUS : C_FIELD;
    gfx_fill(x, y, 2, 16, bg);
    int end = gfx_text(x + 2, y, text, chars, fg, bg);
    gfx_fill(end, y, 2, 16, bg);
}

/// Draws the song panel.
/// @param st the player's status
static void draw_song_panel(mod_status_t *st) {
    static const char *const labels[SF_COUNT] = {"Title", "Tempo", "Speed", "Octave",
                                                 "Step", "Chans", "Stereo", "Restart"};
    mod_song_t *s = &T->song;
    if (T->full)
        gfx_bevel(SONG_X, TOP_Y, SONG_W, TOP_H, true, C_FACE);
    bool playing = st->state != MOD_STOPPED;
    for (int f = 0; f < SF_COUNT; ++f) {
        int y = TOP_Y + 8 + f * 16;
        bool focus = T->focus == FOCUS_SONG && T->song_field == f;
        gfx_text(SONG_X + 6, y, labels[f], 7, focus ? C_YELLOW : C_LABEL, C_FACE);
        char v[24];
        switch (f) {
        case SF_TITLE: snprintf(v, sizeof v, "%.20s", s->title); break;
        case SF_BPM: snprintf(v, sizeof v, "%d", playing ? st->tempo : T->bpm); break;
        case SF_SPEED: snprintf(v, sizeof v, "%d", playing ? st->speed : T->speed); break;
        case SF_OCTAVE: snprintf(v, sizeof v, "%d", T->octave); break;
        case SF_STEP: snprintf(v, sizeof v, "%d", T->step); break;
        case SF_CHANNELS: snprintf(v, sizeof v, "%d", s->channels); break;
        case SF_STEREO: snprintf(v, sizeof v, "%d%%", T->separation); break;
        case SF_RESTART:
            snprintf(v, sizeof v, s->restart < s->length ? "%02d" : "off", s->restart);
            break;
        }
        if (f == SF_TITLE) {
            field_box(SONG_X + 64, y, 20, focus, C_TEXT, v);
            hit_add(SONG_X + 64, y, 164, 16, HIT_SONG_FIELD, f);
            continue;
        }
        field_box(SONG_X + 64, y, 5, focus, C_TEXT, v);
        hit_add(SONG_X + 64, y, 44, 16, HIT_SONG_FIELD, f);
        gfx_button(SONG_X + 112, y, 18, 16, "-", HIT_SONG_DEC, f, false);
        gfx_button(SONG_X + 132, y, 18, 16, "+", HIT_SONG_INC, f, false);
        if (f == SF_OCTAVE) {
            char keys[12];
            snprintf(keys, sizeof keys, "Z=C-%d", T->octave);
            gfx_text(SONG_X + 156, y, keys, 9, C_DIM, C_FACE);
        } else if (f == SF_CHANNELS) {
            gfx_text(SONG_X + 156, y, s->sig[0] ? s->sig : "15 smp", 9, C_DIM, C_FACE);
        } else if (f == SF_BPM) {
            gfx_text(SONG_X + 156, y, playing ? "playing" : "", 9, C_DIM, C_FACE);
        }
    }
}

/// Draws the order list.
static void draw_orders(void) {
    mod_song_t *s = &T->song;
    if (T->full)
        gfx_bevel(ORD_X, TOP_Y, ORD_W, TOP_H, true, C_FACE);
    if (T->pos < T->order_top)
        T->order_top = T->pos;
    if (T->pos >= T->order_top + ORD_ROWS)
        T->order_top = T->pos - ORD_ROWS + 1;
    gfx_printf(ORD_X + 6, TOP_Y + 8, 12, C_LABEL, C_FACE, "Pos Pat  %3d", s->length);
    for (int i = 0; i < ORD_ROWS; ++i) {
        int p = T->order_top + i, y = TOP_Y + 24 + i * 16;
        bool cur = p == T->pos;
        uint8_t bg = cur ? (T->focus == FOCUS_ORDERS ? C_FOCUS : C_CURROW) : C_FIELD;
        if (p < s->length) {
            gfx_printf(ORD_X + 6, y, 12, cur ? C_YELLOW : C_TEXT, bg, "%02d  %02d %s", p, s->orders[p],
                       p == s->restart ? "<" : "");
            hit_add(ORD_X + 6, y, 96, 16, HIT_ORDER, p);
        } else {
            gfx_text(ORD_X + 6, y, "", 12, C_TEXT, C_FIELD);
            hit_add(ORD_X + 6, y, 96, 16, HIT_NONE, 0);
        }
    }
    int y = TOP_Y + TOP_H - 22;
    gfx_button(ORD_X + 4, y, 26, 18, "In", HIT_ORDER_INS, 0, false);
    gfx_button(ORD_X + 30, y, 26, 18, "De", HIT_ORDER_DEL, 0, false);
    gfx_button(ORD_X + 56, y, 26, 18, "-", HIT_ORDER_DEC, 0, false);
    gfx_button(ORD_X + 82, y, 26, 18, "+", HIT_ORDER_INC, 0, false);
}

/// Draws the sample list.
static void draw_sample_list(void) {
    mod_song_t *s = &T->song;
    if (T->full)
        gfx_bevel(SMP_X, TOP_Y, SMP_W, TOP_H, true, C_FACE);
    if (T->sample < T->smp_top + 1)
        T->smp_top = T->sample - 1;
    if (T->sample > T->smp_top + SMP_ROWS)
        T->smp_top = T->sample - SMP_ROWS;
    for (int i = 0; i < SMP_ROWS; ++i) {
        int n = T->smp_top + 1 + i, y = TOP_Y + 8 + i * 16;
        mod_sample_t *m = &s->samples[n - 1];
        bool cur = n == T->sample;
        uint8_t bg = cur ? C_CURROW : C_FIELD;
        char name[23];
        memcpy(name, m->name, 22);
        name[22] = 0;
        for (int k = 0; k < 22; ++k)
            if (name[k] && (name[k] < 0x20 || name[k] > 0x7e))
                name[k] = '.';
        gfx_printf(SMP_X + 6, y, 3, C_YELLOW, bg, "%02d", n);
        gfx_text(SMP_X + 30, y, name, 22, m->length ? C_TEXT : C_DIM, bg);
        gfx_printf(SMP_X + 206, y, 9, C_CYAN, bg, m->length ? " %2d %5lu" : "", m->volume,
                   (unsigned long)m->length);
        hit_add(SMP_X + 6, y, 272, 16, HIT_SAMPLE, n);
    }
    int y = TOP_Y + TOP_H - 22;
    gfx_button(SMP_X + 4, y, 34, 18, "Up", HIT_SAMPLE_UP, 0, false);
    gfx_button(SMP_X + 38, y, 34, 18, "Dn", HIT_SAMPLE_DOWN, 0, false);
    gfx_button(SMP_X + 76, y, 66, 18, "Load", TOOL_LOAD, 0, false);
    gfx_button(SMP_X + 142, y, 66, 18, "Save", TOOL_SAVE, 0, false);
    gfx_button(SMP_X + 212, y, 76, 18, "Edit F3", HIT_SCREEN_SAMPLE, 0, false);
}

/// Draws the scopes.
/// @param st the player's status
static void draw_scopes(mod_status_t *st) {
    int n = T->song.channels, w = DVI_WIDTH / n;
    int8_t data[160];
    for (int c = 0; c < n; ++c) {
        int x = c * w;
        if (T->full)
            gfx_bevel(x, SCOPE_Y, w, SCOPE_H, false, C_BG);
        int iw = w - 2, top = SCOPE_Y + 1, mid = top + 20;
        mod_scope(c, data, iw);
        bool muted = st->muted[c];
        // Column by column, each pixel once: the trace joins each value to the last.
        int prev = mid - data[0] * 19 / 128;
        for (int i = 0; i < iw; ++i) {
            int yv = muted ? mid : mid - data[i] * 19 / 128;
            int lo = yv < prev ? yv : prev, hi = yv < prev ? prev : yv;
            prev = yv;
            uint8_t *d = dvi_framebuf + top * DVI_WIDTH + x + 1 + i;
            for (int r = 0; r < 40; ++r, d += DVI_WIDTH) {
                if (i < 18 && r < 16)
                    continue;  // the label
                int yy = top + r;
                *d = yy >= lo && yy <= hi ? (muted ? C_DIM : C_GREEN) : yy == mid ? C_DARK : C_BG;
            }
        }
        // The level, along the bottom.
        int len = st->level[c] * (iw - 4) / 64;
        for (int i = 0; i < iw - 4; ++i) {
            uint8_t col = i >= len ? C_DARK : i < iw * 6 / 10 ? C_GREEN : i < iw * 85 / 100 ? C_YELLOW : C_RED;
            gfx_vline(x + 3 + i, top + 41, 4, col);
        }
        char label[3] = {(char)('1' + c), 0};
        gfx_text(x + 3, top, label, 2, muted ? C_RED : C_YELLOW, C_BG);
        hit_add(x, SCOPE_Y, w, SCOPE_H, HIT_SCOPE, c);
    }
}

/// Draws the transport buttons.
/// @param st the player's status
static void draw_transport(mod_status_t *st) {
    static const struct {
        const char *label;
        int id;
    } b[] = {
        {"Song F5", HIT_PLAY_SONG}, {"Patt F6", HIT_PLAY_PATTERN}, {"Stop F8", HIT_STOP},
        {"Rec", HIT_RECORD},        {"Follow", HIT_FOLLOW},        {"NewPat", HIT_PAT_NEW},
        {"Clone", HIT_PAT_CLONE},   {"Help F1", HIT_HELP},
    };
    if (T->full)
        gfx_fill(0, TRANS_Y, DVI_WIDTH, 24, C_FACE);
    for (int i = 0; i < 8; ++i) {
        bool lit = (b[i].id == HIT_PLAY_SONG && st->state == MOD_SONG) ||
                   (b[i].id == HIT_PLAY_PATTERN && st->state == MOD_PATTERN) ||
                   (b[i].id == HIT_RECORD && T->record) || (b[i].id == HIT_FOLLOW && T->follow);
        gfx_button(2 + i * 66, TRANS_Y + 2, 64, 20, b[i].label, b[i].id, 0, lit);
    }
    gfx_printf(2 + 8 * 66, TRANS_Y + 4, 13, T->load > 700 ? C_RED : C_LABEL, C_FACE, "CPU%3u.%u%% %s",
               T->load / 10, T->load % 10, T->underruns ? "!" : "");
}

/// Draws the pattern.
/// @param st the player's status
static void draw_pattern(mod_status_t *st) {
    mod_song_t *s = &T->song;
    int nch = s->channels;
    // The header: the pattern, and each channel (click: mute).
    gfx_printf(0, HEAD_Y, 3, C_YELLOW, C_FACE, "%02d", T->pattern);
    for (int c = 0; c < nch; ++c) {
        int x = PAT_X + c * CH_W;
        gfx_printf(x, HEAD_Y, 9, st->muted[c] ? C_RED : C_LABEL, C_FACE, " Ch %d %s", c + 1,
                   st->muted[c] ? "mute" : "");
        gfx_vline(x + CH_W - 1, HEAD_Y, 16, C_DARK);
        hit_add(x, HEAD_Y, CH_W, 16, HIT_SCOPE, c);
    }
    gfx_fill(PAT_X + nch * CH_W, HEAD_Y, DVI_WIDTH - PAT_X - nch * CH_W, 16, C_FACE);

    bool here = st->state != MOD_STOPPED && st->pattern == T->pattern;
    // Following the song: the row playing in the middle. Otherwise the view
    // moves only when the cursor would leave it (so a click doesn't scroll).
    if (here && T->follow)
        T->pat_top = T->row - PAT_ROWS / 2;
    else if (T->row < T->pat_top + 1)
        T->pat_top = T->row - 1;
    else if (T->row > T->pat_top + PAT_ROWS - 2)
        T->pat_top = T->row - PAT_ROWS + 2;
    if (T->pat_top < -PAT_ROWS / 2)
        T->pat_top = -PAT_ROWS / 2;
    if (T->pat_top > MOD_ROWS - PAT_ROWS / 2 - 1)
        T->pat_top = MOD_ROWS - PAT_ROWS / 2 - 1;
    for (int i = 0; i < PAT_ROWS; ++i) {
        int r = T->pat_top + i, y = PAT_Y + i * ROW_H;
        if (r < 0 || r >= MOD_ROWS) {
            gfx_fill(0, y, DVI_WIDTH, ROW_H, C_DARK);
            continue;
        }
        uint8_t bg = r == T->row ? C_CURROW
                     : here && r == st->row ? C_PLAYROW
                     : r % 16 == 0 ? C_ROW16
                     : r % 4 == 0 ? C_ROW4 : C_BG;
        if (here && r == st->row && r == T->row)
            bg = C_PLAYROW;
        char num[4];
        snprintf(num, sizeof num, "%02d", r);
        gfx_fill(0, y, 2, ROW_H, bg);
        gfx_text14(2, y, num, 2, r % 4 ? C_DIM : C_YELLOW, bg);
        gfx_fill(18, y, PAT_X - 18, ROW_H, bg);
        for (int c = 0; c < nch; ++c) {
            int x = PAT_X + c * CH_W;
            uint8_t cb = in_block(r, c) ? C_BLOCK : bg;
            mod_cell_t v = get(r, c);
            char note[4], smp[3], fx[4];
            int n = mod_period_note(v.period);
            if (!v.period)
                strcpy(note, "...");
            else if (n >= 0 && mod_note_period(n, 0) == v.period)
                snprintf(note, sizeof note, "%s", mod_note_name(n));
            else
                snprintf(note, sizeof note, "%03X", v.period);  // not a note of the table
            if (v.sample)
                snprintf(smp, sizeof smp, "%02X", v.sample);
            else
                strcpy(smp, "..");
            if (v.effect || v.param)
                snprintf(fx, sizeof fx, "%X%02X", v.effect, v.param);
            else
                strcpy(fx, "...");
            // The gaps, then the fields: every pixel once.
            gfx_fill(x, y, 2, ROW_H, cb);
            gfx_fill(x + 26, y, 4, ROW_H, cb);
            gfx_fill(x + 46, y, 4, ROW_H, cb);
            gfx_fill(x + 74, y, 1, ROW_H, cb);
            gfx_vline(x + 75, y, ROW_H, C_FACE);
            bool cursor_cell = r == T->row && c == T->ch && T->focus == FOCUS_PATTERN &&
                               T->screen == SCREEN_PATTERN;
            const char *texts[6] = {note, smp, smp + 1, fx, fx + 1, fx + 2};
            int lens[6] = {3, 1, 1, 1, 1, 1};
            uint8_t fgs[6] = {v.period ? C_TEXT : C_DIM, v.sample ? C_CYAN : C_DIM,
                              v.sample ? C_CYAN : C_DIM, v.effect || v.param ? C_ORANGE : C_DIM,
                              v.effect || v.param ? C_ORANGE : C_DIM, v.effect || v.param ? C_ORANGE : C_DIM};
            for (int k = 0; k < 6; ++k) {
                bool on = cursor_cell && T->col == k;
                gfx_text14(x + col_x[k], y, texts[k], lens[k], on ? C_BG : fgs[k], on ? C_CURSOR : cb);
            }
        }
        gfx_fill(PAT_X + nch * CH_W, y, DVI_WIDTH - PAT_X - nch * CH_W, ROW_H, C_DARK);
        hit_add(0, y, DVI_WIDTH, ROW_H, HIT_PATTERN, r);
    }
    gfx_fill(0, PAT_Y + PAT_ROWS * ROW_H, DVI_WIDTH, STATUS_Y - PAT_Y - PAT_ROWS * ROW_H, C_FACE);
}

/// Draws the status line.
/// @param st the player's status
static void draw_status(mod_status_t *st) {
    char line[81];
    if (T->msg[0] && to_ms_since_boot(get_absolute_time()) < T->msg_until) {
        snprintf(line, sizeof line, " %s", T->msg);
    } else {
        T->msg[0] = 0;
        if (st->state != MOD_STOPPED)
            snprintf(line, sizeof line, " Playing: position %02d pattern %02d row %02d   %s",
                     st->order, st->pattern, st->row, T->record ? "RECORDING" : "");
        else
            snprintf(line, sizeof line, " %s  Sample %02d  Octave %d  Step %d   F1 keys  F9 menu",
                     T->record ? "RECORD" : "Edit off", T->sample, T->octave, T->step);
    }
    gfx_text(0, STATUS_Y, line, 80, T->msg[0] ? C_YELLOW : C_LABEL, C_FACE);
}

/// Draws what changed.
static void draw(void) {
    mod_status_t st;
    mod_get_status(&st);
    unsigned load = audio_source_load();
    T->load = (T->load * 3 + load) / 4;
    T->underruns = hdmi_audio_underruns - T->underruns0;
    if (T->follow && st.state != MOD_STOPPED) {
        if (st.state == MOD_SONG && st.order != T->pos && st.order < T->song.length)
            go_pos(st.order);
        if (st.pattern == T->pattern)
            T->row = st.row;
    }
    mouse_pointer_hide();
    if (T->full) {
        hit_clear();
        gfx_fill(0, 0, DVI_WIDTH, DVI_HEIGHT, C_FACE);
    }
    draw_menu_bar();
    if (T->xm) {
        if (T->screen == SCREEN_SAMPLE)
            xmi_draw();
        else
            xed_draw();
        T->full = false;
        mouse_pointer_show();
        return;
    }
    if (T->screen == SCREEN_SAMPLE) {
        smp_draw();
    } else {
        draw_song_panel(&st);
        draw_orders();
        draw_sample_list();
        draw_scopes(&st);
        draw_transport(&st);
        draw_pattern(&st);
    }
    draw_status(&st);
    T->full = false;
    mouse_pointer_show();
}

// ---- Keys --------------------------------------------------------------------------------------------------

/// A hex digit's value.
/// @param k the key
/// @return 0-15, or -1
static int hex_digit(int k) {
    if (k >= '0' && k <= '9')
        return k - '0';
    if (k >= 'a' && k <= 'f')
        return k - 'a' + 10;
    if (k >= 'A' && k <= 'F')
        return k - 'A' + 10;
    return -1;
}

/// Moves the cursor.
/// @param dr rows
/// @param dc columns (note, sample, effect)
static void move_cursor(int dr, int dc) {
    if (dr) {
        T->row = (T->row + dr) % MOD_ROWS;
        if (T->row < 0)
            T->row += MOD_ROWS;
    }
    if (dc) {
        int n = T->song.channels * 6, k = T->ch * 6 + T->col + dc;
        k = (k + n) % n;
        T->ch = k / 6;
        T->col = k % 6;
    }
    if (T->marking) {
        T->bl_ch1 = T->ch;
        T->bl_row1 = T->row;
    }
}

/// A key in the pattern.
/// @param k the key
static void pattern_key(int k) {
    switch (k) {
    case KEY_UP: move_cursor(-1, 0); return;
    case KEY_DOWN: move_cursor(1, 0); return;
    case KEY_LEFT: move_cursor(0, -1); return;
    case KEY_RIGHT: move_cursor(0, 1); return;
    case KEY_PGUP: move_cursor(T->row >= 16 ? -16 : -T->row, 0); return;
    case KEY_PGDN: move_cursor(T->row + 16 < MOD_ROWS ? 16 : MOD_ROWS - 1 - T->row, 0); return;
    case KEY_HOME: move_cursor(-T->row, 0); return;
    case KEY_END: move_cursor(MOD_ROWS - 1 - T->row, 0); return;
    case '\t':
        T->ch = (T->ch + 1) % T->song.channels;
        T->col = 0;
        move_cursor(0, 0);
        return;
    case KEY_INS: shift_rows(T->ch, T->ch, true); return;
    case '\b': shift_rows(T->ch, T->ch, false); return;
    case '\r': {
        // Plays the row's notes, then the next row.
        for (int c = 0; c < T->song.channels; ++c) {
            mod_cell_t v = get(T->row, c);
            int n = mod_period_note(v.period);
            if (v.period && n >= 0)
                mod_jam(&T->song, c, v.sample ? v.sample : T->sample, n);
        }
        move_cursor(1, 0);
        return;
    }
    case KEY_DEL: {
        if (!T->record)
            return;
        mod_cell_t v = get(T->row, T->ch);
        if (T->col == 0)
            v.period = v.sample = 0;
        else if (T->col <= 2)
            v.sample = 0;
        else
            v.effect = v.param = 0;
        put(T->row, T->ch, v);
        advance();
        return;
    }
    }
    if (T->col == 0) {
        int n = trk_note_key(k);
        if (n < 0)
            return;
        if (T->record) {
            mod_cell_t v = get(T->row, T->ch);
            v.period = mod_note_period(n, 0);
            v.sample = T->sample;
            put(T->row, T->ch, v);
        }
        trk_jam(T->sample, n);
        if (T->record)
            advance();
        return;
    }
    int d = hex_digit(k);
    if (d < 0 || !T->record)
        return;
    mod_cell_t v = get(T->row, T->ch);
    switch (T->col) {
    case 1:
        if (d > 1)
            return;
        v.sample = d << 4 | (v.sample & 15);
        if (v.sample > 31)
            v.sample = 31;
        break;
    case 2: v.sample = (v.sample & 0xf0) | d; if (v.sample > 31) v.sample = 31; break;
    case 3: v.effect = d; break;
    case 4: v.param = d << 4 | (v.param & 15); break;
    case 5: v.param = (v.param & 0xf0) | d; break;
    }
    put(T->row, T->ch, v);
    advance();
}

/// A key in the order list.
/// @param k the key
static void orders_key(int k) {
    switch (k) {
    case KEY_UP: go_pos(T->pos - 1); break;
    case KEY_DOWN: go_pos(T->pos + 1); break;
    case KEY_PGUP: go_pos(T->pos - ORD_ROWS); break;
    case KEY_PGDN: go_pos(T->pos + ORD_ROWS); break;
    case KEY_HOME: go_pos(0); break;
    case KEY_END: go_pos(T->song.length - 1); break;
    case KEY_LEFT: order_change(-1); break;
    case KEY_RIGHT: order_change(1); break;
    case KEY_INS: order_insert(); break;
    case KEY_DEL: order_delete(); break;
    case '\r': T->focus = FOCUS_PATTERN; break;
    }
}

/// A key in the song panel.
/// @param k the key
static void song_key(int k) {
    switch (k) {
    case KEY_UP: T->song_field = (T->song_field + SF_COUNT - 1) % SF_COUNT; break;
    case KEY_DOWN: T->song_field = (T->song_field + 1) % SF_COUNT; break;
    case KEY_LEFT: song_field_change(T->song_field, -1); break;
    case KEY_RIGHT: song_field_change(T->song_field, 1); break;
    case KEY_PGDN: song_field_change(T->song_field, -10); break;
    case KEY_PGUP: song_field_change(T->song_field, 10); break;
    case '\r': song_field_enter(T->song_field); break;
    }
}

/// Keys that work everywhere. True if taken.
/// @param k the key
/// @return taken
static bool global_key(int k) {
    int id = -1;
    switch (k) {
    case KEY_F1: id = HIT_HELP; break;
    case KEY_F2: id = HIT_SCREEN_PATTERN; break;
    case KEY_F3: id = HIT_SCREEN_SAMPLE; break;
    case KEY_F4:
        T->screen = SCREEN_PATTERN;
        T->focus = (T->focus + 1) % 3;
        T->full = true;
        return true;
    case KEY_F5: id = HIT_PLAY_SONG; break;
    case KEY_F6: id = HIT_PLAY_PATTERN; break;
    case KEY_F7: id = HIT_PLAY_POS; break;
    case KEY_F8: id = HIT_STOP; break;
    case KEY_F9:
    case KEY_F10: trk_menu_open(0); return true;
    case KEY_F11: song_field_change(SF_OCTAVE, -1); return true;
    case KEY_F12: song_field_change(SF_OCTAVE, 1); return true;
    case KEY_ESC:
    case CTRL('q'): id = HIT_QUIT; break;
    case CTRL('n'): id = HIT_NEW; break;
    case CTRL('o'): id = HIT_LOAD; break;
    case CTRL('s'): id = HIT_SAVE; break;
    case CTRL('l'): id = T->screen == SCREEN_SAMPLE ? TOOL_LOAD : CMD_SOLO; break;
    case CTRL('w'): id = TOOL_SAVE; break;
    case CTRL('f'): id = HIT_FOLLOW; break;
    case CTRL('t'): id = CMD_MUTE; break;
    case CTRL('u'): id = CMD_UNMUTE; break;
    case ' ':
        if (T->focus == FOCUS_SONG && T->screen == SCREEN_PATTERN)
            return false;
        {
            mod_status_t st;
            mod_get_status(&st);
            if (st.state != MOD_STOPPED)
                trk_stop();
            else
                T->record = !T->record;
        }
        return true;
    case '[': song_field_change(SF_STEP, -1); return true;
    case ']': song_field_change(SF_STEP, 1); return true;
    case '-': id = HIT_SAMPLE_UP; break;
    case '=':
    case '+': id = HIT_SAMPLE_DOWN; break;
    case '<': go_pos(T->pos - 1); return true;
    case '>': go_pos(T->pos + 1); return true;
    }
    if (id < 0 && k >= KEY_ALT && T->screen == SCREEN_PATTERN) {
        switch (k - KEY_ALT) {
        case 'b': id = CMD_BEGIN; break;
        case 'e': id = CMD_END; break;
        case 'l': id = CMD_MARK; break;
        case 'u': id = CMD_UNMARK; break;
        case 'x': id = CMD_CUT; break;
        case 'c': id = CMD_COPY; break;
        case 'v': id = CMD_PASTE; break;
        case 'm': id = CMD_MIX; break;
        case 'q': id = CMD_UP1; break;
        case 'a': id = CMD_DOWN1; break;
        case 'w': id = CMD_UP12; break;
        case 's': id = CMD_DOWN12; break;
        case 'i': id = CMD_INS_ROW; break;
        case 'd': id = CMD_DEL_ROW; break;
        case 'n': id = HIT_PAT_NEW; break;
        case 'k': id = HIT_PAT_CLONE; break;
        case 'r': id = HIT_PAT_CLEAR; break;
        case 'f': id = CMD_WRITE_SPEED; break;
        case 'g': id = CMD_WRITE_BPM; break;
        }
    }
    if (id <= 0)
        return false;
    command(id, 0);
    return true;
}

/// Undo or redo.
/// @param redo redo
static void undo_key(bool redo) {
    if (!undo_step(redo))
        return;
    if (T->xm)
        xed_refresh();
    else
        go_pos(T->pos);
}

/// Handles a key.
/// @param k the key
static void key(int k) {
    if (k == CTRL('z') || k == CTRL('y')) {
        undo_key(k == CTRL('y'));
        return;
    }
    if (T->xm) {
        if (k == KEY_F2 || k == KEY_F3) {
            T->screen = k == KEY_F3 ? SCREEN_SAMPLE : SCREEN_PATTERN;
            T->full = true;
            return;
        }
        if (T->screen == SCREEN_SAMPLE) {
            if (xmi_key(k))
                return;
            // Playing works from here too; the pattern's editing keys don't.
            if (k == KEY_F5 || k == KEY_F6 || k == KEY_F7 || k == KEY_F8 || k == ' ' || k == KEY_F11 ||
                k == KEY_F12) {
                xed_key(k);
                return;
            }
            global_key(k);
            return;
        }
        if (xed_key(k))
            return;
        global_key(k);
        return;
    }
    if (T->screen == SCREEN_SAMPLE && smp_key(k))
        return;
    if (global_key(k))
        return;
    if (T->screen != SCREEN_PATTERN)
        return;
    if (T->focus == FOCUS_ORDERS)
        orders_key(k);
    else if (T->focus == FOCUS_SONG)
        song_key(k);
    else
        pattern_key(k);
}

// ---- The mouse ------------------------------------------------------------------------------------------

/// A click (or drag) in the pattern: moves the cursor, marks a block.
/// @param x the x
/// @param y the y
/// @param drag a drag
static void pattern_click(int x, int y, bool drag) {
    int i = (y - PAT_Y) / ROW_H;
    int r = T->pat_top + i;
    if (r < 0 || r >= MOD_ROWS || x < PAT_X)
        return;
    int c = (x - PAT_X) / CH_W, off = (x - PAT_X) % CH_W;
    if (c >= T->song.channels)
        return;
    int k = 0;
    for (int f = 0; f < 6; ++f)
        if (off >= col_x[f])
            k = f;
    if (drag) {
        // Dragging marks a block from where the button went down to the
        // cursor, which goes with the pointer (scrolling at the edges).
        if (!T->marking) {
            T->block = T->marking = true;
            T->bl_ch0 = T->ch;
            T->bl_row0 = T->row;
        }
        if (i <= 0 && r > 0)
            --r;
        else if (i >= PAT_ROWS - 1 && r < MOD_ROWS - 1)
            ++r;
        T->ch = c;
        T->row = r;
        T->bl_ch1 = c;
        T->bl_row1 = r;
        return;
    }
    T->focus = FOCUS_PATTERN;
    T->ch = c;
    T->col = k;
    T->row = r;
    T->marking = false;
    T->block = false;
    T->full = true;  // the focus changed
}

/// Handles the mouse.
static void mouse_events(void) {
    mouse_event_t e;
    static bool dragging;
    static int drag_id;
    bool any = false;
    while (mouse_event(&e)) {
        if (e.type != MOUSE_MOVE) {
            undo_watch();  // what the last click or drag changed
            any = true;
        }
        if (T->xm) {
            // An XM song: its own places; the menu bar and right-click menus here.
            const hit_t *h = hit_find(e.x, e.y);
            if (T->screen == SCREEN_SAMPLE ? xmi_click(h, &e) : xed_click(h, &e))
                continue;
            if (e.type == MOUSE_DOWN && h && h->id == HIT_MENU)
                command(HIT_MENU, h->arg);
            else if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT)
                context_menu(e.x, e.y);
            continue;
        }
        if (e.type == MOUSE_WHEEL) {
            const hit_t *h = hit_find(e.x, e.y);
            int d = -e.wheel;
            if (h && h->id == HIT_ORDER)
                go_pos(T->pos + d);
            else if (h && h->id == HIT_SAMPLE)
                T->sample = T->sample + d < 1 ? 1 : T->sample + d > 31 ? 31 : T->sample + d;
            else if (h && (h->id == HIT_SONG_FIELD || h->id == HIT_SONG_DEC || h->id == HIT_SONG_INC))
                song_field_change(h->arg, -d);
            else if (T->screen == SCREEN_SAMPLE)
                smp_wheel(h, e.x, d);
            else
                move_cursor(d * 2, 0);
            continue;
        }
        if (e.type == MOUSE_UP) {
            if (dragging && drag_id == HIT_PATTERN && T->marking) {
                T->marking = false;
                if (T->bl_ch0 == T->bl_ch1 && T->bl_row0 == T->bl_row1)
                    T->block = false;  // a click, not a drag
            }
            dragging = false;
            continue;
        }
        if (e.type == MOUSE_MOVE) {
            if (!dragging || !(e.buttons & MOUSE_LEFT))
                continue;
            if (drag_id == HIT_PATTERN)
                pattern_click(e.x, e.y, true);
            else if (drag_id == HIT_WAVE || drag_id == HIT_SMP_FIELD)
                smp_click(hit_find(e.x, e.y), e.x, e.y, true);
            continue;
        }
        // A button went down.
        const hit_t *h = hit_find(e.x, e.y);
        if (e.button == MOUSE_RIGHT) {
            // Right button: a menu, or mute a channel.
            if (h && h->id == HIT_SCOPE)
                mute(h->arg, 1);
            else
                context_menu(e.x, e.y);
            continue;
        }
        if (!h)
            continue;
        dragging = true;
        drag_id = h->id;
        if (h->id == HIT_PATTERN) {
            pattern_click(e.x, e.y, false);
            if (e.double_click)
                T->record = !T->record;
        } else if (h->id == HIT_WAVE || h->id == HIT_SMP_FIELD || h->id == HIT_SMP_DEC ||
                   h->id == HIT_SMP_INC || h->id == HIT_WAVE_SCROLL) {
            smp_click(h, e.x, e.y, false);
        } else if (h->id == HIT_SAMPLE && e.double_click) {
            T->sample = h->arg;
            command(HIT_SCREEN_SAMPLE, 0);
        } else if (h->id != HIT_NONE) {
            command(h->id, h->arg);
        }
    }
    if (any)
        undo_watch();
}

// ---- Running ---------------------------------------------------------------------------------------------

/// The tracker's loop.
static void run(void) {
    uint32_t next_frame = 0;
    quit_wanted = false;
    while (!quit_wanted) {
        int k;
        while (!quit_wanted && (k = key_poll()) != KEY_NONE) {
            key(k);
            undo_watch();
        }
        mouse_events();
        mouse_pointer_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - next_frame) >= 0 && !quit_wanted) {
            draw();
            next_frame = now + 40;
        }
        sleep_ms(2);
    }
}

/// Makes the tracker's state (PSRAM).
/// @return made
static bool start(void) {
    T = psram_malloc(sizeof *T);
    if (!T)
        return false;
    memset(T, 0, sizeof *T);
    T->octave = 2;
    T->step = 1;
    T->sample = 1;
    T->follow = true;
    T->speed = 6;
    T->bpm = 125;
    T->separation = 70;
    T->full = true;
    T->wave_dirty = true;
    mod_set_separation(T->separation);
    mod_set_tempo(T->speed, T->bpm);
    return true;
}

/// Frees everything the tracker used.
static void finish(void) {
    undo_free();
    xed_close();
    mod_stop();
    audio_set_source(NULL);
    sleep_ms(5);  // core 1 may be in the middle of a frame
    xm_release();
    mod_clear(&T->song);
    psram_free(T);
    T = NULL;
}

/// The audio source while the tracker runs: whichever player has a song.
/// @param left the left sample
/// @param right the right
/// @return true
static bool trk_render(int32_t *left, int32_t *right) {
    int32_t ml = 0, mr = 0, xl = 0, xr = 0;
    bool a = mod_render(&ml, &mr);
    bool b = T && T->xm ? xm_render_frame(&xl, &xr) : false;
    *left = ml + xl;
    *right = mr + xr;
    return a || b;
}

/// The tracker.
/// @param path a song to load, or NULL
static void tracker(const char *path) {
    if (!start()) {
        printf("tracker: not enough memory\n");
        return;
    }
    if (!mod_new(&T->song, 4)) {
        printf("tracker: not enough memory\n");
        psram_free(T);
        T = NULL;
        return;
    }
    audio_set_source(trk_render);
    T->underruns0 = hdmi_audio_underruns;
    scr_begin();
    mouse_pointer(MOUSE_POINTER_ARROW);
    if (path)
        song_load(path);
    run();
    mouse_pointer(MOUSE_POINTER_OFF);
    finish();
    scr_end();
}

/// modplay of an XM (FastTracker 2) song: played once through.
/// @param path the file
/// @param data its bytes
/// @param len their count
static void xmplay(const char *path, const uint8_t *data, uint32_t len) {
    xm_song_t *s = psram_malloc(sizeof *s);
    const char *err = s ? xm_load(s, data, len) : "not enough memory";
    if (err) {
        printf("%s: %s\n", path, err);
        if (s)
            xm_clear(s);
        psram_free(s);
        return;
    }
    printf("%.20s  (XM, %d channels, %d positions, %d instruments)\nAny key stops.\n", s->title,
           s->channels, s->length, s->ninstruments);
    xm_set_loop(false);
    xm_set_volume(100);
    xm_pause(false);
    audio_set_source(xm_render_frame);
    if (!xm_play(s, 0, 0))
        printf("Not enough memory for the player.\n");
    uint32_t start_us = time_us_32(), under0 = hdmi_audio_underruns;
    sleep_ms(50);
    while (getchar_timeout_us(100000) == PICO_ERROR_TIMEOUT) {
        xm_status_t st;
        xm_get_status(&st);
        if (!st.playing)
            break;
        unsigned load = audio_source_load();
        uint32_t t = (time_us_32() - start_us) / 1000000;
        printf("\r%2lu:%02lu  position %02d/%02d  row %02d  speed %2d  tempo %3d  voices %2d  CPU %u.%u%%  ",
               (unsigned long)(t / 60), (unsigned long)(t % 60), st.order, s->length - 1, st.row,
               st.speed, st.bpm, st.active, load / 10, load % 10);
        fflush(stdout);
    }
    while (getchar_timeout_us(20000) != PICO_ERROR_TIMEOUT)
        ;
    xm_stop();
    audio_set_source(NULL);
    sleep_ms(5);
    xm_release();
    xm_set_loop(true);
    uint32_t under = hdmi_audio_underruns - under0;
    printf("\nStopped.%s\n", under ? " (the sound skipped)" : "");
    xm_clear(s);
    psram_free(s);
}

/// modplay: plays a MOD (or XM) once through.
/// @param path the file
static void modplay(const char *path) {
    if (!start()) {
        printf("modplay: not enough memory\n");
        return;
    }
    uint32_t len;
    uint8_t *data = file_read_all(path, &len, 16u << 20);
    if (!data) {
        printf("%s: %s\n", path, T->msg);
        psram_free(T);
        T = NULL;
        return;
    }
    if (xm_is_xm(data, len)) {
        xmplay(path, data, len);
        psram_free(data);
        psram_free(T);
        T = NULL;
        return;
    }
    const char *err = mod_load(&T->song, data, len);
    psram_free(data);
    if (err) {
        printf("%s: %s\n", path, err);
        mod_clear(&T->song);
        psram_free(T);
        T = NULL;
        return;
    }
    mod_song_t *s = &T->song;
    printf("%.20s  (%d channels, %d positions, %d patterns)\nAny key stops.\n", s->title,
           s->channels, s->length, s->npatterns);
    audio_set_source(mod_render);
    mod_play(s, 0, 0, false);
    uint32_t start_us = time_us_32(), under0 = hdmi_audio_underruns;
    int last_order = -1;
    while (getchar_timeout_us(100000) == PICO_ERROR_TIMEOUT) {
        mod_status_t st;
        mod_get_status(&st);
        if (st.order < last_order)
            break;  // it went round: once is enough
        last_order = st.order;
        unsigned load = audio_source_load();
        uint32_t t = (time_us_32() - start_us) / 1000000;
        printf("\r%2lu:%02lu  position %02d/%02d  row %02d  speed %2d  tempo %3d  CPU %u.%u%%  ",
               (unsigned long)(t / 60), (unsigned long)(t % 60), st.order, s->length - 1, st.row,
               st.speed, st.tempo, load / 10, load % 10);
        fflush(stdout);
    }
    while (getchar_timeout_us(20000) != PICO_ERROR_TIMEOUT)
        ;
    uint32_t under = hdmi_audio_underruns - under0;
    printf("\nStopped.%s\n", under ? " (the sound skipped)" : "");
    finish();
}

void tracker_command(int argc, char **argv) {
    if (!strcmp(argv[0], "modplay")) {
        if (argc < 2) {
            printf("usage: modplay FILE.MOD\n");
            return;
        }
        modplay(argv[1]);
        return;
    }
    tracker(argc > 1 ? argv[1] : NULL);
}
