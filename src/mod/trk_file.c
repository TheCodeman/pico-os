/**
 * @file trk_file.c
 * @brief The tracker's files: the file picker, loading and saving songs, and the help screen.
 *
 * FatFs calls go through the SMB wrapper, so mounted shares work too.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dvi.h"
#include "ff.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "smb_vfs.h"
#include "trk.h"

uint8_t *file_read_all(const char *path, uint32_t *len, uint32_t max) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        trk_message("Can't open %s.", path);
        return NULL;
    }
    FSIZE_t size = f_size(&f);
    if (size > max) {
        f_close(&f);
        trk_message("%s is too big (the most is %lu KB).", path, (unsigned long)(max >> 10));
        return NULL;
    }
    uint8_t *d = psram_malloc(size ? size : 1);
    if (!d) {
        f_close(&f);
        trk_message("Not enough memory for %s.", path);
        return NULL;
    }
    UINT got = 0;
    FRESULT r = FR_OK;
    uint32_t done = 0;
    while (done < size && (r = f_read(&f, d + done, size - done > 32768 ? 32768 : size - done, &got)) == FR_OK && got)
        done += got;
    f_close(&f);
    if (r != FR_OK || done != size) {
        psram_free(d);
        trk_message("Can't read %s.", path);
        return NULL;
    }
    *len = size;
    return d;
}

bool file_write_all(const char *path, const void *data, uint32_t len) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        trk_message("Can't write %s.", path);
        return false;
    }
    uint32_t done = 0;
    UINT put;
    FRESULT r = FR_OK;
    while (done < len && (r = f_write(&f, (const uint8_t *)data + done,
                                      len - done > 32768 ? 32768 : len - done, &put)) == FR_OK && put)
        done += put;
    FRESULT c = f_close(&f);
    if (r != FR_OK || c != FR_OK || done != len) {
        trk_message("Writing %s failed (is the drive full?).", path);
        return false;
    }
    return true;
}

// ---- The file picker: a disk-op panel in the tracker's style ---------------------------------
// The folder, its folders and the files the filter lets through (name,
// size, date), a scroll bar, buttons for the parent folder, the root and
// each mounted share, the filter, a name field and Load/Save and Cancel.
// Keys: arrows, PgUp/PgDn, Home/End move; Enter opens a folder or takes
// the file (or the name typed); typing edits the name; Backspace with the
// name empty goes up a folder; Tab switches the filter; Esc cancels. The
// mouse: click, double-click, the wheel, the scroll bar and the buttons.

/// The most entries in a folder.
#define FP_MAX   2000
/// The longest name.
#define FP_NAME  100
/// List rows shown.
#define FP_ROWS  17

/// A folder entry.
typedef struct {
    char name[FP_NAME];  ///< its name
    uint32_t size;  ///< its size
    uint16_t date;  ///< its FAT date
    bool dir;  ///< a folder
} fp_entry_t;

/// The folder shown (kept between pickers).
static char dir[256];
/// All files, not only those of the type.
static bool show_all;

/// The picker's box: left.
#define PX 16
#define PY 16  ///< its top
#define PW 608  ///< its width
#define PH 448  ///< its height
#define PATH_Y   (PY + 26)  ///< the path's row
#define BTN_Y    (PY + 50)  ///< the buttons' row
#define LIST_X   (PX + 8)  ///< the list: left
#define LIST_Y   (PY + 78)  ///< top
#define LIST_W   (PW - 16 - 18)  ///< width
#define BAR_X    (LIST_X + LIST_W + 2)  ///< the scroll bar's left
#define NAME_Y   (LIST_Y + FP_ROWS * 16 + 10)  ///< the name's row
#define FOOT_Y   (NAME_Y + 26)  ///< the footer's row

/// What a click hits.
enum { FB_NONE, FB_PARENT, FB_ROOT, FB_SHARE, FB_FILTER, FB_OK, FB_CANCEL, FB_UP, FB_DOWN,
       FB_TRACK, FB_LIST, FB_NAME };

/// The picker's state.
typedef struct {
    /// Its title, and the extensions shown (".MOD.").
    ///@{
    const char *title, *exts;
    ///@}
    bool save;  ///< saving (else loading)
    fp_entry_t *ent;  ///< the entries
    /// Their count, the one chosen and the first shown.
    ///@{
    int n, sel, top;
    ///@}
    char name[128];  ///< the name typed
    int cur;          ///< the cursor in the name
    char shares[2][24];  ///< the shares mounted (buttons)
    int nshares;  ///< their count
    char status[80];  ///< the status line
    bool confirm;     ///< "replace?" asked: the next OK replaces
    bool on_list;     ///< last used the list (Enter acts on it), not the name
} picker_t;

/// Whether a name has one of the extensions.
/// @param name the name
/// @param exts ".MOD." or ".WAV.IFF."
/// @return it has
static bool ext_wanted(const char *name, const char *exts) {
    const char *dot = strrchr(name, '.');
    if (dot && strlen(dot) < 10) {
        // exts is ".MOD." or ".WAV.IFF." (a trailing dot or not).
        for (const char *e = exts; *e == '.';) {
            const char *end = strchr(e + 1, '.');
            size_t n = end ? (size_t)(end - e) : strlen(e);
            if (n == strlen(dot) && !strncasecmp(dot, e, n))
                return true;
            if (!end)
                break;
            e = end;
        }
    }
    // Amiga style: MOD.name
    return !strncasecmp(name, "mod.", 4) && strstr(exts, ".MOD");
}

/// qsort's comparison: "..", folders, then names.
/// @param a an entry
/// @param b another
/// @return less, equal or more than 0
static int compare(const void *a, const void *b) {
    const fp_entry_t *x = a, *y = b;
    if (!strcmp(x->name, ".."))
        return -1;
    if (!strcmp(y->name, ".."))
        return 1;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/// A folder and a name joined.
/// @param out the path
/// @param size room in out
/// @param a the folder
/// @param b the name
static void join(char *out, int size, const char *a, const char *b) {
    snprintf(out, size, "%s%s%s", a, a[strlen(a) - 1] == '/' ? "" : "/", b);
}

/// Reads the folder.
/// @param pk the picker
/// @return read
static bool read_folder(picker_t *pk) {
    pk->n = 0;
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) {
        snprintf(pk->status, sizeof pk->status, "Can't open %.60s", dir);
        return false;
    }
    if (strcmp(dir, "/")) {
        fp_entry_t *e = &pk->ent[pk->n++];
        memset(e, 0, sizeof *e);
        strcpy(e->name, "..");
        e->dir = true;
    }
    FILINFO fi;
    int skipped = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (!strcmp(fi.fname, ".") || !strcmp(fi.fname, ".."))
            continue;
        bool is_dir = fi.fattrib & AM_DIR;
        if (!is_dir && !show_all && !ext_wanted(fi.fname, pk->exts))
            continue;
        if (pk->n == FP_MAX) {
            ++skipped;
            continue;
        }
        fp_entry_t *e = &pk->ent[pk->n++];
        snprintf(e->name, sizeof e->name, "%s", fi.fname);
        e->size = (uint32_t)fi.fsize;
        e->date = fi.fdate;
        e->dir = is_dir;
    }
    f_closedir(&d);
    qsort(pk->ent, pk->n, sizeof *pk->ent, compare);
    pk->sel = pk->top = 0;
    if (skipped)
        snprintf(pk->status, sizeof pk->status, "%d more not shown (the most is %d)", skipped, FP_MAX);
    return true;
}

/// Goes to a folder (back if it can't be read).
/// @param pk the picker
/// @param where the folder
static void go(picker_t *pk, const char *where) {
    char was[256], want[256];
    snprintf(was, sizeof was, "%s", dir);
    snprintf(want, sizeof want, "%s", where);  // where may be dir itself
    snprintf(dir, sizeof dir, "%s", want[0] ? want : "/");
    pk->status[0] = 0;
    if (!read_folder(pk)) {
        char why[80];
        snprintf(why, sizeof why, "%s", pk->status);
        snprintf(dir, sizeof dir, "%s", was);
        read_folder(pk);
        snprintf(pk->status, sizeof pk->status, "%s", why);
    }
}

/// Goes up a folder.
/// @param pk the picker
static void go_up(picker_t *pk) {
    char up[256];
    snprintf(up, sizeof up, "%s", dir);
    char *slash = strrchr(up, '/');
    if (slash && slash != up)
        *slash = 0;
    else
        strcpy(up, "/");
    go(pk, up);
}

// ---- Drawing

/// A button on the button row.
/// @param x left
/// @param w width
/// @param label its text
/// @param id what it is
/// @param lit pressed
static void fp_button(int x, int w, const char *label, int id, bool lit) {
    gfx_button(x, BTN_Y, w, 20, label, id, 0, lit);
}

/// Draws the picker.
/// @param pk it
static void draw_picker(picker_t *pk) {
    mouse_pointer_hide();
    hit_clear();
    T->full = true;  // gfx_button draws whole buttons
    gfx_bevel(PX, PY, PW, PH, true, C_FACE);
    gfx_printf(PX + 8, PY + 5, (PW - 16) / 8, C_YELLOW, C_FACE, "%s", pk->title);
    // The folder
    gfx_bevel(PX + 8, PATH_Y - 1, PW - 16, 18, false, C_FIELD);
    gfx_printf(PX + 12, PATH_Y, (PW - 24) / 8, C_CYAN, C_FIELD, "%s", dir);
    // Buttons
    int x = PX + 8;
    fp_button(x, 72, "Parent", FB_PARENT, false), x += 76;
    fp_button(x, 56, "Root", FB_ROOT, false), x += 60;
    for (int i = 0; i < pk->nshares; ++i) {
        gfx_button(x, BTN_Y, 80, 20, pk->shares[i], FB_SHARE, i, false);
        x += 84;
    }
    gfx_button(x, BTN_Y, 112, 20,
               show_all ? "All files" : strstr(pk->exts, ".MOD.XM") ? "MOD and XM" : strstr(pk->exts, ".MOD") ? "*.MOD only"
               : strstr(pk->exts, ".XM") ? "*.XM only" : "Sound files", FB_FILTER,
               0, false);
    x += 116;
    int files = 0, folders = 0;
    for (int i = 0; i < pk->n; ++i)
        pk->ent[i].dir ? ++folders : ++files;
    if (folders && !strcmp(pk->ent[0].name, ".."))
        --folders;
    gfx_printf(x + 4, BTN_Y + 2, (PX + PW - 8 - x - 4) / 8, C_LABEL, C_FACE, "%d folder%s, %d file%s",
               folders, folders == 1 ? "" : "s", files, files == 1 ? "" : "s");
    // The list
    gfx_bevel(LIST_X - 2, LIST_Y - 2, LIST_W + 4, FP_ROWS * 16 + 4, false, C_FIELD);
    if (pk->sel < pk->top)
        pk->top = pk->sel;
    if (pk->sel >= pk->top + FP_ROWS)
        pk->top = pk->sel - FP_ROWS + 1;
    for (int r = 0; r < FP_ROWS; ++r) {
        int i = pk->top + r, y = LIST_Y + r * 16;
        if (i >= pk->n) {
            gfx_fill(LIST_X, y, LIST_W, 16, C_FIELD);
            continue;
        }
        fp_entry_t *e = &pk->ent[i];
        bool on = i == pk->sel;
        uint8_t bg = on ? C_CURROW : C_FIELD, fg = e->dir ? C_YELLOW : C_TEXT;
        char size[12] = "", date[12] = "";
        if (e->dir)
            strcpy(size, strcmp(e->name, "..") ? "<DIR>" : "<UP>");
        else
            snprintf(size, sizeof size, "%9lu", (unsigned long)e->size);
        if (e->date)
            snprintf(date, sizeof date, "%04d-%02d-%02d", 1980 + (e->date >> 9), (e->date >> 5) & 15, e->date & 31);
        char shown[FP_NAME + 2];
        snprintf(shown, sizeof shown, "%s%s", e->name, e->dir && strcmp(e->name, "..") ? "/" : "");
        int cols = LIST_W / 8 - 21;
        gfx_fill(LIST_X, y, 4, 16, bg);
        gfx_text(LIST_X + 4, y, shown, cols, fg, bg);
        gfx_text(LIST_X + 4 + cols * 8, y, size, 10, e->dir ? C_YELLOW : C_CYAN, bg);
        gfx_text(LIST_X + 4 + (cols + 10) * 8, y, date, 11, C_DIM, bg);
        gfx_fill(LIST_X + 4 + (cols + 21) * 8, y, LIST_W - 4 - (cols + 21) * 8, 16, bg);
        hit_add(LIST_X, y, LIST_W, 16, FB_LIST, i);
    }
    // The scroll bar: arrows, the track and the thumb
    int track_y = LIST_Y + 16, track_h = FP_ROWS * 16 - 32;
    gfx_button(BAR_X, LIST_Y - 2, 18, 18, "\x18", FB_UP, 0, false);
    gfx_button(BAR_X, LIST_Y + FP_ROWS * 16 - 16, 18, 18, "\x19", FB_DOWN, 0, false);
    gfx_fill(BAR_X, track_y, 18, track_h, C_DARK);
    if (pk->n > FP_ROWS) {
        int th = track_h * FP_ROWS / pk->n;
        th = th < 10 ? 10 : th;
        int ty = track_y + (track_h - th) * pk->top / (pk->n - FP_ROWS);
        gfx_bevel(BAR_X, ty, 18, th, true, C_LIGHT);
    }
    hit_add(BAR_X, track_y, 18, track_h, FB_TRACK, 0);
    // The name
    gfx_text(PX + 8, NAME_Y, "Name", 5, C_LABEL, C_FACE);
    int fx = PX + 56, fw = PW - 64;
    gfx_bevel(fx, NAME_Y - 1, fw, 18, false, C_FIELD);
    int cols = (fw - 8) / 8, off = pk->cur > cols - 1 ? pk->cur - cols + 1 : 0, len = strlen(pk->name);
    for (int i = 0; i < cols; ++i) {
        int k = off + i;
        bool at = k == pk->cur;
        gfx_char(fx + 4 + i * 8, NAME_Y, k < len ? (uint8_t)pk->name[k] : ' ', at ? C_BG : C_TEXT,
                 at ? C_CURSOR : C_FIELD);
    }
    hit_add(fx, NAME_Y - 1, fw, 18, FB_NAME, off);
    // The status and the buttons
    gfx_text(PX + 8, FOOT_Y + 3, pk->status, 40, pk->confirm ? C_YELLOW : C_LABEL, C_FACE);
    gfx_button(PX + PW - 8 - 2 * 124, FOOT_Y, 120, 22, pk->save ? (pk->confirm ? "Replace" : "Save") : "Load",
               FB_OK, 0, true);
    gfx_button(PX + PW - 8 - 120, FOOT_Y, 120, 22, "Cancel", FB_CANCEL, 0, false);
    T->full = false;
    mouse_pointer_show();
}

// ---- Acting

/// Chooses an entry (its name goes into the name field).
/// @param pk the picker
/// @param i the entry
static void select_entry(picker_t *pk, int i) {
    pk->on_list = true;
    if (i < 0)
        i = 0;
    if (i >= pk->n)
        i = pk->n - 1;
    pk->sel = i < 0 ? 0 : i;
    pk->confirm = false;
    if (pk->n && !pk->ent[pk->sel].dir) {
        snprintf(pk->name, sizeof pk->name, "%s", pk->ent[pk->sel].name);
        pk->cur = strlen(pk->name);
    }
}

/// Enter (or Load/Save, or a double-click): 1 chosen, 0 carry on.
/// @param pk the picker
/// @param from_list from the list (else the name)
/// @param out the path chosen
/// @param size room in out
/// @return 1 or 0
static int act(picker_t *pk, bool from_list, char *out, int size) {
    if (from_list && pk->n) {
        fp_entry_t *e = &pk->ent[pk->sel];
        if (e->dir) {
            char next[256];
            if (!strcmp(e->name, ".."))
                go_up(pk);
            else
                join(next, sizeof next, dir, e->name), go(pk, next);
            return 0;
        }
        snprintf(pk->name, sizeof pk->name, "%s", e->name);
        pk->cur = strlen(pk->name);
    }
    if (!pk->name[0]) {
        snprintf(pk->status, sizeof pk->status, pk->save ? "Type a name to save as" : "Choose a file");
        return 0;
    }
    char path[256];
    if (pk->name[0] == '/')
        snprintf(path, sizeof path, "%s", pk->name);
    else
        join(path, sizeof path, dir, pk->name);
    FILINFO fi;
    bool exists = f_stat(path, &fi) == FR_OK;
    if (exists && (fi.fattrib & AM_DIR)) {
        go(pk, path);
        pk->name[0] = 0;
        pk->cur = 0;
        return 0;
    }
    if (pk->save) {
        if (!strchr(pk->name, '.')) {
            strncat(path, strstr(pk->exts, ".WAV") ? ".WAV" : strstr(pk->exts, ".XM") ? ".XM" : ".MOD",
                    sizeof path - strlen(path) - 1);
            exists = f_stat(path, &fi) == FR_OK;
        }
        if (exists && !pk->confirm) {
            pk->confirm = true;
            snprintf(pk->status, sizeof pk->status, "It exists: Replace it?");
            return 0;
        }
    } else if (!exists) {
        snprintf(pk->status, sizeof pk->status, "No such file");
        return 0;
    }
    snprintf(out, size, "%s", path);
    return 1;
}

/// A key in the name field.
/// @param pk the picker
/// @param k the key
static void name_key(picker_t *pk, int k) {
    int len = strlen(pk->name);
    pk->confirm = false;
    pk->on_list = false;
    if (k == '\b') {
        if (pk->cur > 0) {
            memmove(pk->name + pk->cur - 1, pk->name + pk->cur, len - pk->cur + 1);
            --pk->cur;
        }
    } else if (k == KEY_DEL) {
        if (pk->cur < len)
            memmove(pk->name + pk->cur, pk->name + pk->cur + 1, len - pk->cur);
    } else if (k == KEY_LEFT) {
        if (pk->cur > 0)
            --pk->cur;
    } else if (k == KEY_RIGHT) {
        if (pk->cur < len)
            ++pk->cur;
    } else if (k == CTRL('u')) {
        pk->name[0] = 0;
        pk->cur = 0;
    } else if (k >= 0x20 && k < 0x7f && len < (int)sizeof pk->name - 1) {
        memmove(pk->name + pk->cur + 1, pk->name + pk->cur, len - pk->cur + 1);
        pk->name[pk->cur++] = (char)k;
    }
}

bool file_pick(const char *title, const char *exts, bool save, char *out, int size) {
    picker_t *pk = psram_malloc(sizeof *pk);
    fp_entry_t *ent = psram_malloc(FP_MAX * sizeof *ent);
    if (!pk || !ent) {
        psram_free(pk);
        psram_free(ent);
        trk_message("Not enough memory.");
        return false;
    }
    memset(pk, 0, sizeof *pk);
    pk->title = title;
    pk->exts = exts;
    pk->save = save;
    pk->ent = ent;
    pk->nshares = smb_mount_points(pk->shares, 2);
    // Where to start: the last folder used, the song's, or the current one.
    if (!dir[0]) {
        if (T->path[0] == '/') {
            snprintf(dir, sizeof dir, "%s", T->path);
            char *slash = strrchr(dir, '/');
            slash == dir ? (void)(dir[1] = 0) : (void)(*slash = 0);
        } else if (f_getcwd(dir, sizeof dir) != FR_OK) {
            strcpy(dir, "/");
        }
    }
    go(pk, dir);
    pk->on_list = !save;
    if (save) {
        const char *base = strrchr(T->path, '/');
        if (strstr(exts, ".WAV")) {
            mod_sample_t *m = &T->song.samples[T->sample - 1];
            int k = 0;
            for (int i = 0; i < 22 && m->name[i] && k < 8; ++i)
                if (isalnum((unsigned char)m->name[i]))
                    pk->name[k++] = m->name[i];
            if (!k)
                k = snprintf(pk->name, sizeof pk->name, "SAMPLE%02d", T->sample);
            snprintf(pk->name + k, sizeof pk->name - k, ".WAV");
        } else if (T->path[0]) {
            snprintf(pk->name, sizeof pk->name, "%s", base ? base + 1 : T->path);
        }
        pk->cur = strlen(pk->name);
    }
    int result = -1;
    bool dragging = false;
    draw_picker(pk);
    while (result < 0) {
        bool redraw = false;
        int k = key_poll();
        if (k != KEY_NONE) {
            redraw = true;
            switch (k) {
            case KEY_ESC: result = 0; break;
            case KEY_UP: select_entry(pk, pk->sel - 1); break;
            case KEY_DOWN: select_entry(pk, pk->sel + 1); break;
            case KEY_PGUP: select_entry(pk, pk->sel - (FP_ROWS - 1)); break;
            case KEY_PGDN: select_entry(pk, pk->sel + (FP_ROWS - 1)); break;
            case KEY_HOME: select_entry(pk, 0); break;
            case KEY_END: select_entry(pk, pk->n - 1); break;
            case '\t': show_all = !show_all; go(pk, dir); break;
            case '\r':
                // The entry chosen in the list, or the name typed.
                if (act(pk, pk->on_list || !pk->name[0], out, size))
                    result = 1;
                break;
            case '\b':
                if (!pk->name[0]) {
                    go_up(pk);
                    break;
                }
                /* fall through */
            default: name_key(pk, k); break;
            }
        }
        mouse_event_t e;
        while (result < 0 && mouse_event(&e)) {
            const hit_t *h = hit_find(e.x, e.y);
            redraw = true;
            if (e.type == MOUSE_WHEEL) {
                pk->top -= e.wheel * 3;
                int most = pk->n > FP_ROWS ? pk->n - FP_ROWS : 0;
                pk->top = pk->top < 0 ? 0 : pk->top > most ? most : pk->top;
                if (pk->sel < pk->top)
                    pk->sel = pk->top;
                if (pk->sel >= pk->top + FP_ROWS)
                    pk->sel = pk->top + FP_ROWS - 1;
                continue;
            }
            if (e.type == MOUSE_UP) {
                dragging = false;
                continue;
            }
            int track_y = LIST_Y + 16, track_h = FP_ROWS * 16 - 32;
            if (e.type == MOUSE_MOVE) {
                if (dragging && (e.buttons & MOUSE_LEFT) && pk->n > FP_ROWS) {
                    int v = (e.y - track_y) * (pk->n - FP_ROWS) / (track_h > 1 ? track_h : 1);
                    pk->top = v < 0 ? 0 : v > pk->n - FP_ROWS ? pk->n - FP_ROWS : v;
                    if (pk->sel < pk->top || pk->sel >= pk->top + FP_ROWS)
                        pk->sel = pk->top;
                } else {
                    redraw = false;
                }
                continue;
            }
            if (e.button == MOUSE_RIGHT || !h)
                continue;
            switch (h->id) {
            case FB_LIST:
                select_entry(pk, h->arg);
                if (e.double_click && act(pk, true, out, size))
                    result = 1;
                break;
            case FB_PARENT: go_up(pk); break;
            case FB_ROOT: go(pk, "/"); break;
            case FB_SHARE: go(pk, pk->shares[h->arg]); break;
            case FB_FILTER: show_all = !show_all; go(pk, dir); break;
            case FB_UP: if (pk->top > 0) pk->top--; if (pk->sel >= pk->top + FP_ROWS) pk->sel--; break;
            case FB_DOWN:
                if (pk->top + FP_ROWS < pk->n)
                    pk->top++;
                if (pk->sel < pk->top)
                    pk->sel++;
                break;
            case FB_TRACK:
                dragging = true;
                if (pk->n > FP_ROWS) {
                    int v = (e.y - track_y) * (pk->n - FP_ROWS) / (track_h > 1 ? track_h : 1);
                    pk->top = v < 0 ? 0 : v > pk->n - FP_ROWS ? pk->n - FP_ROWS : v;
                    if (pk->sel < pk->top || pk->sel >= pk->top + FP_ROWS)
                        pk->sel = pk->top;
                }
                break;
            case FB_NAME: {
                pk->on_list = false;
                int c = h->arg + (e.x - (PX + 56 + 4)) / 8, len = strlen(pk->name);
                pk->cur = c < 0 ? 0 : c > len ? len : c;
                break;
            }
            case FB_OK:
                if (act(pk, (pk->on_list && pk->n && pk->ent[pk->sel].dir) || !pk->name[0], out, size))
                    result = 1;
                break;
            case FB_CANCEL: result = 0; break;
            }
        }
        if (redraw && result < 0)
            draw_picker(pk);
        mouse_pointer_update();
        sleep_ms(5);
    }
    psram_free(ent);
    psram_free(pk);
    hit_clear();
    T->full = true;
    return result == 1;
}

// ---- Songs --------------------------------------------------------------------------------------------------

bool song_load(const char *path) {
    uint32_t len;
    uint8_t *data = file_read_all(path, &len, 8u << 20);
    if (!data)
        return false;
    if (xm_is_xm(data, len)) {
        // An XM: the tracker works on it in its XM mode.
        xm_song_t *x = psram_malloc(sizeof *x);
        const char *xerr = x ? xm_load(x, data, len) : "not enough memory";
        psram_free(data);
        if (xerr) {
            if (x) {
                xm_clear(x);
                psram_free(x);
            }
            trk_message("%s: %s.", path, xerr);
            return false;
        }
        trk_stop();
        xed_take(x);
        snprintf(T->path, sizeof T->path, "%s", path);
        T->dirty = false;
        T->full = true;
        trk_message("Loaded %.20s: XM, %d channels, %d positions, %d instruments.", x->title, x->channels,
                    x->length, x->ninstruments);
        return true;
    }
    mod_song_t *s = psram_malloc(sizeof *s);
    const char *err = s ? mod_load(s, data, len) : "not enough memory";
    psram_free(data);
    if (err) {
        if (s)
            mod_clear(s);
        psram_free(s);
        trk_message("%s: %s.", path, err);
        return false;
    }
    trk_stop();
    xed_close();
    mod_clear(&T->song);
    T->song = *s;
    psram_free(s);
    undo_reset();
    snprintf(T->path, sizeof T->path, "%s", path);
    T->dirty = false;
    T->pos = T->row = T->ch = T->col = 0;
    T->pattern = T->song.orders[0];
    T->sample = 1;
    T->block = T->marking = false;
    T->speed = 6;
    T->bpm = 125;
    T->sel0 = T->sel1 = T->view0 = T->view_len = 0;
    mod_set_tempo(6, 125);
    T->full = T->wave_dirty = true;
    trk_message("Loaded %.20s: %d channels, %d positions, %d patterns%s.", T->song.title,
                T->song.channels, T->song.length, T->song.npatterns,
                T->song.nsamples == 15 ? ", 15 samples" : "");
    return true;
}

/// Whether a path ends in .XM.
/// @param path the path
/// @return it does
static bool ends_xm(const char *path) {
    size_t n = strlen(path);
    return n >= 3 && path[n - 3] == '.' && (path[n - 2] | 0x20) == 'x' && (path[n - 1] | 0x20) == 'm';
}

bool song_to_xm(void) {
    if (T->xm)
        return false;
    xm_song_t *x = psram_malloc(sizeof *x);
    const char *err = x ? xm_from_mod(x, &T->song, T->speed, T->bpm, T->separation) : "not enough memory";
    if (err) {
        if (x) {
            xm_clear(x);
            psram_free(x);
        }
        trk_message("Converting: %s.", err);
        return false;
    }
    trk_stop();
    char path[256];
    snprintf(path, sizeof path, "%s", T->path);
    char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (path[0] && dot && (!slash || dot > slash))
        snprintf(dot, sizeof path - (dot - path), ".XM");
    else if (path[0])
        snprintf(path + strlen(path), sizeof path - strlen(path), ".XM");
    int sample = T->sample;
    xed_take(x);
    T->sample = sample;
    mod_clear(&T->song);
    mod_new(&T->song, 4);
    snprintf(T->path, sizeof T->path, "%s", path);
    T->dirty = true;
    return true;
}

bool song_save(const char *path) {
    // A MOD saved with a .XM name: made into an XM first.
    if (!T->xm && ends_xm(path) && !song_to_xm())
        return false;
    if (T->xm) {
        if (!xed_save(path))
            return false;
        if (path != T->path)
            snprintf(T->path, sizeof T->path, "%s", path);
        T->dirty = false;
        T->full = true;
        return true;
    }
    size_t n = mod_save_size(&T->song);
    uint8_t *data = psram_malloc(n);
    if (!data) {
        trk_message("Not enough memory to save.");
        return false;
    }
    mod_save(&T->song, data);
    bool ok = file_write_all(path, data, n);
    psram_free(data);
    if (!ok)
        return false;
    if (path != T->path)
        snprintf(T->path, sizeof T->path, "%s", path);
    T->dirty = false;
    T->full = true;
    trk_message("Saved %s (%lu bytes).", path, (unsigned long)n);
    return true;
}

bool song_save_as(void) {
    char path[256];
    trk_dialog_begin();
    bool ok = file_pick("Save the song", T->xm ? ".XM." : ".MOD.XM.", true, path, sizeof path);
    trk_dialog_end();
    return ok && song_save(path);
}

bool song_check_saved(const char *doing) {
    if (!T->dirty)
        return true;
    char text[80];
    snprintf(text, sizeof text, "The song has changes. Save them before %s?", doing);
    trk_dialog_begin();
    int a = gui_message("Unsaved changes", text, "Save|Don't save|Cancel", false);
    trk_dialog_end();
    if (a == 0)
        return T->path[0] ? song_save(T->path) : song_save_as();
    return a == 1;
}

// ---- Help ----------------------------------------------------------------------------------------------------

/// The help page's lines (\001: a heading).
static const char *const help[] = {
    "\001PICO TRACKER - KEYS",
    "",
    "\001Everywhere",
    "F1 this help       F2 patterns       F3 samples      F4 pattern/orders/song",
    "F5 play song       F6 play pattern   F7 play from cursor     F8 stop",
    "F9 or F10 menus    F11/F12 octave down/up    [ ] edit step   - = sample",
    "< > previous/next position    Space: stop, or record (edit) mode on/off",
    "Ctrl-S save  Ctrl-O load  Ctrl-N new  Ctrl-Q or Esc quit  Ctrl-F follow",
    "Ctrl-T mute channel  Ctrl-L solo (in samples: load)  Ctrl-U unmute all",
    "Ctrl-Z undo, Ctrl-Y redo: many steps back (notes, blocks, orders, settings,",
    "samples, instruments, envelopes and the note map).",
    "",
    "\001Piano keys (the base octave and the one above)",
    "  S D   G H J   L ;          2 3   5 6 7   9 0",
    " Z X C V B N M , . /        Q W E R T Y U I O P",
    " C D E F G A B C D E        C D E F G A B C D E",
    "",
    "\001Pattern editor",
    "Arrows move, Tab next channel, PgUp/PgDn 16 rows, Home/End first/last row.",
    "With record on: a piano key writes the note and the sample; digits and A-F",
    "write the sample number, effect and parameter. The cursor moves by the step.",
    "Del clears the field; Ins/Backspace insert/delete a row in the channel;",
    "Enter plays the row. Alt-I/Alt-D insert/delete a row in all channels.",
    "Alt-B begin block, Alt-E end, Alt-L mark channel (twice: pattern), Alt-U",
    "unmark, Alt-C copy, Alt-X cut, Alt-V paste, Alt-M mix paste (fills gaps).",
    "Alt-Q/Alt-A transpose a semitone up/down, Alt-W/Alt-S an octave (the block,",
    "or the note under the cursor). Alt-N new pattern, Alt-K clone, Alt-R clear.",
    "Alt-F/Alt-G write the speed/tempo as an Fxx effect at the cursor.",
    "",
    "\001Order list (F4) and song settings (F4 again)",
    "Up/Down choose, Left/Right change the pattern (or value), Ins/Del insert or",
    "delete a position, Enter types a value (tempo, speed, title).",
    "",
    "\001Sample editor (F3)",
    "Up/Down choose a setting, Left/Right/PgUp/PgDn change it, Enter types it.",
    "Piano keys play the sample. Alt-L select all, Alt-U unselect, Alt-Z zoom to",
    "the selection, Alt-O zoom out, Alt-+/Alt-- zoom, Alt-, Alt-. scroll,",
    "Alt-X cut, Alt-K crop, Ctrl-Z undo, Ctrl-Y redo. The tools are buttons",
    "and in the Sample menu. Load reads WAV (8/16/24/32-bit, float, stereo is",
    "mixed), IFF 8SVX or raw 8-bit; Save writes an 8-bit WAV with the loop.",
    "",
    "\001XM songs",
    "File > Convert to XM (or Save as a .XM name) makes a MOD into an XM that",
    "plays the same. F3 is the instrument screen: drag across the waveform to",
    "select, drag the orange loop markers, drag envelope points (click to add,",
    "right-click one to delete), click the note map to give keys the sample.",
    "The Instrument menu (and the right button) has the sample tools for 16-bit",
    "sound, Save sample as a 16-bit WAV with its loop, .XI instrument files,",
    "copy/swap/clear instrument and envelope presets.",
    "",
    "\001Mouse",
    "Click buttons, fields, samples, positions and pattern cells. Drag in the",
    "pattern to mark a block; drag on the waveform to select, or drag the loop's",
    "S and E markers. The wheel scrolls the pattern, changes the value under the",
    "pointer, or zooms the waveform. Click a scope to mute its channel; right-",
    "click to solo it. Right-click elsewhere for a menu there. Double-click a sample",
    "to edit it, a pattern cell to switch record mode.",
    "",
    "\001Effects",
    "0xy arpeggio  1xx porta up  2xx porta down  3xx tone porta  4xy vibrato",
    "5xy 3+volume slide  6xy 4+volume slide  7xy tremolo  8xx pan  9xx offset",
    "Axy volume slide  Bxx jump  Cxx volume  Dxx break  Fxx speed (<20) / tempo",
    "E1x/E2x fine porta  E3x glissando  E4x vibrato wave  E5x finetune",
    "E6x loop  E7x tremolo wave  E8x pan  E9x retrigger  EAx/EBx fine volume",
    "ECx note cut  EDx note delay  EEx pattern delay  (EFx invert loop: ignored)",
    NULL,
};

void help_show(void) {
    int lines = 0;
    while (help[lines])
        ++lines;
    int top = 0, rows = 28;
    mouse_pointer_hide();
    gfx_fill(0, 0, DVI_WIDTH, DVI_HEIGHT, C_BG);
    for (;;) {
        for (int i = 0; i < rows; ++i) {
            int k = top + i;
            const char *s = k < lines ? help[k] : "";
            bool head = s[0] == 1;
            gfx_text(0, i * 16, head ? s + 1 : s, 80, head ? C_YELLOW : C_TEXT, head ? C_FACE : C_BG);
        }
        gfx_text(0, rows * 16, "", 80, C_TEXT, C_FACE);
        gfx_printf(0, (rows + 1) * 16, 80, C_LABEL, C_FACE,
                   " Up/Down/PgUp/PgDn scroll, Esc back            lines %d-%d of %d", top + 1,
                   top + rows < lines ? top + rows : lines, lines);
        int k = key_get();
        if (k == KEY_ESC || k == KEY_F1 || k == 'q' || k == '\r' || k == CTRL('q'))
            break;
        if (k == KEY_DOWN)
            ++top;
        else if (k == KEY_UP)
            --top;
        else if (k == KEY_PGDN || k == ' ')
            top += rows - 1;
        else if (k == KEY_PGUP)
            top -= rows - 1;
        if (top > lines - rows)
            top = lines - rows;
        if (top < 0)
            top = 0;
    }
    T->full = true;
    mouse_flush();
}
