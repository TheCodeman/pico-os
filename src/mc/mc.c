/**
 * @file mc.c
 * @brief mc: the main loop, command line, menus, options and help.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "mc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "console.h"
#include "editor.h"
#include "glob.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "shell.h"

#define INI_FILE  "/MC.INI"
#define MENU_FILE "/MC.MNU"

static char cmd[256];
static int cmd_pos;
static bool searching;  ///< quick search (Ctrl-S)
static char search[40];
static bool quit;

// ---- Drawing ---------------------------------------------------------------------------------------

static void draw_menubar(void) {
    scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
    scr_text(0, 2, -1, C_MENU, "  Left     File     Command     Options     Right");
}

static void draw_keybar(void) {
    static const char *const labels[10] = {"Help",  "Menu",  "View",   "Edit",   "Copy",
                                           "RenMov", "Mkdir", "Delete", "PullDn", "Quit"};
    int c = 0;
    for (int i = 0; i < 10; ++i) {
        int w = i == 9 ? SCR_W - c : 8;
        scr_printf(SCR_H - 1, c, 2, C_KEYNUM, "%2d", i + 1);
        scr_text(SCR_H - 1, c + 2, w - 2, C_KEYTEXT, labels[i]);
        c += 8;
    }
}

static void draw_cmdline(void) {
    int row = SCR_H - 2;
    scr_fill(row, 0, 1, SCR_W, ' ', C_CMDLINE);
    if (searching) {
        scr_printf(row, 0, SCR_W, A(BLACK, CYAN), "Search: %s", search);
        scr_cursor(row, 8 + (int)strlen(search));
        return;
    }
    char prompt[48];
    const char *path = panels[active].path;
    int pl = strlen(path);
    snprintf(prompt, sizeof prompt, "%s%s> ", pl > 30 ? "..." : "", pl > 30 ? path + pl - 30 : path);
    int start = strlen(prompt), room = SCR_W - start - 1;
    int off = cmd_pos > room ? cmd_pos - room : 0;
    scr_text(row, 0, -1, C_CMDLINE, prompt);
    scr_text(row, start, room, C_CMDLINE, cmd + off);
    scr_cursor(row, start + cmd_pos - off);
}

void mc_draw(void) {
    draw_menubar();
    panel_draw(0);
    panel_draw(1);
    draw_cmdline();
    draw_keybar();
}

void mc_redraw(void) {
    mc_draw();
    scr_flush();
}

// ---- Running things ---------------------------------------------------------------------------------

/// Runs a shell command on the shell's own screen, then comes back. The
/// command runs in the active panel's folder; a cd there moves the panel.
void run_command(const char *line, bool pause) {
    panel_t *p = &panels[active];
    f_chdir(p->path);
    scr_show_user();
    printf("\r\n\x1b[0m%s> %s\r\n", p->path, line);
    fflush(stdout);
    shell_exec(line);
    console_reset_modes();
    if (pause) {
        printf("\x1b[0m\r\nPress any key to return to mc ");
        fflush(stdout);
        key_get();
        printf("\r\n");
    }
    fflush(stdout);
    printf("\x1b[?25l");
    fflush(stdout);
    scr_grab_user();
    char cwd[256];
    if (f_getcwd(cwd, sizeof cwd) == FR_OK && strcmp(cwd, p->path)) {
        snprintf(p->path, sizeof p->path, "%s", cwd);
        p->count = 0;
    }
    panel_load(&panels[0], NULL);
    panel_load(&panels[1], NULL);
}

static bool has_ext(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (; *exts; ++exts)
        if (!strcasecmp(dot + 1, *exts))
            return true;
    return false;
}

static void quoted(char *out, size_t size, const char *verb, const char *path) {
    snprintf(out, size, strchr(path, ' ') ? "%s \"%s\"" : "%s %s", verb, path);
}

/// Enter on a file: BASIC programs run, pictures are shown, the rest viewed.
static void open_entry(void) {
    panel_t *p = &panels[active];
    if (!p->count)
        return;
    entry_t *e = &p->ent[p->cur];
    const char *name = ent_name(p, e);
    if (e->attr & AM_DIR) {
        panel_enter(p, name);
        return;
    }
    char path[300], line[320];
    panel_full_path(p, name, path, sizeof path);
    static const char *const basic_ext[] = {"BAS", NULL};
    static const char *const image_ext[] = {"BMP", "PNG", "JPG", "JPEG", NULL};
    if (has_ext(name, basic_ext)) {
        quoted(line, sizeof line, "basic", path);
        run_command(line, true);
    } else if (has_ext(name, image_ext)) {
        quoted(line, sizeof line, "view", path);
        run_command(line, false);
    } else {
        view_file(path);
    }
}

static void edit_entry(void) {
    panel_t *p = &panels[active];
    if (!p->count || (p->ent[p->cur].attr & AM_DIR))
        return;
    char path[300], line[320];
    panel_full_path(p, ent_name(p, &p->ent[p->cur]), path, sizeof path);
    quoted(line, sizeof line, "edit", path);
    run_command(line, false);
}

static void view_entry(void) {
    panel_t *p = &panels[active];
    if (!p->count)
        return;
    entry_t *e = &p->ent[p->cur];
    if (e->attr & AM_DIR) {
        panel_enter(p, ent_name(p, e));
        return;
    }
    char path[300];
    panel_full_path(p, ent_name(p, e), path, sizeof path);
    view_file(path);
}

// ---- Settings ---------------------------------------------------------------------------------------

static void save_setup(bool tell) {
    FIL f;
    if (f_open(&f, INI_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        if (tell)
            dlg_message("Save setup", "Can't write " INI_FILE ".", "OK", true);
        return;
    }
    for (int i = 0; i < 2; ++i) {
        const char *side = i ? "right" : "left";
        f_printf(&f, "%s_path=%s\n%s_mode=%d\n%s_sort=%d\n%s_reverse=%d\n", side, panels[i].path,
                 side, panels[i].mode, side, panels[i].sort, side, panels[i].reverse);
    }
    f_printf(&f, "active=%d\nhidden=%d\nconfirm_delete=%d\nconfirm_overwrite=%d\nconfirm_exit=%d\n",
             active, show_hidden, confirm_delete, confirm_overwrite, confirm_exit);
    f_close(&f);
    if (tell)
        dlg_message("Save setup", "Saved in " INI_FILE ".", "OK", false);
}

static void load_setup(void) {
    FIL f;
    if (f_open(&f, INI_FILE, FA_READ) != FR_OK)
        return;
    char line[300];
    while (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        int n = atoi(v);
        for (int i = 0; i < 2; ++i) {
            const char *side = i ? "right_" : "left_";
            size_t sl = strlen(side);
            if (strncmp(line, side, sl))
                continue;
            const char *key = line + sl;
            if (!strcmp(key, "path"))
                snprintf(panels[i].path, sizeof panels[i].path, "%s", v);
            else if (!strcmp(key, "mode") && n >= 0 && n <= MODE_INFO)
                panels[i].mode = n;
            else if (!strcmp(key, "sort") && n >= 0 && n <= SORT_NONE)
                panels[i].sort = n;
            else if (!strcmp(key, "reverse"))
                panels[i].reverse = n;
        }
        if (!strcmp(line, "active"))
            active = n & 1;
        else if (!strcmp(line, "hidden"))
            show_hidden = n;
        else if (!strcmp(line, "confirm_delete"))
            confirm_delete = n;
        else if (!strcmp(line, "confirm_overwrite"))
            confirm_overwrite = n;
        else if (!strcmp(line, "confirm_exit"))
            confirm_exit = n;
    }
    f_close(&f);
}

// ---- Menus, help, user menu ------------------------------------------------------------------------

enum {
    M_FULL = 1, M_BRIEF, M_LONG, M_INFO, M_SORT, M_HIDDEN, M_RESCAN,  ///< M_FULL..M_INFO: 1 + MODE_*
    M_VIEW, M_EDIT, M_COPY, M_MOVE, M_MKDIR, M_DELETE, M_SELECT, M_UNSELECT, M_INVERT, M_ATTR,
    M_QUIT, M_FIND, M_SWAP, M_COMPARE, M_SHELL, M_QUICKCD, M_EDITMENU, M_CONF_DEL, M_CONF_OVER,
    M_CONF_EXIT, M_SAVE,
};

static void select_group(bool on) {
    static char pattern[64] = "*";
    if (dlg_input(on ? "Select group" : "Unselect group", "Files matching (* and ?):", pattern,
                  sizeof pattern) && pattern[0])
        panel_select_pattern(&panels[active], pattern, on);
}

static void invert(void) {
    panel_t *p = &panels[active];
    for (int i = 0; i < p->count; ++i)
        if (!(p->ent[i].attr & AM_DIR))
            panel_mark(p, i, !p->ent[i].marked);
}

static void sort_dialog(panel_t *p) {
    static char *const items[] = {"Name", "Extension", "Size", "Modify time", "Unsorted",
                                  "Reverse order on/off"};
    int i = dlg_list("Sort order", items, 6, p->sort);
    if (i == 5)
        p->reverse = !p->reverse;
    else if (i >= 0)
        p->sort = i;
    if (i >= 0)
        panel_load(p, NULL);
}

static void quick_cd(void) {
    char where[256] = "";
    if (!dlg_input("Quick cd", "Go to folder:", where, sizeof where) || !where[0])
        return;
    panel_t *p = &panels[active];
    char path[300];
    if (where[0] == '/')
        snprintf(path, sizeof path, "%s", where);
    else
        panel_full_path(p, where, path, sizeof path);
    glob_absolute(path, path, sizeof path);
    DIR d;
    if (f_opendir(&d, path) != FR_OK) {
        dlg_message("Quick cd", "No such folder.", "OK", true);
        return;
    }
    f_closedir(&d);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->count = 0;
    panel_load(p, "..");
}

static void help(void) {
    dlg_message("mc - keys",
                "Tab        other panel         Enter  open folder / run .BAS / view\n"
                "Insert, Space (empty line)  mark   + - *  select, unselect, invert\n"
                "F3 view   F4 edit   F5 copy   F6 move/rename   F7 mkdir   F8 delete\n"
                "F2 user menu (/MC.MNU)   F9 menus   F10 quit   Esc 1..0 = F1..F10\n"
                "Ctrl-S quick search   Ctrl-O shell screen   Ctrl-U swap panels\n"
                "Ctrl-R reread   Alt-Enter put name in command   Alt-C quick cd\n"
                "Alt-? find file   Alt-T listing mode   Alt-H hidden files\n"
                "Typing goes to the command line; Enter runs it in this folder.",
                "OK", false);
}

static const char default_menu[] =
    "# mc user menu (F2): Title|command; %f file, %n name, %d folder\n"
    "Run in BASIC|basic %f\n"
    "Edit|edit %f\n"
    "Show picture|view %f\n"
    "Disk free|df\n"
    "Memory|mem\n"
    "Copy to the other panel|cp %f %o\n";

static void user_menu(void) {
    FIL f;
    if (f_open(&f, MENU_FILE, FA_READ) != FR_OK) {
        if (dlg_message("User menu", "There is no " MENU_FILE " yet. Make one with examples?",
                        "Yes|No", false) != 0)
            return;
        if (f_open(&f, MENU_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw;
            f_write(&f, default_menu, sizeof default_menu - 1, &bw);
            f_close(&f);
        }
        if (f_open(&f, MENU_FILE, FA_READ) != FR_OK)
            return;
    }
    char *titles[32], *cmds[32];
    int n = 0;
    char line[256];
    while (n < 32 && f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *bar = strchr(line, '|');
        if (line[0] == '#' || !bar)
            continue;
        *bar = 0;
        titles[n] = strdup(line);
        cmds[n] = strdup(bar + 1);
        ++n;
    }
    f_close(&f);
    int i = dlg_list("User menu", titles, n, 0);
    if (i >= 0) {
        // %f the file, %n its name, %d the folder, %o the other panel's folder.
        panel_t *p = &panels[active];
        char file[300] = "", out[320];
        const char *name = p->count ? ent_name(p, &p->ent[p->cur]) : "";
        panel_full_path(p, name, file, sizeof file);
        size_t k = 0;
        for (const char *c = cmds[i]; *c && k < sizeof out - 1; ++c) {
            const char *sub = NULL;
            if (c[0] == '%' && c[1]) {
                switch (c[1]) {
                case 'f': sub = file; break;
                case 'n': sub = name; break;
                case 'd': sub = p->path; break;
                case 'o': sub = panels[!active].path; break;
                }
            }
            if (sub) {
                k += snprintf(out + k, sizeof out - k, "%s", sub);
                ++c;
            } else {
                out[k++] = *c;
            }
        }
        out[k < sizeof out ? k : sizeof out - 1] = 0;
        run_command(out, true);
    }
    for (int k = 0; k < n; ++k) {
        free(titles[k]);
        free(cmds[k]);
    }
}

static void do_menu_item(int id) {
    panel_t *p = &panels[active];
    switch (id) {
    case M_BRIEF: p->mode = MODE_BRIEF; break;
    case M_FULL: p->mode = MODE_FULL; break;
    case M_LONG: p->mode = MODE_LONG; break;
    case M_INFO: p->mode = MODE_INFO; break;
    case M_SORT: sort_dialog(p); break;
    case M_HIDDEN:
        show_hidden = !show_hidden;
        panel_load(&panels[0], NULL);
        panel_load(&panels[1], NULL);
        break;
    case M_RESCAN:
        panel_load(&panels[0], NULL);
        panel_load(&panels[1], NULL);
        break;
    case M_VIEW: view_entry(); break;
    case M_EDIT: edit_entry(); break;
    case M_COPY: op_copy(false); break;
    case M_MOVE: op_copy(true); break;
    case M_MKDIR: op_mkdir(); break;
    case M_DELETE: op_delete(); break;
    case M_SELECT: select_group(true); break;
    case M_UNSELECT: select_group(false); break;
    case M_INVERT: invert(); break;
    case M_ATTR: op_attributes(); break;
    case M_QUIT: quit = true; break;
    case M_FIND: op_find(); break;
    case M_SWAP: {
        panel_t t = panels[0];
        panels[0] = panels[1];
        panels[1] = t;
        break;
    }
    case M_COMPARE: op_compare(); break;
    case M_SHELL:
        scr_show_user();
        key_get();
        printf("\x1b[?25l");
        fflush(stdout);
        scr_grab_user();
        break;
    case M_QUICKCD: quick_cd(); break;
    case M_EDITMENU: run_command("edit " MENU_FILE, false); break;
    case M_CONF_DEL: confirm_delete = !confirm_delete; break;
    case M_CONF_OVER: confirm_overwrite = !confirm_overwrite; break;
    case M_CONF_EXIT: confirm_exit = !confirm_exit; break;
    case M_SAVE: save_setup(true); break;
    }
}

static void pull_down(int open) {
    panel_t *p = &panels[active];
    menu_item_t left[] = {
        {"Brief", M_BRIEF, NULL, p->mode == MODE_BRIEF},
        {"Full", M_FULL, NULL, p->mode == MODE_FULL},
        {"Long", M_LONG, NULL, p->mode == MODE_LONG},
        {"Info", M_INFO, NULL, p->mode == MODE_INFO},
        {"-", 0, NULL, false},
        {"Sort order...", M_SORT, NULL, false},
        {"Show hidden files", M_HIDDEN, "Alt-H", show_hidden},
        {"-", 0, NULL, false},
        {"Rescan", M_RESCAN, "Ctrl-R", false},
        {NULL, 0, NULL, false},
    };
    menu_item_t file[] = {
        {"View", M_VIEW, "F3", false},        {"Edit", M_EDIT, "F4", false},
        {"Copy", M_COPY, "F5", false},        {"Rename/Move", M_MOVE, "F6", false},
        {"Make folder", M_MKDIR, "F7", false}, {"Delete", M_DELETE, "F8", false},
        {"-", 0, NULL, false},                {"Select group", M_SELECT, "+", false},
        {"Unselect group", M_UNSELECT, "-", false}, {"Invert selection", M_INVERT, "*", false},
        {"Attributes...", M_ATTR, NULL, false}, {"-", 0, NULL, false},
        {"Quit", M_QUIT, "F10", false},       {NULL, 0, NULL, false},
    };
    menu_item_t command[] = {
        {"Find file", M_FIND, "Alt-?", false},  {"Swap panels", M_SWAP, "Ctrl-U", false},
        {"Compare folders", M_COMPARE, NULL, false}, {"Shell screen", M_SHELL, "Ctrl-O", false},
        {"Quick cd", M_QUICKCD, "Alt-C", false}, {"Edit user menu", M_EDITMENU, NULL, false},
        {NULL, 0, NULL, false},
    };
    menu_item_t options[] = {
        {"Confirm delete", M_CONF_DEL, NULL, confirm_delete},
        {"Confirm overwrite", M_CONF_OVER, NULL, confirm_overwrite},
        {"Confirm exit", M_CONF_EXIT, NULL, confirm_exit},
        {"Show hidden files", M_HIDDEN, NULL, show_hidden},
        {"-", 0, NULL, false},
        {"Save setup", M_SAVE, NULL, false},
        {NULL, 0, NULL, false},
    };
    // The Left and Right menus are the same, for their own panel: their ids
    // are offset by 200 and 300.
    menu_item_t right[sizeof left / sizeof *left];
    memcpy(right, left, sizeof left);
    for (int i = 0; left[i].text; ++i) {
        if (!left[i].id || left[i].id == M_HIDDEN)
            continue;
        right[i].check = left[i].id <= M_INFO && panels[1].mode == left[i].id - M_FULL;
        left[i].check = left[i].id <= M_INFO && panels[0].mode == left[i].id - M_FULL;
        left[i].id += 200;
        right[i].id += 300;
    }
    menu_t menus[] = {{"Left", left}, {"File", file}, {"Command", command},
                      {"Options", options}, {"Right", right}};
    int id = menu_run(menus, 5, open);
    if (id > 200) {
        int was = active;
        active = id > 300;
        do_menu_item(id - (id > 300 ? 300 : 200));
        if (panels[was].mode != MODE_INFO)
            active = was;
        else
            active = !was;
    } else if (id > 0) {
        do_menu_item(id);
    }
}

// ---- The loop -------------------------------------------------------------------------------------------

static void cmd_insert(int c) {
    int len = strlen(cmd);
    if (len >= (int)sizeof cmd - 1)
        return;
    memmove(cmd + cmd_pos + 1, cmd + cmd_pos, len - cmd_pos + 1);
    cmd[cmd_pos++] = c;
}

static void handle(int k) {
    panel_t *p = &panels[active];
    if (searching) {
        if (k >= 0x20 && k < 0x7f) {
            size_t n = strlen(search);
            if (n < sizeof search - 1) {
                search[n] = k;
                search[n + 1] = 0;
                panel_quick_search(p, search);
            }
            return;
        }
        if (k == '\b') {
            size_t n = strlen(search);
            if (n)
                search[n - 1] = 0;
            return;
        }
        if (k == CTRL('s')) {  // the next match
            for (int i = p->cur + 1; i < p->count; ++i)
                if (!strncasecmp(ent_name(p, &p->ent[i]), search, strlen(search))) {
                    p->cur = i;
                    panel_fix_scroll(p);
                    break;
                }
            return;
        }
        searching = false;
        if (k == KEY_ESC || k == '\r')
            return;
    }
    bool line_empty = !cmd[0];
    switch (k) {
    case '\t':
        active = !active;
        if (panels[active].mode == MODE_INFO)
            active = !active;
        return;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PGUP:
    case KEY_PGDN:
        panel_key_move(p, k);
        return;
    case KEY_HOME:
    case KEY_END:
    case KEY_LEFT:
    case KEY_RIGHT:
        if (!line_empty) {
            int len = strlen(cmd);
            cmd_pos = k == KEY_HOME ? 0 : k == KEY_END ? len
                    : k == KEY_LEFT ? (cmd_pos ? cmd_pos - 1 : 0)
                                    : (cmd_pos < len ? cmd_pos + 1 : len);
        } else {
            panel_key_move(p, k);
        }
        return;
    case '\r':
        if (!line_empty) {
            char line[256];
            snprintf(line, sizeof line, "%s", cmd);
            cmd[0] = 0;
            cmd_pos = 0;
            if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
                quit = true;
                return;
            }
            run_command(line, false);
        } else {
            open_entry();
        }
        return;
    case KEY_ALT + '\r':
        if (p->count) {
            const char *name = ent_name(p, &p->ent[p->cur]);
            for (const char *c = name; *c; ++c)
                cmd_insert(*c);
            cmd_insert(' ');
        }
        return;
    case '\b':
        if (cmd_pos) {
            memmove(cmd + cmd_pos - 1, cmd + cmd_pos, strlen(cmd) - cmd_pos + 1);
            --cmd_pos;
        }
        return;
    case KEY_DEL:
        if (cmd[cmd_pos])
            memmove(cmd + cmd_pos, cmd + cmd_pos + 1, strlen(cmd) - cmd_pos);
        return;
    case KEY_INS:
    case CTRL('t'):
        if (p->count) {
            panel_mark(p, p->cur, !p->ent[p->cur].marked);
            panel_key_move(p, KEY_DOWN);
        }
        return;
    case KEY_F1: help(); return;
    case KEY_F2: user_menu(); return;
    case KEY_F3: view_entry(); return;
    case KEY_F4: edit_entry(); return;
    case KEY_F5: op_copy(false); return;
    case KEY_F6: op_copy(true); return;
    case KEY_F7: op_mkdir(); return;
    case KEY_F8: op_delete(); return;
    case KEY_F9: pull_down(active ? 4 : 0); return;
    case KEY_F10:
        if (!confirm_exit || dlg_message("Quit", "Leave mc?", "Yes|No", false) == 0)
            quit = true;
        return;
    case CTRL('o'): do_menu_item(M_SHELL); return;
    case CTRL('u'): do_menu_item(M_SWAP); return;
    case CTRL('r'): do_menu_item(M_RESCAN); return;
    case CTRL('s'):
        searching = true;
        search[0] = 0;
        return;
    case CTRL('c'):
        cmd[0] = 0;
        cmd_pos = 0;
        return;
    case KEY_ALT + 'c': quick_cd(); return;
    case KEY_ALT + '?': op_find(); return;
    case KEY_ALT + 'h': do_menu_item(M_HIDDEN); return;
    case KEY_ALT + 't':
        p->mode = p->mode == MODE_FULL ? MODE_BRIEF : p->mode == MODE_BRIEF ? MODE_LONG : MODE_FULL;
        return;
    case KEY_ESC:
        cmd[0] = 0;
        cmd_pos = 0;
        return;
    }
    if (line_empty && k == ' ' && p->count) {
        handle(KEY_INS);
        return;
    }
    if (line_empty && k == '+') {
        select_group(true);
        return;
    }
    if (line_empty && k == '-') {
        select_group(false);
        return;
    }
    if (line_empty && k == '*') {
        invert();
        return;
    }
    if (k >= 0x20 && k < 0x100)
        cmd_insert(k);
}

/// The mouse: click an entry to choose it (double-click opens it, the right
/// button marks it), the wheel scrolls, the menu bar and the F-key bar work.
static void mouse_click(const mouse_event_t *e) {
    if (e->type != MOUSE_DOWN && e->type != MOUSE_WHEEL)
        return;
    int row = e->y / 16, col = e->x / 8;
    if (e->type == MOUSE_DOWN && row == 0) {
        static const char *const titles[] = {"Left", "File", "Command", "Options", "Right"};
        int c = 2, open = 0;
        for (int i = 0; i < 5; ++i) {
            int w = strlen(titles[i]) + 4;
            if (col >= c && col < c + w)
                open = i;
            c += w;
        }
        pull_down(open);
        return;
    }
    if (e->type == MOUSE_DOWN && row == SCR_H - 1) {
        if (col / 8 < 10)
            handle(KEY_F1 + col / 8);
        return;
    }
    if (row < PANEL_TOP || row >= PANEL_TOP + PANEL_H)
        return;
    int which = panels[active].mode == MODE_LONG ? active : col >= 40;
    panel_t *p = &panels[which];
    if (p->mode == MODE_INFO)
        return;
    active = which;
    if (e->type == MOUSE_WHEEL) {
        for (int i = 0; i < 3; ++i)
            panel_key_move(p, e->wheel > 0 ? KEY_UP : KEY_DOWN);
        return;
    }
    int r = row - (PANEL_TOP + 2);
    if (r < 0 || r >= PANEL_ROWS)
        return;
    int x = col - (panels[active].mode == MODE_LONG ? 0 : which * 40);
    if (p->mode == MODE_BRIEF && x >= 20)
        r += PANEL_ROWS;
    int i = p->top + r;
    if (i >= p->count)
        return;
    p->cur = i;
    if (e->button == MOUSE_RIGHT)
        panel_mark(p, i, !p->ent[i].marked);
    else if (e->double_click)
        open_entry();
}

void mc_command(int argc, char **argv) {
    // One at a time: an mc started from mc's command line would free the
    // panels the first one is still using.
    static bool running;
    if (running) {
        printf("mc is already running (F10 goes back to it)\n");
        return;
    }
    panels = psram_malloc(2 * sizeof *panels);
    if (!panels) {
        printf("mc: not enough memory\n");
        return;
    }
    running = true;
    memset(panels, 0, 2 * sizeof *panels);
    char cwd[256] = "/";
    f_getcwd(cwd, sizeof cwd);
    snprintf(panels[0].path, sizeof panels[0].path, "%s", cwd);
    snprintf(panels[1].path, sizeof panels[1].path, "%s", cwd);
    load_setup();
    for (int i = 1; i < argc && i <= 2; ++i)
        snprintf(panels[i - 1].path, sizeof panels[i - 1].path, "%s", argv[i]);
    for (int i = 0; i < 2; ++i) {
        DIR d;
        if (f_opendir(&d, panels[i].path) == FR_OK)
            f_closedir(&d);
        else
            strcpy(panels[i].path, "/");  // gone (a share not mounted, say)
        panel_load(&panels[i], NULL);
    }
    if (panels[active].mode == MODE_INFO)
        active = !active;
    cmd[0] = 0;
    cmd_pos = 0;
    quit = false;
    searching = false;
    scr_begin();
    while (!quit) {
        mouse_pointer(MOUSE_POINTER_TEXT);  // again: a command run may have changed it
        mc_redraw();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE)
            mouse_click(&e);
        else
            handle(k);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    save_setup(false);
    f_chdir(panels[active].path);
    panel_free(&panels[0]);
    panel_free(&panels[1]);
    scr_end();
    psram_free(panels);
    panels = NULL;
    running = false;
}
