/**
 * @file mc.h
 * @brief mc: a Midnight Commander style file manager on the 80x30 console.
 *
 * Shared between mc_scr.c (screen and keys), mc_ui.c (dialogs and menus),
 * mc_panel.c (the two panels), mc_ops.c (copy, move, delete, find),
 * mc_view.c (the viewer) and mc.c (the main loop).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "mouse.h"

/// Screen columns.
#define SCR_W 80
/// Screen rows.
#define SCR_H 30

// ---- Colours: PC attributes, foreground + 16 * background --------------------------------
/// A PC attribute from foreground and background colour numbers.
#define A(fg, bg) ((uint8_t)((fg) | (bg) << 4))
/// The 16 PC colours.
enum {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGRAY,
    DGRAY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE,
};
#define C_PANEL     A(LGRAY, BLUE)    ///< panel text and frames
#define C_DIR       A(WHITE, BLUE)    ///< folders
#define C_EXEC      A(LGREEN, BLUE)   ///< programs (.BAS)
/// Marked files.
#define C_MARKED    A(YELLOW, BLUE)
#define C_SELECT    A(BLACK, CYAN)    ///< the cursor bar
/// Marked files under the cursor bar.
#define C_SELMARK   A(YELLOW, CYAN)
#define C_HEADER    A(YELLOW, BLUE)   ///< column titles
#define C_TITLE_ON  A(BLACK, CYAN)    ///< the active panel's path
/// The menu bar.
#define C_MENU      A(BLACK, CYAN)
/// A menu item's hot key letter.
#define C_MENU_HOT  A(YELLOW, CYAN)
/// The highlighted menu item.
#define C_MENU_SEL  A(WHITE, BLACK)
/// Key bar numbers.
#define C_KEYNUM    A(LGRAY, BLACK)
/// Key bar labels.
#define C_KEYTEXT   A(BLACK, CYAN)
/// The command line.
#define C_CMDLINE   A(LGRAY, BLACK)
/// Dialogs.
#define C_DIALOG    A(BLACK, LGRAY)
/// A dialog's title.
#define C_DLG_TITLE A(BLUE, LGRAY)
/// The focused button or field in a dialog.
#define C_DLG_FOCUS A(BLACK, CYAN)
/// Input fields.
#define C_INPUT     A(BLACK, CYAN)
/// Error dialogs.
#define C_ERROR     A(WHITE, RED)
/// The focused button in an error dialog.
#define C_ERR_FOCUS A(BLACK, LGRAY)
/// The file viewer.
#define C_VIEW      A(LGRAY, BLUE)

/// CP437 line drawing
#define D_H  0xcd
/// CP437 double vertical line.
#define D_V  0xba
/// CP437 double top-left corner.
#define D_TL 0xc9
/// CP437 double top-right corner.
#define D_TR 0xbb
/// CP437 double bottom-left corner.
#define D_BL 0xc8
/// CP437 double bottom-right corner.
#define D_BR 0xbc
/// CP437 single horizontal line.
#define S_H  0xc4
/// CP437 single vertical line.
#define S_V  0xb3
#define DS_L 0xc7  ///< ╟ double vertical, single line going right
#define DS_R 0xb6  ///< ╢
#define DS_T 0xd1  ///< ╤ double horizontal, single line going down
#define DS_B 0xcf  ///< ╧
#define SS_X 0xc5  ///< ┼
#define SS_T 0xc2  ///< ┬
#define SS_B 0xc1  ///< ┴
/// CP437 full block.
#define BLOCK_FULL  0xdb
/// CP437 light shade.
#define BLOCK_LIGHT 0xb0

// ---- Screen (mc_scr.c): drawn into a buffer, sent by scr_flush ---------------------------
void scr_begin(void);  ///< saves the shell screen
void scr_end(void);    ///< puts it back
/// True between scr_begin and scr_end.
/// @return true while the screen is in use
bool scr_active(void); ///< between them (one user at a time: mc, telnet's directory)
/// Puts a character and attribute in a cell of the screen being drawn.
/// @param row the row
/// @param col the column
/// @param ch the character
/// @param attr its attribute
void scr_put(int row, int col, uint8_t ch, uint8_t attr);
/// Fills an h by w rectangle with a character and attribute.
/// @param row the top
/// @param col the left
/// @param h the height
/// @param w the width
/// @param ch the character
/// @param attr its attribute
void scr_fill(int row, int col, int h, int w, uint8_t ch, uint8_t attr);
/// Text, cut or padded to width (width < 0: as long as it is).
/// @param row the row
/// @param col the column
/// @param width the width (< 0: as long as it is)
/// @param attr the attribute
/// @param s the text
void scr_text(int row, int col, int width, uint8_t attr, const char *s);
/// Formatted text, cut or padded to width.
/// @param row the row
/// @param col the column
/// @param width the width
/// @param attr the attribute
/// @param fmt printf's format, and its arguments
void scr_printf(int row, int col, int width, uint8_t attr, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
/// Draws a box, with double or single lines.
/// @param row the top
/// @param col the left
/// @param h the height
/// @param w the width
/// @param dbl double lines
/// @param attr the attribute
void scr_box(int row, int col, int h, int w, bool dbl, uint8_t attr);
/// Draws a horizontal line with its two ends.
/// @param row the row
/// @param col the left
/// @param w the width
/// @param left the left end
/// @param mid the middle
/// @param right the right end
/// @param attr the attribute
void scr_hline(int row, int col, int w, uint8_t left, uint8_t mid, uint8_t right, uint8_t attr);
/// Sends the cells that changed to the console.
void scr_flush(void);
/// For a pixel screen (the tracker) with dialogs and menus over it: forgets
/// the cells, so flush sends only what is drawn after this, and where a cell
/// is no longer drawn puts back the pixels from `picture` (a 640x480 copy of
/// the screen, kept until the next call). NULL ends it.
/// @param picture the 640x480 picture under the cells, or NULL to end
void scr_overlay(const uint8_t *picture);
/// Puts the cursor in a cell (-1 hides it).
/// @param row the row
/// @param col the column
void scr_cursor(int row, int col);  ///< -1: hidden
/// The character and attribute in a cell of the screen being drawn.
/// @param row the row
/// @param col the column
/// @param ch set to the character
/// @param attr set to its attribute
void scr_get(int row, int col, uint8_t *ch, uint8_t *attr);
/// Saved copies of the whole screen (PSRAM), e.g. under a dialog.
/// @return the copy (PSRAM), or NULL
void *scr_save(void);
/// Puts back a copy from scr_save (and frees it).
/// @param saved the copy
void scr_restore(void *saved);
/// The shell's screen: shown for Ctrl-O and while a command runs.
void scr_show_user(void);
/// Takes the console's screen as the shell's (after a command ran).
void scr_grab_user(void);

/// Keys: bytes, or these. Esc then a digit is an F-key (Esc 0 = F10), Esc
/// then a letter is Alt (KEY_ALT + letter).
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INS, KEY_DEL, KEY_ESC,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11,
    KEY_F12,
    KEY_MOUSE = 0x1ff,  ///< from key_get_mouse: a click or the wheel
    KEY_ALT = 0x200,  ///< + the letter
    KEY_NONE = -1,  ///< no key waiting
};
/// The control-key code of letter `c`.
#define CTRL(c) ((c) & 0x1f)
/// Waits for a key.
/// @return the key (byte or KEY_ value)
int key_get(void);               ///< waits
/// The same, or KEY_MOUSE and a mouse event (moves, buttons, the wheel).
/// @param e filled in for KEY_MOUSE
/// @return the key, or KEY_MOUSE
int key_get_mouse(mouse_event_t *e);
/// The key waiting, if any.
/// @return the key, or KEY_NONE
int key_poll(void);              ///< KEY_NONE if nothing
/// Checks for Esc or Ctrl-C without taking other keys.
/// @return true if one is waiting
bool key_stop(void);             ///< Esc or Ctrl-C waiting (others are kept)

// ---- Dialogs and menus (mc_ui.c) -------------------------------------------------------------
/// A message with buttons ("Yes|No|All"); the chosen button's number, -1 for
/// Esc. error: red.
/// @param title the title
/// @param text the message
/// @param buttons the buttons, "Yes|No"
/// @param error shown in red
/// @return the button chosen, or -1 for Esc
int dlg_message(const char *title, const char *text, const char *buttons, bool error);
/// One line of input; false if cancelled.
/// @param title the title
/// @param prompt the prompt
/// @param buf the text, edited in place
/// @param size its size
/// @return false if cancelled
bool dlg_input(const char *title, const char *prompt, char *buf, int size);
/// A list to pick from; the chosen index or -1.
/// @param title the title
/// @param items the items
/// @param n how many
/// @param start the one first selected
/// @return the index chosen, or -1
int dlg_list(const char *title, char *const *items, int n, int start);
/// Progress: a box with up to two bars, kept until dlg_progress_end.
/// @param title the box's title
void dlg_progress_begin(const char *title);
/// Shows the progress box's two lines and bars.
/// @param line1 the first line
/// @param line2 the second
/// @param done1 the first bar's progress
/// @param total1 its total
/// @param done2 the second bar's progress
/// @param total2 its total
void dlg_progress(const char *line1, const char *line2, uint64_t done1, uint64_t total1,
                  uint64_t done2, uint64_t total2);
/// Takes the progress box away.
void dlg_progress_end(void);
/// A pull-down menu bar; the chosen item's id, or -1.
typedef struct {
    const char *text;  ///< NULL ends; "-" is a separator
    int id;  ///< what menu_run returns for it
    const char *key;   ///< the shortcut shown on the right
    bool check;        ///< shows a tick (set by the caller)
} menu_item_t;
/// A menu on the bar: its title and items.
typedef struct {
    const char *title;  ///< the menu's title on the bar
    menu_item_t *items;  ///< its items, ending with one whose text is NULL
} menu_t;
/// Runs a pull-down menu bar; the chosen item's id, or -1.
/// @param menus the menus
/// @param count how many
/// @param open the menu to open first, or -1
/// @return the item's id, or -1
int menu_run(menu_t *menus, int count, int open);

// ---- Panels (mc_panel.c) -------------------------------------------------------------------------
/// A file in a panel: its name, size, date, attributes and mark.
typedef struct {
    uint32_t name;  ///< offset in the panel's name pool
    FSIZE_t size;  ///< the file's size
    /// its date and time (FAT format)
    ///@{
    WORD date, time;
    ///@}
    BYTE attr;  ///< its FAT attributes
    bool marked;  ///< marked
} entry_t;

/// A panel's view: full, brief, long or info.
enum { MODE_FULL, MODE_BRIEF, MODE_LONG, MODE_INFO };
/// A panel's sort order.
enum { SORT_NAME, SORT_EXT, SORT_SIZE, SORT_TIME, SORT_NONE };

/// A panel: its folder, files, cursor, view, sort and marks.
typedef struct {
    char path[256];  ///< the folder shown
    /// the view (MODE_) and sort order (SORT_)
    ///@{
    int mode, sort;
    ///@}
    bool reverse;  ///< sort the other way round
    entry_t *ent;  ///< PSRAM
    /// files in it, and room for
    ///@{
    int count, cap;
    ///@}
    char *names;   ///< PSRAM
    /// bytes used and room in the name pool
    ///@{
    size_t names_len, names_cap;
    ///@}
    /// the cursor, and the first file shown
    ///@{
    int cur, top;
    ///@}
    int marked;  ///< files marked
    FSIZE_t marked_size;  ///< their total size
    char error[60];  ///< why the listing failed
} panel_t;

extern panel_t *panels;  ///< two of them, in PSRAM while mc runs
extern int active;  ///< 0 left, 1 right
/// Show hidden and system files.
extern bool show_hidden;

/// The top row of the panels.
#define PANEL_TOP    1
/// Rows of a panel, its frame included.
#define PANEL_H      27
#define PANEL_ROWS   (PANEL_H - 5)  ///< list rows in full mode

/// A file's name in the panel's name pool.
/// @param p the panel
/// @param e the file
/// @return its name
const char *ent_name(const panel_t *p, const entry_t *e);
/// Reads the panel's folder again.
/// @param p the panel
/// @param keep a name to keep the cursor on, or NULL
void panel_load(panel_t *p, const char *keep);  ///< reread; keep the cursor on that name
/// Sorts the panel's files.
/// @param p the panel
void panel_sort(panel_t *p);
/// Draws panel 0 (left) or 1 (right).
/// @param which 0 or 1
void panel_draw(int which);
/// Keeps the cursor in view.
/// @param p the panel
void panel_fix_scroll(panel_t *p);
/// The files a panel shows at once.
/// @param p the panel
/// @return how many
int panel_rows(const panel_t *p);   ///< entries visible
/// Moves the cursor for an arrow, page or Home/End key.
/// @param p the panel
/// @param key the key
void panel_key_move(panel_t *p, int key);
/// Goes into a folder (".." goes up).
/// @param p the panel
/// @param name the folder
void panel_enter(panel_t *p, const char *name);  ///< into a folder ("..": up)
/// The full path of `name` in the panel's folder.
/// @param p the panel
/// @param name the file
/// @param out where the path goes
/// @param size its size
void panel_full_path(const panel_t *p, const char *name, char *out, size_t size);
/// Marks or unmarks file `i`.
/// @param p the panel
/// @param i the file's index
/// @param on marked or not
void panel_mark(panel_t *p, int i, bool on);
/// Frees the panel's files.
/// @param p the panel
void panel_free(panel_t *p);
/// Marks or unmarks the files matching `pattern`.
/// @param p the panel
/// @param pattern the pattern (* and ?)
/// @param on mark or unmark
void panel_select_pattern(panel_t *p, const char *pattern, bool on);
/// Moves the cursor to the first file starting with `text`.
/// @param p the panel
/// @param text the start of a name
void panel_quick_search(panel_t *p, const char *text);
/// Draws the info view of the other panel's file.
/// @param which the panel drawn
/// @param other the panel whose file is shown
void info_draw(int which, const panel_t *other);

// ---- Operations (mc_ops.c) ---------------------------------------------------------------------
/// mc's Configuration settings: ask before deleting, overwriting and leaving.
///@{
extern bool confirm_delete, confirm_overwrite, confirm_exit;
///@}
/// Copies (or moves) the marked files, or the one under the cursor, to the other panel.
/// @param move move instead of copy
void op_copy(bool move);
/// Deletes the marked files, or the one under the cursor.
void op_delete(void);
/// Makes a folder.
void op_mkdir(void);
/// Finds files by name and text.
void op_find(void);
/// Marks the files that differ from the other panel's.
void op_compare(void);
/// Changes a file's attributes.
void op_attributes(void);

// ---- Viewer (mc_view.c) ----------------------------------------------------------------------
/// The file viewer (F3).
/// @param path the file
void view_file(const char *path);

// ---- Main (mc.c) --------------------------------------------------------------------------------
void mc_redraw(void);   ///< everything, then flush
void mc_draw(void);     ///< everything into the buffer
/// @param line the command line
/// @param pause wait for a key afterwards
void run_command(const char *line, bool pause);  ///< a shell command, on the shell's screen
/// The mc command.
/// @param argc the argument count
/// @param argv the arguments
void mc_command(int argc, char **argv);
