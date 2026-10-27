/**
 * @file mc_panel.c
 * @brief mc's two panels: reading, sorting and drawing a folder listing.
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
#include <strings.h>

#include "glob.h"
#include "mc.h"
#include "psram_heap.h"
#include "smb_vfs.h"

panel_t *panels;
int active;
bool show_hidden;

const char *ent_name(const panel_t *p, const entry_t *e) {
    return p->names + e->name;
}

void panel_free(panel_t *p) {
    psram_free(p->ent);
    psram_free(p->names);
    p->ent = NULL;
    p->names = NULL;
    p->count = p->cap = 0;
    p->names_len = p->names_cap = 0;
}

/// Adds an entry to a panel.
/// @param p the panel
/// @param name its name
/// @param size its size
/// @param date its date
/// @param time its time
/// @param attr its attributes
/// @return false when out of memory
static bool add(panel_t *p, const char *name, FSIZE_t size, WORD date, WORD time, BYTE attr) {
    if (p->count == p->cap) {
        int cap = p->cap ? p->cap * 2 : 64;
        entry_t *e = psram_realloc(p->ent, cap * sizeof *e);
        if (!e)
            return false;
        p->ent = e;
        p->cap = cap;
    }
    size_t len = strlen(name) + 1;
    if (p->names_len + len > p->names_cap) {
        size_t cap = p->names_cap ? p->names_cap * 2 : 2048;
        while (cap < p->names_len + len)
            cap *= 2;
        char *n = psram_realloc(p->names, cap);
        if (!n)
            return false;
        p->names = n;
        p->names_cap = cap;
    }
    memcpy(p->names + p->names_len, name, len);
    p->ent[p->count++] = (entry_t){p->names_len, size, date, time, attr, false};
    p->names_len += len;
    return true;
}

/// The panel being sorted (for compare).
static const panel_t *sorting;

/// A name's extension.
/// @param name the name
/// @return the extension, or ""
static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : "";
}

/// qsort's comparison by the panel's sort order.
/// @param a an entry
/// @param b another
/// @return less, equal or more than 0
static int compare(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    const char *nx = ent_name(sorting, x), *ny = ent_name(sorting, y);
    // ".." first, then folders, then files.
    if (!strcmp(nx, ".."))
        return -1;
    if (!strcmp(ny, ".."))
        return 1;
    bool dx = x->attr & AM_DIR, dy = y->attr & AM_DIR;
    if (dx != dy)
        return dx ? -1 : 1;
    int r = 0;
    switch (sorting->sort) {
    case SORT_EXT: r = strcasecmp(ext_of(nx), ext_of(ny)); break;
    case SORT_SIZE: r = x->size < y->size ? 1 : x->size > y->size ? -1 : 0; break;  // big first
    case SORT_TIME: {
        uint32_t tx = (uint32_t)x->date << 16 | x->time, ty = (uint32_t)y->date << 16 | y->time;
        r = tx < ty ? 1 : tx > ty ? -1 : 0;  // newest first
        break;
    }
    }
    if (!r)
        r = strcasecmp(nx, ny);
    return sorting->reverse ? -r : r;
}

void panel_sort(panel_t *p) {
    if (p->sort == SORT_NONE || p->count < 2)
        return;
    char keep[256] = "";
    if (p->cur < p->count)
        snprintf(keep, sizeof keep, "%s", ent_name(p, &p->ent[p->cur]));
    sorting = p;
    qsort(p->ent, p->count, sizeof *p->ent, compare);
    for (int i = 0; i < p->count; ++i)
        if (!strcmp(ent_name(p, &p->ent[i]), keep))
            p->cur = i;
}

void panel_load(panel_t *p, const char *keep) {
    // Marks survive a reread for names still there.
    char **marked = NULL;
    int nmarked = 0;
    for (int i = 0; i < p->count; ++i) {
        if (!p->ent[i].marked)
            continue;
        char **bigger = realloc(marked, (nmarked + 1) * sizeof *marked);
        char *name = bigger ? strdup(ent_name(p, &p->ent[i])) : NULL;
        if (bigger)
            marked = bigger;
        if (!name)
            break;  // out of memory: those marks are lost
        marked[nmarked++] = name;
    }
    char cur_name[256] = "";
    if (keep)
        snprintf(cur_name, sizeof cur_name, "%s", keep);
    else if (p->cur < p->count)
        snprintf(cur_name, sizeof cur_name, "%s", ent_name(p, &p->ent[p->cur]));
    p->count = 0;
    p->names_len = 0;
    p->error[0] = 0;
    if (strcmp(p->path, "/"))
        add(p, "..", 0, 0, 0, AM_DIR);
    DIR dir;
    static FILINFO info;
    FRESULT r = f_opendir(&dir, p->path);
    if (r == FR_OK) {
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (!show_hidden && (info.fattrib & (AM_HID | AM_SYS)))
                continue;
            if (!add(p, info.fname, info.fsize, info.fdate, info.ftime, info.fattrib)) {
                snprintf(p->error, sizeof p->error, "too many files: not all shown");
                break;
            }
        }
        f_closedir(&dir);
    } else {
        snprintf(p->error, sizeof p->error, "can't read this folder (error %d)", r);
    }
    p->marked = 0;
    p->marked_size = 0;
    for (int i = 0; i < p->count; ++i)
        for (int k = 0; k < nmarked; ++k)
            if (!strcmp(ent_name(p, &p->ent[i]), marked[k]))
                panel_mark(p, i, true);
    for (int k = 0; k < nmarked; ++k)
        free(marked[k]);
    free(marked);
    p->cur = 0;
    sorting = p;
    if (p->sort != SORT_NONE && p->count > 1)
        qsort(p->ent, p->count, sizeof *p->ent, compare);
    for (int i = 0; i < p->count; ++i)
        if (!strcasecmp(ent_name(p, &p->ent[i]), cur_name))
            p->cur = i;
    panel_fix_scroll(p);
}

void panel_mark(panel_t *p, int i, bool on) {
    entry_t *e = &p->ent[i];
    if (e->marked == on || !strcmp(ent_name(p, e), ".."))
        return;
    e->marked = on;
    p->marked += on ? 1 : -1;
    if (on)
        p->marked_size += e->size;
    else
        p->marked_size -= e->size;
}

void panel_select_pattern(panel_t *p, const char *pattern, bool on) {
    for (int i = 0; i < p->count; ++i)
        if (!(p->ent[i].attr & AM_DIR) && glob_match(pattern, ent_name(p, &p->ent[i])))
            panel_mark(p, i, on);
}

void panel_full_path(const panel_t *p, const char *name, char *out, size_t size) {
    size_t n = strlen(p->path);
    snprintf(out, size, "%s%s%s", p->path, n && p->path[n - 1] == '/' ? "" : "/", name);
}

void panel_enter(panel_t *p, const char *name) {
    char path[256], came_from[256] = "";
    if (!strcmp(name, "..")) {
        char *slash = strrchr(p->path, '/');
        snprintf(came_from, sizeof came_from, "%s", slash ? slash + 1 : "");
        snprintf(path, sizeof path, "%.*s", slash && slash != p->path ? (int)(slash - p->path) : 1,
                 p->path);
    } else {
        panel_full_path(p, name, path, sizeof path);
    }
    DIR d;
    if (f_opendir(&d, path) != FR_OK) {
        dlg_message("Error", "Can't open that folder.", "OK", true);
        return;
    }
    f_closedir(&d);
    snprintf(p->path, sizeof p->path, "%s", path);
    for (int i = 0; i < p->count; ++i)
        p->ent[i].marked = false;
    p->count = 0;
    panel_load(p, came_from[0] ? came_from : "..");
}

// ---- Drawing --------------------------------------------------------------------------------------

/// A panel's left column.
/// @param which 0 left, 1 right
/// @return the column
static int panel_col(int which) {
    return panels[active].mode == MODE_LONG ? 0 : which * 40;
}

/// A panel's width.
/// @return the width
static int panel_width(void) {
    return panels[active].mode == MODE_LONG ? SCR_W : 40;
}

int panel_rows(const panel_t *p) {
    return p->mode == MODE_BRIEF ? PANEL_ROWS * 2 : PANEL_ROWS;
}

void panel_fix_scroll(panel_t *p) {
    if (p->cur >= p->count)
        p->cur = p->count ? p->count - 1 : 0;
    if (p->cur < 0)
        p->cur = 0;
    int rows = panel_rows(p);
    if (p->cur < p->top)
        p->top = p->cur;
    if (p->cur >= p->top + rows)
        p->top = p->cur - rows + 1;
    if (p->top < 0)
        p->top = 0;
}

void panel_key_move(panel_t *p, int key) {
    int rows = panel_rows(p);
    switch (key) {
    case KEY_UP: --p->cur; break;
    case KEY_DOWN: ++p->cur; break;
    case KEY_PGUP: p->cur -= rows - 1; break;
    case KEY_PGDN: p->cur += rows - 1; break;
    case KEY_HOME: p->cur = 0; break;
    case KEY_END: p->cur = p->count - 1; break;
    case KEY_LEFT:
        if (p->mode == MODE_BRIEF)
            p->cur -= PANEL_ROWS;
        break;
    case KEY_RIGHT:
        if (p->mode == MODE_BRIEF)
            p->cur += PANEL_ROWS;
        break;
    }
    panel_fix_scroll(p);
}

void panel_quick_search(panel_t *p, const char *text) {
    size_t n = strlen(text);
    for (int i = 0; i < p->count; ++i)
        if (!strncasecmp(ent_name(p, &p->ent[i]), text, n)) {
            p->cur = i;
            panel_fix_scroll(p);
            return;
        }
}

/// An entry's size column: bytes, \<DIR\> or UP--DIR.
/// @param out the text
/// @param width its width
/// @param e the entry
/// @param name its name
static void size_text(char *out, int width, const entry_t *e, const char *name) {
    if (!strcmp(name, ".."))
        snprintf(out, width + 1, "%*s", width, "UP--DIR");
    else if (e->attr & AM_DIR)
        snprintf(out, width + 1, "%*s", width, "<DIR>");
    else if (e->size < 10000000ull || width > 9)
        snprintf(out, width + 1, "%*llu", width, (unsigned long long)e->size);
    else if (e->size < 10000000000ull)
        snprintf(out, width + 1, "%*lluK", width - 1, (unsigned long long)(e->size >> 10));
    else
        snprintf(out, width + 1, "%*lluM", width - 1, (unsigned long long)(e->size >> 20));
}

/// An entry's date.
/// @param out the text
/// @param size room in out
/// @param date the FAT date
/// @param time the FAT time
/// @param long_form with the time
static void date_text(char *out, size_t size, WORD date, WORD time, bool long_form) {
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = (date >> 9) + 1980, m = (date >> 5) & 15, d = date & 31;
    if (!date || m < 1 || m > 12) {
        snprintf(out, size, "%s", "");
        return;
    }
    if (long_form)
        snprintf(out, size, "%04d-%02d-%02d %02d:%02d", y, m, d, time >> 11, (time >> 5) & 63);
    else
        snprintf(out, size, "%s %2d %02d:%02d", months[m - 1], d, time >> 11, (time >> 5) & 63);
}

/// An entry's colour.
/// @param p the panel
/// @param e the entry
/// @param selected under the cursor
/// @return the attribute
static uint8_t name_attr(const panel_t *p, const entry_t *e, bool selected) {
    if (selected)
        return e->marked ? C_SELMARK : C_SELECT;
    if (e->marked)
        return C_MARKED;
    if (e->attr & AM_DIR)
        return C_DIR;
    const char *ext = ext_of(ent_name(p, e));
    if (!strcasecmp(ext, "BAS"))
        return C_EXEC;
    if (e->attr & (AM_HID | AM_SYS))
        return A(DGRAY, BLUE);
    return C_PANEL;
}

/// An entry's name, cut to fit, folders with a leading '/'.
/// @param out the text
/// @param width its width
/// @param p the panel
/// @param e the entry
static void cell_name(char *out, int width, const panel_t *p, const entry_t *e) {
    const char *name = ent_name(p, e);
    bool dir = (e->attr & AM_DIR) && strcmp(name, "..");
    char full[260];
    snprintf(full, sizeof full, "%s%s", dir ? "/" : "", name);
    int len = strlen(full);
    if (len > width)  // too long: keep the start and the end, with } between (as mc)
        snprintf(out, width + 1, "%.*s}%s", width / 2, full, full + len - (width - width / 2 - 1));
    else
        snprintf(out, width + 1, "%s", full);
}

/// The free space line.
/// @param out the text
/// @param size room in out
/// @param p the panel
static void free_space(char *out, size_t size, const panel_t *p) {
    out[0] = 0;
    if (smb_path_on_share(p->path)) {
        snprintf(out, size, " network share ");
        return;
    }
    DWORD free_clusters;
    FATFS *fs;
    if (f_getfree("", &free_clusters, &fs) != FR_OK)
        return;
    uint64_t cluster = (uint64_t)fs->csize * 512;
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * cluster, free_b = free_clusters * cluster;
    snprintf(out, size, " %lluM/%lluM (%d%%) ", (unsigned long long)(free_b >> 20),
             (unsigned long long)(total >> 20), total ? (int)(free_b * 100 / total) : 0);
}

void panel_draw(int which) {
    panel_t *p = &panels[which];
    if (panels[active].mode == MODE_LONG && which != active)
        return;
    int col = panel_col(which), w = panel_width(), top = PANEL_TOP;
    if (p->mode == MODE_INFO) {
        info_draw(which, &panels[!which]);
        return;
    }
    scr_fill(top, col, PANEL_H, w, ' ', C_PANEL);
    scr_box(top, col, PANEL_H, w, true, C_PANEL);
    // The path in the top frame.
    char title[80];
    int tw = w - 6;
    int plen = strlen(p->path);
    snprintf(title, sizeof title, " %s ", plen > tw ? p->path + plen - tw : p->path);
    scr_text(top, col + (w - (int)strlen(title)) / 2, -1, which == active ? C_TITLE_ON : C_PANEL,
             title);
    int inner = w - 2, list_top = top + 2, rows = PANEL_ROWS;
    // Columns: (start, width) inside the frame.
    int name_w, size_w = 0, date_w = 0, attr_w = 0;
    if (p->mode == MODE_LONG) {
        size_w = 11;
        date_w = 16;
        attr_w = 5;
        name_w = inner - size_w - date_w - attr_w - 3;
    } else if (p->mode == MODE_FULL) {
        size_w = 7;
        date_w = 12;
        name_w = inner - size_w - date_w - 2;
    } else {
        name_w = (inner - 1) / 2;
    }
    int x0 = col + 1;
    // Headers and column lines.
    if (p->mode == MODE_BRIEF) {
        scr_text(top + 1, x0 + (name_w - 4) / 2, -1, C_HEADER, "Name");
        scr_text(top + 1, x0 + name_w + 1 + (inner - name_w - 1 - 4) / 2, -1, C_HEADER, "Name");
        for (int r = 0; r <= rows; ++r)
            scr_put(top + 1 + r, x0 + name_w, S_V, C_PANEL);
    } else {
        int c = x0;
        scr_text(top + 1, c + (name_w - 4) / 2, -1, C_HEADER, "Name");
        c += name_w;
        int widths[3] = {size_w, date_w, attr_w};
        const char *heads[3] = {"Size", "Modify time", "Attr"};
        for (int k = 0; k < 3; ++k) {
            if (!widths[k])
                continue;
            for (int r = 0; r <= rows; ++r)
                scr_put(top + 1 + r, c, S_V, C_PANEL);
            scr_text(top + 1, c + 1 + (widths[k] - (int)strlen(heads[k])) / 2, -1, C_HEADER,
                     heads[k]);
            c += widths[k] + 1;
        }
    }
    // Entries.
    for (int r = 0; r < panel_rows(p); ++r) {
        int i = p->top + r;
        int row = list_top + (p->mode == MODE_BRIEF ? r % rows : r);
        int cx = x0 + (p->mode == MODE_BRIEF && r >= rows ? name_w + 1 : 0);
        int nw = p->mode == MODE_BRIEF && r >= rows ? inner - name_w - 1 : name_w;
        if (i >= p->count)
            continue;
        entry_t *e = &p->ent[i];
        bool sel = i == p->cur && which == active;
        uint8_t at = name_attr(p, e, sel);
        char text[SCR_W + 1];
        cell_name(text, nw, p, e);
        scr_text(row, cx, nw, at, text);
        if (p->mode == MODE_BRIEF)
            continue;
        int c = cx + name_w;
        uint8_t col_at = sel ? at : (e->marked ? C_MARKED : C_PANEL);
        size_text(text, size_w, e, ent_name(p, e));
        scr_put(row, c, S_V, sel ? at : C_PANEL);
        scr_text(row, c + 1, size_w, col_at, text);
        c += size_w + 1;
        date_text(text, sizeof text, e->date, e->time, p->mode == MODE_LONG);
        scr_put(row, c, S_V, sel ? at : C_PANEL);
        scr_printf(row, c + 1, date_w, col_at, "%*s", date_w, text);
        c += date_w + 1;
        if (attr_w) {
            scr_put(row, c, S_V, sel ? at : C_PANEL);
            scr_printf(row, c + 1, attr_w, col_at, " %c%c%c%c", e->attr & AM_RDO ? 'R' : '-',
                       e->attr & AM_HID ? 'H' : '-', e->attr & AM_SYS ? 'S' : '-',
                       e->attr & AM_ARC ? 'A' : '-');
        }
    }
    // The separator, with the marked total in it, and the mini status line.
    int sep = list_top + rows;
    scr_hline(sep, col, w, DS_L, S_H, DS_R, C_PANEL);
    if (p->mode != MODE_BRIEF) {
        int c = x0 + name_w;
        int widths[3] = {size_w, date_w, attr_w};
        for (int k = 0; k < 3; ++k)
            if (widths[k]) {
                scr_put(sep, c, SS_B, C_PANEL);
                c += widths[k] + 1;
            }
    } else {
        scr_put(sep, x0 + name_w, SS_B, C_PANEL);
    }
    if (p->marked) {
        char m[60];
        snprintf(m, sizeof m, " %llu bytes in %d file%s ", (unsigned long long)p->marked_size,
                 p->marked, p->marked == 1 ? "" : "s");
        scr_text(sep, col + (w - (int)strlen(m)) / 2, -1, C_MARKED, m);
    }
    char status[SCR_W + 1] = "";
    if (p->error[0]) {
        snprintf(status, sizeof status, "%s", p->error);
    } else if (p->count && p->cur < p->count) {
        entry_t *e = &p->ent[p->cur];
        char sz[16], name[SCR_W];
        size_text(sz, 9, e, ent_name(p, e));
        cell_name(name, inner - 11, p, e);
        snprintf(status, sizeof status, "%-*s %s", inner - 11, name, sz + (sz[0] == ' ' ? 0 : 0));
    }
    scr_text(sep + 1, x0, inner, p->error[0] ? A(YELLOW, BLUE) : C_PANEL, status);
    char fs[48];
    free_space(fs, sizeof fs, p);
    scr_text(top + PANEL_H - 1, col + w - 2 - (int)strlen(fs), -1, C_PANEL, fs);
}

void info_draw(int which, const panel_t *other) {
    int col = panel_col(which), w = 40, top = PANEL_TOP;
    scr_fill(top, col, PANEL_H, w, ' ', C_PANEL);
    scr_box(top, col, PANEL_H, w, true, C_PANEL);
    scr_text(top, col + (w - 6) / 2, -1, C_PANEL, " Info ");
    int r = top + 2;
    scr_text(r++, col + 2, w - 4, C_HEADER, "Pico 2 mini OS file manager");
    scr_hline(r++, col, w, DS_L, S_H, DS_R, C_PANEL);
    if (!other->count || other->cur >= other->count) {
        scr_text(r, col + 2, w - 4, C_PANEL, "No file");
        return;
    }
    const entry_t *e = &other->ent[other->cur];
    const char *name = ent_name(other, e);
    char text[80];
    scr_printf(r++, col + 2, w - 4, C_PANEL, "File:     %s", name);
    scr_printf(r++, col + 2, w - 4, C_PANEL, "Location: %s", other->path);
    if (e->attr & AM_DIR)
        scr_text(r++, col + 2, w - 4, C_PANEL, "Size:     folder");
    else
        scr_printf(r++, col + 2, w - 4, C_PANEL, "Size:     %llu bytes", (unsigned long long)e->size);
    date_text(text, sizeof text, e->date, e->time, true);
    scr_printf(r++, col + 2, w - 4, C_PANEL, "Modified: %s", text[0] ? text : "-");
    scr_printf(r++, col + 2, w - 4, C_PANEL, "Attrib:   %s%s%s%s%s",
               e->attr & AM_RDO ? "read-only " : "", e->attr & AM_HID ? "hidden " : "",
               e->attr & AM_SYS ? "system " : "", e->attr & AM_ARC ? "archive " : "",
               e->attr & AM_DIR ? "folder" : "");
    ++r;
    if (smb_path_on_share(other->path)) {
        scr_text(r++, col + 2, w - 4, C_PANEL, "Filesystem: network share (SMB)");
    } else {
        static const char *const types[] = {"", "FAT12", "FAT16", "FAT32", "exFAT"};
        DWORD free_clusters;
        FATFS *fs;
        if (f_getfree("", &free_clusters, &fs) == FR_OK) {
            char label[24] = "";
            f_getlabel("", label, NULL);
            uint64_t cluster = (uint64_t)fs->csize * 512;
            scr_printf(r++, col + 2, w - 4, C_PANEL, "Filesystem: %s \"%s\"",
                       fs->fs_type <= 4 ? types[fs->fs_type] : "?", label);
            scr_printf(r++, col + 2, w - 4, C_PANEL, "Free:  %llu MB of %llu MB",
                       (unsigned long long)((free_clusters * cluster) >> 20),
                       (unsigned long long)(((uint64_t)(fs->n_fatent - 2) * cluster) >> 20));
            scr_printf(r++, col + 2, w - 4, C_PANEL, "Cluster: %llu KB",
                       (unsigned long long)(cluster >> 10));
        }
    }
    ++r;
    scr_printf(r++, col + 2, w - 4, C_PANEL, "Files in panel: %d", other->count);
    if (other->marked)
        scr_printf(r++, col + 2, w - 4, C_MARKED, "Marked: %d (%llu bytes)", other->marked,
                   (unsigned long long)other->marked_size);
}
