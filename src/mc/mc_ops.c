/**
 * @file mc_ops.c
 * @brief mc's file operations: copy, move, delete, mkdir, find, compare, attributes.
 *
 * They work through the normal file calls, so on the USB drive and on
 * network shares alike.
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

/// Ask before deleting, before overwriting, before quitting.
///@{
bool confirm_delete = true, confirm_overwrite = true, confirm_exit = false;
///@}

/// Copy buffer size.
#define COPY_BUF (32 * 1024)

/// Overwrite answers: ask, all, none, older ones.
enum { OW_ASK, OW_ALL, OW_NONE, OW_OLDER };

/// A copy or move.
typedef struct {
    /// A move (not a copy); stopped.
    ///@{
    bool move, abort;
    ///@}
    int overwrite;  ///< OW_ASK and the like
    /// Bytes to copy and copied.
    ///@{
    uint64_t total_bytes, done_bytes;
    ///@}
    /// Files to copy and copied.
    ///@{
    int total_files, done_files;
    ///@}
    uint8_t *buf;   ///< PSRAM, with the two files
    /// The files open.
    ///@{
    FIL *src, *dst;
    ///@}
} job_t;

/// The names in a folder, folders marked with a leading '/'.
/// @param path the folder
/// @param out set to the names (free each and it)
/// @return their count
static int list_names(const char *path, char ***out) {
    static FILINFO info;
    char **names = NULL;
    int n = 0;
    DIR dir;
    if (f_opendir(&dir, path) == FR_OK) {
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            char **bigger = realloc(names, (n + 1) * sizeof *names);
            if (!bigger)
                break;
            names = bigger;
            size_t len = strlen(info.fname);
            if (!(names[n] = malloc(len + 2)))
                break;
            names[n][0] = (info.fattrib & AM_DIR) ? '/' : ' ';
            memcpy(names[n] + 1, info.fname, len + 1);
            ++n;
        }
        f_closedir(&dir);
    }
    *out = names;
    return n;
}

/// A folder and name joined with a slash.
/// @param folder the folder
/// @param name the name
/// @return the path (malloc'd), or NULL
static char *join(const char *folder, const char *name) {
    size_t a = strlen(folder), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (p)
        snprintf(p, a + b + 2, "%s%s%s", folder, a && folder[a - 1] == '/' ? "" : "/", name);
    return p;
}

/// The files to work on: the marked ones, or the one under the cursor.
/// @param p the panel
/// @param out set to the paths (free each and it)
/// @return their count
static int sources(panel_t *p, char ***out) {
    char **list = NULL;
    int n = 0;
    for (int i = 0; i < p->count; ++i) {
        bool take = p->marked ? p->ent[i].marked : i == p->cur;
        if (!take || !strcmp(ent_name(p, &p->ent[i]), ".."))
            continue;
        char **bigger = realloc(list, (n + 1) * sizeof *list);
        if (!bigger)
            break;
        list = bigger;
        list[n++] = strdup(ent_name(p, &p->ent[i]));
    }
    *out = list;
    return n;
}

/// Counts the files and bytes under a path.
/// @param j the job
/// @param path the path
/// @param depth folders deep
static void scan(job_t *j, const char *path, int depth) {
    FILINFO info;
    if (f_stat(path, &info) != FR_OK)
        return;
    if (!(info.fattrib & AM_DIR)) {
        j->total_bytes += info.fsize;
        ++j->total_files;
        return;
    }
    if (depth > 16 || key_stop()) {
        j->abort = true;
        return;
    }
    dlg_progress("Counting the files...", path, 0, 0, 0, 0);
    char **names;
    int n = list_names(path, &names);
    for (int i = 0; i < n && !j->abort; ++i) {
        char *c = join(path, names[i] + 1);
        if (c)
            scan(j, c, depth + 1);
        free(c);
    }
    glob_free(names, n);
}

/// A file's size and date as text.
/// @param out the text
/// @param size room in out
/// @param f the file
static void fmt_info(char *out, size_t size, const FILINFO *f) {
    snprintf(out, size, "%10llu bytes, %04d-%02d-%02d %02d:%02d", (unsigned long long)f->fsize,
             (f->fdate >> 9) + 1980, (f->fdate >> 5) & 15, f->fdate & 31, f->ftime >> 11,
             (f->ftime >> 5) & 63);
}

/// May the copy overwrite `to`? Asks if need be.
/// @param j the job
/// @param src the file being copied
/// @param to where it goes
/// @return copy it
static bool may_overwrite(job_t *j, const FILINFO *src, const char *to) {
    FILINFO dst;
    if (f_stat(to, &dst) != FR_OK)
        return true;
    if (j->overwrite == OW_ALL)
        return true;
    if (j->overwrite == OW_NONE)
        return false;
    bool older = ((uint32_t)dst.fdate << 16 | dst.ftime) < ((uint32_t)src->fdate << 16 | src->ftime);
    if (j->overwrite == OW_OLDER)
        return older;
    if (!confirm_overwrite)
        return true;
    char a[60], b[60], text[400];
    fmt_info(a, sizeof a, src);
    fmt_info(b, sizeof b, &dst);
    snprintf(text, sizeof text, "%s already exists.\n\nNew:      %s\nExisting: %s\n\nOverwrite it?",
             to, a, b);
    switch (dlg_message("File exists", text, "Yes|No|All|None|Older|Abort", true)) {
    case 0: return true;
    case 2: j->overwrite = OW_ALL; return true;
    case 3: j->overwrite = OW_NONE; return false;
    case 4: j->overwrite = OW_OLDER; return older;
    case 5:
    case -1: j->abort = true; return false;
    }
    return false;
}

/// After a failure: true to skip and go on, false to stop.
/// @param j the job
/// @param what the operation ("read" and the like)
/// @param path the path
/// @param r the error
/// @return go on
static bool skip_error(job_t *j, const char *what, const char *path, FRESULT r) {
    char text[340];
    snprintf(text, sizeof text, "Can't %s\n%s\n(error %d)", what, path, r);
    if (dlg_message("Error", text, "Skip|Abort", true) == 0)
        return true;
    j->abort = true;
    return false;
}

/// Copies (or moves) one file.
/// @param j the job
/// @param from the file
/// @param to where it goes
static void copy_file(job_t *j, const char *from, const char *to) {
    FILINFO info;
    if (f_stat(from, &info) != FR_OK) {
        skip_error(j, "read", from, FR_NO_FILE);
        return;
    }
    if (glob_same(from, to)) {
        dlg_message("Error", "A file can't be copied onto itself.", "OK", true);
        return;
    }
    if (!may_overwrite(j, &info, to)) {
        j->done_bytes += info.fsize;
        return;
    }
    if (j->move) {  // the same drive: a rename does it
        FILINFO dst;
        if (f_stat(to, &dst) == FR_OK)
            f_unlink(to);
        if (f_rename(from, to) == FR_OK) {
            ++j->done_files;
            j->done_bytes += info.fsize;
            return;
        }
    }
    FIL *src = j->src, *dst = j->dst;
    FRESULT r = f_open(src, from, FA_READ);
    if (r != FR_OK) {
        skip_error(j, "read", from, r);
        return;
    }
    r = f_open(dst, to, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        f_close(src);
        skip_error(j, "create", to, r);
        return;
    }
    uint64_t done = 0;
    UINT got, put;
    char line1[SCR_W], line2[SCR_W];
    snprintf(line1, sizeof line1, "%s %s", j->move ? "Moving" : "Copying", from);
    while ((r = f_read(src, j->buf, COPY_BUF, &got)) == FR_OK && got) {
        r = f_write(dst, j->buf, got, &put);
        if (r != FR_OK || put != got) {
            if (r == FR_OK)
                r = FR_DENIED;  // drive full
            break;
        }
        done += got;
        j->done_bytes += got;
        snprintf(line2, sizeof line2, "Total: %d of %d files, %llu of %llu KB", j->done_files + 1,
                 j->total_files, (unsigned long long)(j->done_bytes >> 10),
                 (unsigned long long)(j->total_bytes >> 10));
        dlg_progress(line1, line2, done, info.fsize, j->done_bytes, j->total_bytes);
        if (key_stop()) {
            j->abort = true;
            break;
        }
    }
    f_close(src);
    f_close(dst);
    if (j->abort || r != FR_OK) {
        f_unlink(to);  // don't leave half a file
        if (!j->abort)
            skip_error(j, "write", to, r);
        return;
    }
    ++j->done_files;
    if (j->move)
        f_unlink(from);
}

/// Copies (or moves) a folder and what's in it.
/// @param j the job
/// @param from the folder
/// @param to where it goes
/// @param depth folders deep
static void copy_tree(job_t *j, const char *from, const char *to, int depth) {
    if (depth > 16) {
        dlg_message("Error", "Folders nested too deep.", "OK", true);
        j->abort = true;
        return;
    }
    if (j->move && f_rename(from, to) == FR_OK)  // the same drive: one rename
        return;
    FRESULT r = f_mkdir(to);
    if (r != FR_OK && r != FR_EXIST) {
        skip_error(j, "make folder", to, r);
        return;
    }
    char **names;
    int n = list_names(from, &names);
    for (int i = 0; i < n && !j->abort; ++i) {
        char *s = join(from, names[i] + 1), *d = join(to, names[i] + 1);
        if (s && d) {
            if (names[i][0] == '/')
                copy_tree(j, s, d, depth + 1);
            else
                copy_file(j, s, d);
        }
        free(s);
        free(d);
    }
    glob_free(names, n);
    if (j->move && !j->abort)
        f_unlink(from);  // empty now (if everything went)
}

void op_copy(bool move) {
    panel_t *p = &panels[active], *o = &panels[!active];
    char **names;
    int n = sources(p, &names);
    if (!n)
        return;
    char dest[256], prompt[320];
    snprintf(dest, sizeof dest, "%s", o->path);
    if (n == 1)
        snprintf(prompt, sizeof prompt, "%s \"%s\" to:", move ? "Move" : "Copy", names[0]);
    else
        snprintf(prompt, sizeof prompt, "%s %d files/folders to:", move ? "Move" : "Copy", n);
    if (!dlg_input(move ? "Rename/Move" : "Copy", prompt, dest, sizeof dest) || !dest[0]) {
        glob_free(names, n);
        return;
    }
    // A relative target is from the active panel's folder.
    char target[300];
    if (dest[0] == '/')
        snprintf(target, sizeof target, "%s", dest);
    else
        panel_full_path(p, dest, target, sizeof target);
    FILINFO info;
    bool to_dir = !strcmp(target, "/") || (f_stat(target, &info) == FR_OK && (info.fattrib & AM_DIR));
    if (n > 1 && !to_dir) {
        dlg_message("Error", "Several files need a folder to go into.", "OK", true);
        glob_free(names, n);
        return;
    }
    job_t j = {.move = move};
    j.buf = psram_malloc(COPY_BUF + 2 * sizeof(FIL));
    j.src = (FIL *)(j.buf + COPY_BUF);
    j.dst = j.src + 1;
    if (!j.buf) {
        glob_free(names, n);
        return;
    }
    dlg_progress_begin(move ? "Move" : "Copy");
    char from[300];
    for (int i = 0; i < n && !j.abort; ++i) {
        panel_full_path(p, names[i], from, sizeof from);
        scan(&j, from, 0);
    }
    for (int i = 0; i < n && !j.abort; ++i) {
        panel_full_path(p, names[i], from, sizeof from);
        char to[300];
        if (to_dir)
            snprintf(to, sizeof to, "%s%s%s", target,
                     target[strlen(target) - 1] == '/' ? "" : "/", names[i]);
        else
            snprintf(to, sizeof to, "%s", target);
        if (f_stat(from, &info) == FR_OK && (info.fattrib & AM_DIR)) {
            if (glob_inside(to, from)) {
                dlg_message("Error", "A folder can't be copied into itself.", "OK", true);
                continue;
            }
            copy_tree(&j, from, to, 0);
        } else {
            copy_file(&j, from, to);
        }
    }
    dlg_progress_end();
    psram_free(j.buf);
    glob_free(names, n);
    for (int i = 0; i < p->count; ++i)
        panel_mark(p, i, false);
    panel_load(p, NULL);
    panel_load(o, NULL);
}

// ---- Delete --------------------------------------------------------------------------------------

/// A delete.
typedef struct {
    /// delete non-empty folders without asking; stop
    ///@{
    bool all, abort;
    ///@}
    int done;  ///< files deleted
} del_t;

/// Deletes a folder's contents.
/// @param d the delete
/// @param path the folder
/// @param depth folders deep
static void delete_tree(del_t *d, const char *path, int depth) {
    char **names;
    int n = list_names(path, &names);
    for (int i = 0; i < n && !d->abort && depth < 16; ++i) {
        char *c = join(path, names[i] + 1);
        if (!c)
            break;
        if (names[i][0] == '/') {
            delete_tree(d, c, depth + 1);
        } else {
            dlg_progress("Deleting", c, 0, 0, 0, 0);
            if (f_unlink(c) == FR_OK)
                ++d->done;
            if (key_stop())
                d->abort = true;
        }
        free(c);
    }
    glob_free(names, n);
    if (!d->abort && f_unlink(path) == FR_OK)
        ++d->done;
}

void op_delete(void) {
    panel_t *p = &panels[active];
    char **names;
    int n = sources(p, &names);
    if (!n)
        return;
    char text[320];
    if (n == 1)
        snprintf(text, sizeof text, "Delete \"%s\"?", names[0]);
    else
        snprintf(text, sizeof text, "Delete %d files/folders?", n);
    if (confirm_delete && dlg_message("Delete", text, "Yes|No", true) != 0) {
        glob_free(names, n);
        return;
    }
    del_t d = {0};
    dlg_progress_begin("Delete");
    for (int i = 0; i < n && !d.abort; ++i) {
        char path[300];
        panel_full_path(p, names[i], path, sizeof path);
        FILINFO info;
        if (f_stat(path, &info) != FR_OK)
            continue;
        if (!(info.fattrib & AM_DIR)) {
            dlg_progress("Deleting", path, i, n, 0, 0);
            FRESULT r = f_unlink(path);
            if (r == FR_OK)
                ++d.done;
            else
                skip_error(&(job_t){0}, "delete", path, r);
            continue;
        }
        if (glob_inside("", path)) {
            dlg_message("Error", "Can't delete the current folder.", "OK", true);
            continue;
        }
        // An empty folder goes at once; a full one, after asking.
        if (f_unlink(path) == FR_OK) {
            ++d.done;
            continue;
        }
        if (!d.all) {
            snprintf(text, sizeof text, "Folder \"%s\" isn't empty.\nDelete it and everything in it?",
                     names[i]);
            int c = dlg_message("Delete", text, "Yes|No|All|Abort", true);
            if (c == 1)
                continue;
            if (c == 3 || c < 0) {
                d.abort = true;
                break;
            }
            if (c == 2)
                d.all = true;
        }
        delete_tree(&d, path, 0);
    }
    dlg_progress_end();
    glob_free(names, n);
    for (int i = 0; i < p->count; ++i)
        panel_mark(p, i, false);
    panel_load(p, NULL);
    panel_load(&panels[!active], NULL);
}

void op_mkdir(void) {
    panel_t *p = &panels[active];
    char name[128] = "";
    if (!dlg_input("Make folder", "Enter the folder's name:", name, sizeof name) || !name[0])
        return;
    char path[300];
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s", name);
    else
        panel_full_path(p, name, path, sizeof path);
    FRESULT r = f_mkdir(path);
    if (r != FR_OK) {
        char text[340];
        snprintf(text, sizeof text, "Can't make %s\n(%s)", path,
                 r == FR_EXIST ? "it already exists" : "error");
        dlg_message("Error", text, "OK", true);
        return;
    }
    const char *slash = strrchr(name, '/');
    panel_load(p, slash ? slash + 1 : name);
    panel_load(&panels[!active], NULL);
}

// ---- Find file -------------------------------------------------------------------------------------

/// A find.
typedef struct {
    /// The file name pattern; the text looked for (or NULL).
    ///@{
    const char *pattern, *text;
    ///@}
    char **found;  ///< the paths found
    int n;  ///< their count
    bool abort;  ///< stopped
} find_t;

/// The search's file and buffer (PSRAM, while op_find runs).
static FIL *find_file;
static char *find_buf;  ///< its buffer
/// The buffer's size.
#define FIND_BUF 2048

/// Whether a file contains text.
/// @param path the file
/// @param text the text
/// @return it does
static bool file_has(const char *path, const char *text) {
    FIL *f = find_file;
    if (f_open(f, path, FA_READ) != FR_OK)
        return false;
    size_t tl = strlen(text);
    char *buf = find_buf;
    UINT got;
    size_t keep = 0;
    bool hit = false;
    while (!hit && f_read(f, buf + keep, FIND_BUF - 1 - keep, &got) == FR_OK && got) {
        size_t len = keep + got;
        buf[len] = 0;
        for (size_t i = 0; i + tl <= len && !hit; ++i)
            if (!strncasecmp(buf + i, text, tl))
                hit = true;
        keep = tl - 1 < len ? tl - 1 : len;  // a match across reads
        memmove(buf, buf + len - keep, keep);
    }
    f_close(f);
    return hit;
}

/// Finds matching files under a path.
/// @param f the find
/// @param path the folder
/// @param depth folders deep
static void find_in(find_t *f, const char *path, int depth) {
    if (depth > 16 || f->n >= 500)
        return;
    dlg_progress("Searching", path, 0, 0, 0, 0);
    if (key_stop()) {
        f->abort = true;
        return;
    }
    char **names;
    int n = list_names(path, &names);
    for (int i = 0; i < n && !f->abort; ++i) {
        char *c = join(path, names[i] + 1);
        if (!c)
            break;
        bool dir = names[i][0] == '/';
        if (glob_match(f->pattern, names[i] + 1) && (dir ? !f->text[0] : (!f->text[0] || file_has(c, f->text)))) {
            char **bigger = realloc(f->found, (f->n + 1) * sizeof *f->found);
            if (bigger) {
                f->found = bigger;
                f->found[f->n++] = strdup(c);
            }
        }
        if (dir)
            find_in(f, c, depth + 1);
        free(c);
    }
    glob_free(names, n);
}

void op_find(void) {
    panel_t *p = &panels[active];
    static char start[256], pattern[64] = "*", text[64];
    snprintf(start, sizeof start, "%s", p->path);
    if (!dlg_input("Find file", "Start at:", start, sizeof start) ||
        !dlg_input("Find file", "File name (* and ? work):", pattern, sizeof pattern) ||
        !dlg_input("Find file", "Containing text (empty: any):", text, sizeof text))
        return;
    find_t f = {pattern[0] ? pattern : "*", text, NULL, 0, false};
    find_file = psram_malloc(sizeof *find_file + FIND_BUF);
    if (!find_file)
        return;
    find_buf = (char *)(find_file + 1);
    dlg_progress_begin("Find file");
    find_in(&f, start, 0);
    dlg_progress_end();
    psram_free(find_file);
    find_file = NULL;
    if (!f.n) {
        dlg_message("Find file", f.abort ? "Stopped: nothing found yet." : "Nothing found.", "OK",
                    false);
        return;
    }
    char title[48];
    snprintf(title, sizeof title, "Found %d%s", f.n, f.abort ? " (stopped)" : "");
    int i = dlg_list(title, f.found, f.n, 0);
    if (i >= 0) {
        // Go there: the folder in the active panel, the cursor on the file.
        char *slash = strrchr(f.found[i], '/');
        snprintf(p->path, sizeof p->path, "%.*s", slash && slash != f.found[i] ? (int)(slash - f.found[i]) : 1,
                 f.found[i]);
        p->count = 0;
        panel_load(p, slash ? slash + 1 : f.found[i]);
    }
    glob_free(f.found, f.n);
}

// ---- Compare and attributes ---------------------------------------------------------------------

void op_compare(void) {
    panel_t *a = &panels[0], *b = &panels[1];
    for (int i = 0; i < a->count; ++i)
        panel_mark(a, i, false);
    for (int i = 0; i < b->count; ++i)
        panel_mark(b, i, false);
    // Marks files missing from the other side, or newer, or with another size.
    for (int side = 0; side < 2; ++side) {
        panel_t *x = side ? b : a, *y = side ? a : b;
        for (int i = 0; i < x->count; ++i) {
            entry_t *e = &x->ent[i];
            if (e->attr & AM_DIR)
                continue;
            int k = 0;
            while (k < y->count && strcasecmp(ent_name(x, e), ent_name(y, &y->ent[k])))
                ++k;
            if (k == y->count) {
                panel_mark(x, i, true);
                continue;
            }
            entry_t *o = &y->ent[k];
            uint32_t te = (uint32_t)e->date << 16 | e->time, to = (uint32_t)o->date << 16 | o->time;
            if (te > to || (te == to && e->size != o->size))
                panel_mark(x, i, true);
        }
    }
}

void op_attributes(void) {
    panel_t *p = &panels[active];
    if (smb_path_on_share(p->path)) {
        dlg_message("Attributes", "Attributes can only be changed on the USB drive.", "OK", false);
        return;
    }
    if (!p->count || !strcmp(ent_name(p, &p->ent[p->cur]), ".."))
        return;
    entry_t *e = &p->ent[p->cur];
    static const struct {
        BYTE bit;
        const char *name;
    } bits[] = {{AM_RDO, "Read-only"}, {AM_HID, "Hidden"}, {AM_SYS, "System"}, {AM_ARC, "Archive"}};
    BYTE attr = e->attr;
    int sel = 0;
    void *saved = scr_save();
    int w = 40, h = 10, row = 9, col = 20;
    for (;;) {
        scr_fill(row, col, h, w, ' ', C_DIALOG);
        scr_box(row + 1, col + 2, h - 2, w - 4, true, C_DIALOG);
        scr_text(row + 1, col + 13, -1, C_DLG_TITLE, " Attributes ");
        for (int i = 0; i < 4; ++i)
            scr_printf(row + 2 + i, col + 5, 30, i == sel ? C_DLG_FOCUS : C_DIALOG, "[%c] %s",
                       attr & bits[i].bit ? 'x' : ' ', bits[i].name);
        scr_text(row + 7, col + 5, 30, A(DGRAY, LGRAY), "Space: change  Enter: set");
        scr_cursor(-1, 0);
        scr_flush();
        int k = key_get();
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        if (k == KEY_UP)
            sel = (sel + 3) % 4;
        else if (k == KEY_DOWN || k == '\t')
            sel = (sel + 1) % 4;
        else if (k == ' ')
            attr ^= bits[sel].bit;
        else if (k == '\r') {
            for (int i = 0; i < p->count; ++i) {
                bool take = p->marked ? p->ent[i].marked : i == p->cur;
                if (!take || !strcmp(ent_name(p, &p->ent[i]), ".."))
                    continue;
                char path[300];
                panel_full_path(p, ent_name(p, &p->ent[i]), path, sizeof path);
                f_chmod(path, attr, AM_RDO | AM_HID | AM_SYS | AM_ARC);
            }
            break;
        }
    }
    scr_restore(saved);
    panel_load(p, NULL);
}
