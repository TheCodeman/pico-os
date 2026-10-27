/**
 * @file trk.h
 * @brief The MOD tracker: a ProTracker-style editor drawn in pixels on the 640x480 screen.
 *
 * trk_gfx.c draws (and remembers where the buttons are for the mouse), trk.c
 * is the main loop with the pattern screen, trk_sample.c the sample screen,
 * sample files and tools, trk_file.c songs on disk and the file picker,
 * trk_ui.c the menus and dialogs (in the tracker's own style, with mc's
 * calls).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "mc.h"
#include "mod.h"
#include "mouse.h"
#include "xm.h"

// ---- Drawing (trk_gfx.c) ------------------------------------------------------------------------
/// Colours, RGB332
#define C_BG       0x00  ///< black
#define C_FACE     0x25  ///< panel face
#define C_LIGHT    0x6e  ///< bevel highlight
#define C_DARK     0x01  ///< bevel shadow
#define C_TEXT     0xff  ///< white
#define C_DIM      0x92  ///< grey
#define C_LABEL    0xb6  ///< light grey on the faces
/// Tracker colour (RGB332): yellow.
#define C_YELLOW   0xfc
/// Tracker colour (RGB332): cyan.
#define C_CYAN     0x5f
/// Tracker colour (RGB332): green.
#define C_GREEN    0x3c
/// Tracker colour (RGB332): orange.
#define C_ORANGE   0xf4
/// Tracker colour (RGB332): red.
#define C_RED      0xe0
/// Tracker colour (RGB332): blue.
#define C_BLUE     0x0b
#define C_FIELD    0x00  ///< value boxes
#define C_FOCUS    0x2b  ///< a field with the keyboard
#define C_ROW4     0x05  ///< pattern: every 4th row
#define C_ROW16    0x26  ///< every 16th
#define C_CURROW   0x4a  ///< the cursor's row
#define C_PLAYROW  0x60  ///< the row playing
#define C_BLOCK    0x8d  ///< the marked block
#define C_CURSOR   0xfe  ///< the cursor field (text black)

/// Where drawing goes: the screen, or an off-screen copy that is then shown
/// in one go (menus and dialogs, so nothing half-drawn is ever seen).
extern uint8_t *gfx_target;
/// Fills a w by h rectangle with colour c.
void gfx_fill(int x, int y, int w, int h, uint8_t c);
/// Draws a horizontal line w pixels long.
void gfx_hline(int x, int y, int w, uint8_t c);
/// Draws a vertical line h pixels long.
void gfx_vline(int x, int y, int h, uint8_t c);
/// Draws one 8x16 character.
void gfx_char(int x, int y, uint8_t ch, uint8_t fg, uint8_t bg);
/// Text, padded with spaces (or cut) to n characters if n >= 0; returns the
/// x after it.
int gfx_text(int x, int y, const char *s, int n, uint8_t fg, uint8_t bg);
/// The same, 14 pixels high (the pattern's rows).
void gfx_char14(int x, int y, uint8_t ch, uint8_t fg, uint8_t bg);
/// Text 14 pixels high, padded or cut to n characters if n >= 0; the x after it.
int gfx_text14(int x, int y, const char *s, int n, uint8_t fg, uint8_t bg);
/// Formatted text 14 pixels high, as gfx_text14.
int gfx_printf(int x, int y, int n, uint8_t fg, uint8_t bg, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
/// A raised (or sunken) panel: face filled, edges lit and shaded.
void gfx_bevel(int x, int y, int w, int h, bool raised, uint8_t face);
/// The edges only, for drawing a pressed button without clearing.
void gfx_bevel_edges(int x, int y, int w, int h, bool raised);

/// Places the mouse can click, made while drawing; cleared on a full redraw.
typedef struct {
    /// the clickable rectangle
    ///@{
    int16_t x, y, w, h;
    ///@}
    /// what it is (HIT_), and its argument
    ///@{
    int16_t id, arg;
    ///@}
} hit_t;
/// Forgets the clickable places (before a full redraw).
void hit_clear(void);
/// Adds a clickable place with its id and argument.
void hit_add(int x, int y, int w, int h, int id, int arg);
/// The clickable place at x, y, or NULL.
const hit_t *hit_find(int x, int y);
/// A button: drawn and made clickable. lit: shown pressed (a mode that is on).
void gfx_button(int x, int y, int w, int h, const char *label, int id, int arg, bool lit);

// ---- The editor's state -----------------------------------------------------------------------------
/// The screen shown: the pattern editor or the sample editor.
enum { SCREEN_PATTERN, SCREEN_SAMPLE };
/// Where the keys go on the pattern screen.
enum { FOCUS_PATTERN, FOCUS_ORDERS, FOCUS_SONG };
/// Song panel fields
enum { SF_TITLE, SF_BPM, SF_SPEED, SF_OCTAVE, SF_STEP, SF_CHANNELS, SF_STEREO, SF_RESTART, SF_COUNT };
/// Sample panel fields
enum { MF_NAME, MF_VOLUME, MF_FINETUNE, MF_LOOP, MF_LOOP_START, MF_LOOP_LEN, MF_SEL_START,
       MF_SEL_END, MF_COUNT };

/// Button and click ids (hit_t.id)
enum {
    HIT_NONE, HIT_PLAY_SONG, HIT_PLAY_PATTERN, HIT_PLAY_POS, HIT_STOP, HIT_RECORD, HIT_FOLLOW,
    HIT_SCREEN_PATTERN, HIT_SCREEN_SAMPLE, HIT_HELP, HIT_MENU, HIT_LOAD, HIT_SAVE, HIT_NEW, HIT_QUIT,
    HIT_SONG_FIELD,    ///< arg: field; -/+ buttons add HIT_DEC/HIT_INC
    HIT_SONG_DEC, HIT_SONG_INC,
    HIT_ORDER,         ///< arg: position
    HIT_ORDER_DEC, HIT_ORDER_INC, HIT_ORDER_INS, HIT_ORDER_DEL, HIT_ORDER_UP, HIT_ORDER_DOWN,
    HIT_SAMPLE,        ///< arg: sample 1..31
    HIT_SAMPLE_UP, HIT_SAMPLE_DOWN,
    HIT_SCOPE,         ///< arg: channel (mute)
    HIT_PATTERN,       ///< the pattern area
    HIT_PATTERN_UP, HIT_PATTERN_DOWN, HIT_PAT_NEW, HIT_PAT_CLONE, HIT_PAT_CLEAR,
    HIT_SMP_FIELD, HIT_SMP_DEC, HIT_SMP_INC,
    HIT_WAVE,          ///< the waveform
    HIT_WAVE_SCROLL,
    HIT_TOOL,          ///< arg: a sample tool (TOOL_...)
};

/// Sample tools (menu and buttons)
enum {
    TOOL_LOAD = 100, TOOL_SAVE, TOOL_CLEAR, TOOL_NORMALIZE, TOOL_AMPLIFY, TOOL_REVERSE,
    TOOL_FADE_IN, TOOL_FADE_OUT, TOOL_SILENCE, TOOL_CROP, TOOL_CUT, TOOL_LOOP_SEL,
    TOOL_GENERATE, TOOL_HALVE, TOOL_DOUBLE, TOOL_CENTRE, TOOL_SEL_ALL, TOOL_ZOOM_SEL,
    TOOL_ZOOM_ALL, TOOL_COPY_TO, TOOL_NAME,
    /// XM instruments
    TOOL_XI_LOAD, TOOL_XI_SAVE, TOOL_INS_COPY, TOOL_INS_SWAP, TOOL_INS_CLEAR, TOOL_ENV_PRESET,
};

/// Commands for an XM song (trk_xm.c), beside the HIT_ ones it shares.
enum {
    XED_BEGIN = 400, XED_END, XED_MARK, XED_UNMARK, XED_CUT, XED_COPY, XED_PASTE, XED_MIX,
    XED_UP1, XED_DOWN1, XED_UP12, XED_DOWN12, XED_INS_ROW, XED_DEL_ROW, XED_MUTE, XED_SOLO,
    XED_UNMUTE,
};

/// The tracker's state: the song, screen, cursor, selection, octave, step and settings.
typedef struct {
    mod_song_t song;  ///< the MOD song being edited
    xm_song_t *xm;       ///< an XM song instead (then `song` is unused)
    char path[256];      ///< "" for a new song
    bool dirty;  ///< changed since it was loaded or saved
    /// the screen shown and where the keys go
    ///@{
    int screen, focus;
    ///@}
    /// The pattern editor
    int pos;             ///< the order position shown
    int pattern;         ///< the pattern being edited
    /// the cursor: col 0 note, 1-2 sample, 3 effect, 4-5 parameter
    ///@{
    int row, ch, col;
    ///@}
    int pat_top;         ///< the first row on the screen
    /// the octave, row step and sample keys enter
    ///@{
    int octave, step, sample;
    ///@}
    /// recording notes, and the view following the player
    ///@{
    bool record, follow;
    ///@}
    /// speed, BPM and stereo separation for playing
    ///@{
    int speed, bpm, separation;
    ///@}
    /// the song field with the cursor, and the order list's first row shown
    ///@{
    int song_field, order_top;
    ///@}
    /// The block (marked with Alt-B and Alt-E, or dragged with the mouse)
    bool marking;        ///< begun, not ended
    bool block;  ///< a block is selected
    /// the block's corners
    ///@{
    int bl_ch0, bl_row0, bl_ch1, bl_row1;
    ///@}
    mod_cell_t clip[MOD_ROWS * MOD_MAX_CH];  ///< the clipboard
    /// its width and height
    ///@{
    int clip_w, clip_h;
    ///@}
    /// The sample screen
    ///@{
    int smp_field, smp_top;
    ///@}
    /// the selection, bytes (sel1 > sel0 if any)
    ///@{
    uint32_t sel0, sel1;
    ///@}
    /// what the waveform shows
    ///@{
    uint32_t view0, view_len;
    ///@}
    bool wave_dirty;  ///< the sample's waveform needs drawing
    /// the waveform's columns
    ///@{
    int8_t wmin[640], wmax[640];
    ///@}
    /// Drawing
    bool full;           ///< everything must be drawn again
    char msg[80];  ///< the status line's message
    uint32_t msg_until;  ///< when it goes away
    hit_t hits[160];  ///< the clickable places on screen
    int nhits;  ///< how many
    /// Load measurement
    /// the sound skipped (since the tracker started)
    ///@{
    unsigned load, underruns, underruns0;
    ///@}
} trk_t;

/// The tracker's state while it runs.
extern trk_t *T;

// ---- trk.c -----------------------------------------------------------------------------------------
/// Shows a message on the status line.
void trk_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void trk_dialog_begin(void);  ///< before an mc dialog or menu
void trk_dialog_end(void);    ///< after it: everything is drawn again
void trk_stop(void);          ///< stops the player (before changing samples)
int trk_note_key(int key);    ///< a piano key's note (0..35), -1 if it isn't one
/// Plays a note of a sample now (the keyboard and the mouse).
void trk_jam(int sample, int note);
void trk_menu_open(int open); ///< the menu bar, with menu `open` pulled down
void tracker_command(int argc, char **argv);  ///< tracker, mod, modplay

// ---- trk_xm.c: the tracker with an XM song -------------------------------------------------
int xed_pattern(void);         ///< the pattern being edited
void xed_refresh(void);        ///< after an undo: the cursor kept in the pattern
void xed_take(xm_song_t *s);   ///< an XM loaded: the tracker works on it from now on
bool xed_new(int channels);    ///< a new empty XM song
void xed_close(void);          ///< back to MOD (the XM freed)
/// Saves the XM song as `path`.
bool xed_save(const char *path);
/// Draws the pattern screen for an XM song.
void xed_draw(void);
bool xed_key(int k);           ///< false: the tracker's own keys (menus, files, help)
bool xed_command(int id);      ///< false if not one for XM songs
/// A click on the pattern screen of an XM song; false if not its own.
bool xed_click(const hit_t *h, const mouse_event_t *e);
/// trk_xmi.c: its instrument screen (F3)
void xmi_draw(void);
/// A key on the instrument screen; false if not taken.
bool xmi_key(int k);
/// A click on the instrument screen; false if not its own.
bool xmi_click(const hit_t *h, const mouse_event_t *e);
/// Forgets the instrument screen's cursor and view (another song).
void xmi_reset(void);
void xmi_tool(int tool);       ///< the Instrument menu's TOOL_ ids

// ---- trk_ui.c: menus and dialogs, as mc's (mc.h) but drawn in pixels ------------------
/// Each keeps and puts back what it covers. Menu items starting with '~' are greyed.
int gui_message(const char *title, const char *text, const char *buttons, bool error);
/// One line of input in a dialog; false if cancelled.
bool gui_input(const char *title, const char *prompt, char *buf, int size);
/// A list to pick from; the chosen index or -1.
int gui_list(const char *title, char *const *items, int n, int start);
/// A pull-down menu bar; the chosen item's id or -1.
int gui_menu(menu_t *menus, int count, int open);
int gui_popup(menu_item_t *items, int x, int y);  ///< a right-click menu at x, y

// ---- mod2xm.c: a MOD as an XM (separation: the stereo, 0-100%) ---------------------------------
/// NULL if fine; else what failed (xm_clear frees what was made).
const char *xm_from_mod(xm_song_t *x, const mod_song_t *m, int speed, int bpm, int separation);

// ---- trk_undo.c: undo and redo (Ctrl-Z, Ctrl-Y), many steps ----------------------------------
/// Edits to the pattern being edited, the order list and song settings and the
/// current sample's or instrument's settings are noticed after each key or
/// click (undo_watch); tools changing sample data keep a copy first.
void undo_watch(void);
void undo_keep_mod_sample(int n);     ///< before changing MOD sample n's sound
void undo_keep_xm_instrument(int n);  ///< before changing XM instrument n's samples
/// Undoes (or with redo, redoes) one step; false if there was none.
bool undo_step(bool redo);
/// True if there is a step to undo (or redo).
bool undo_can(bool redo);
void undo_reset(void);                ///< another song
void undo_free(void);                 ///< leaving the tracker

// ---- trk_sample.c ----------------------------------------------------------------------------------
/// Draws the sample screen.
void smp_draw(void);
bool smp_key(int key);        ///< false if not taken
/// A click (or drag) on the sample screen.
void smp_click(const hit_t *h, int x, int y, bool drag);
/// The mouse wheel on the sample screen.
void smp_wheel(const hit_t *h, int x, int d);
/// Runs one of the Sample menu's tools.
void smp_tool(int tool);
/// Replaces a sample's data (the old is freed); the player must be stopped.
void smp_set_data(int n, int8_t *data, uint32_t length);
/// Keeps a sample's loop inside its data.
void smp_fix_loop(mod_sample_t *m);
void smp_load(const char *path);  ///< into the current sample
void smp_save(const char *path);  ///< the current sample, as WAV

// ---- trk_file.c --------------------------------------------------------------------------------------
/// Picks a file (with folders to go into): true and its path in out. save:
/// a name can be typed. exts: ".MOD.WAV" style list of extensions shown.
bool file_pick(const char *title, const char *exts, bool save, char *out, int size);
/// Loads a MOD or XM song.
bool song_load(const char *path);
/// Saves the song as `path`.
bool song_save(const char *path);
/// Asks for a name and saves the song; false if cancelled or failed.
bool song_save_as(void);
bool song_to_xm(void);   ///< the MOD being edited made into an XM (Save then writes .XM)
bool song_check_saved(const char *doing);  ///< false: cancelled
/// Reads a whole file into PSRAM (NULL with a message on failure).
uint8_t *file_read_all(const char *path, uint32_t *len, uint32_t max);
/// Writes a whole file; false (with a message) on failure.
bool file_write_all(const char *path, const void *data, uint32_t len);
/// The tracker's help screen (F1).
void help_show(void);
