/**
 * @file mc_ui.c
 * @brief mc's dialogs and menus, drawn with CP437 frames over a saved screen.
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

#include "mc.h"
#include "pico/stdlib.h"

/// Lines of `text` (split at \n): their count and the longest.
/// @param text the text
/// @param longest set to the longest line
/// @return the line count
static int measure(const char *text, int *longest) {
    int lines = 1, len = 0;
    *longest = 0;
    for (const char *p = text;; ++p) {
        if (!*p || *p == '\n') {
            if (len > *longest)
                *longest = len;
            if (!*p)
                break;
            ++lines;
            len = 0;
        } else {
            ++len;
        }
    }
    return lines;
}

/// Draws a dialog's frame.
/// @param row its row
/// @param col its column
/// @param h its height
/// @param w its width
/// @param title its title
/// @param attr its colours
static void frame(int row, int col, int h, int w, const char *title, uint8_t attr) {
    scr_fill(row, col, h, w, ' ', attr);
    scr_box(row + 1, col + 2, h - 2, w - 4, true, attr);
    if (title && *title) {
        int n = strlen(title) + 2;
        scr_printf(row + 1, col + (w - n) / 2, n, attr == C_ERROR ? A(YELLOW, RED) : C_DLG_TITLE,
                   " %s ", title);
    }
}

/// Where the last buttons drawn are, for the mouse.
/// Their row (-1: none), count, columns and widths.
///@{
static int btn_row = -1, btn_n, btn_col[8], btn_w[8];
///@}

/// The button at a cell.
/// @param row the row
/// @param col the column
/// @return the button, or -1
static int button_at(int row, int col) {
    for (int i = 0; row == btn_row && i < btn_n; ++i)
        if (col >= btn_col[i] && col < btn_col[i] + btn_w[i])
            return i;
    return -1;
}

/// A mouse event's cell.
/// @param e the event
/// @param row set to the row
/// @param col set to the column
static void cell_of(const mouse_event_t *e, int *row, int *col) {
    *row = e->y / 16;
    *col = e->x / 8;
}

/// Buttons "Yes|No" laid out centred on a row; returns how many.
/// @param row the row
/// @param col the left column
/// @param w the width
/// @param buttons the labels
/// @param focus the one chosen
/// @param error error colours
/// @return the count
static int draw_buttons(int row, int col, int w, const char *buttons, int focus, bool error) {
    char names[8][16];
    int n = 0, total = 0;
    for (const char *p = buttons; *p && n < 8;) {
        int k = 0;
        while (*p && *p != '|' && k < 15)
            names[n][k++] = *p++;
        names[n][k] = 0;
        total += k + 5;
        ++n;
        if (*p == '|')
            ++p;
    }
    int c = col + (w - total + 1) / 2;
    btn_row = row;
    btn_n = n;
    for (int i = 0; i < n; ++i) {
        btn_col[i] = c;
        btn_w[i] = strlen(names[i]) + 4;
        uint8_t at = i == focus ? (error ? C_ERR_FOCUS : C_DLG_FOCUS) : (error ? C_ERROR : C_DIALOG);
        scr_printf(row, c, -1, at, "[ %s ]", names[i]);
        scr_put(row, c + 2, names[i][0], i == focus ? at : (error ? A(YELLOW, RED) : A(YELLOW, LGRAY)));
        c += strlen(names[i]) + 5;
    }
    return n;
}

int dlg_message(const char *title, const char *text, const char *buttons, bool error) {
    void *saved = scr_save();
    int longest, lines = measure(text, &longest);
    int bw = strlen(buttons) + 4 * 5;
    int w = longest + 8;
    if (w < bw)
        w = bw;
    if (w < (int)strlen(title) + 10)
        w = strlen(title) + 10;
    if (w > SCR_W - 2)
        w = SCR_W - 2;
    int h = lines + 6, row = (SCR_H - h) / 2 - 1, col = (SCR_W - w) / 2;
    uint8_t at = error ? C_ERROR : C_DIALOG;
    int focus = 0, result = -1;
    for (;;) {
        frame(row, col, h, w, title, at);
        const char *p = text;
        for (int i = 0; i < lines; ++i) {
            const char *nl = strchr(p, '\n');
            int len = nl ? nl - p : (int)strlen(p);
            char line[SCR_W];
            snprintf(line, sizeof line, "%.*s", len < SCR_W - 1 ? len : SCR_W - 1, p);
            scr_text(row + 2 + i, col + (w - len) / 2, -1, at, line);
            p = nl ? nl + 1 : p + len;
        }
        int n = draw_buttons(row + h - 3, col, w, buttons, focus, error);
        scr_cursor(-1, 0);
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE) {
            if (e.type != MOUSE_DOWN)
                continue;
            if (e.button == MOUSE_RIGHT)
                break;  // as Esc
            int r, c, b;
            cell_of(&e, &r, &c);
            if ((b = button_at(r, c)) >= 0) {
                result = b;
                break;
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        if (k == '\r') {
            result = focus;
            break;
        }
        if (k == KEY_RIGHT || k == '\t')
            focus = (focus + 1) % n;
        else if (k == KEY_LEFT)
            focus = (focus + n - 1) % n;
        else if (k >= 0x20 && k < 0x7f) {
            // The first letter of a button chooses it.
            const char *b = buttons;
            for (int i = 0; i < n; ++i) {
                if ((*b | 0x20) == (k | 0x20)) {
                    result = i;
                    goto done;
                }
                b = strchr(b, '|');
                if (!b)
                    break;
                ++b;
            }
        }
    }
done:
    scr_restore(saved);
    return result;
}

/// An editable line at (row, col), width w: the buffer, cursor and scroll.
/// @param row the row
/// @param col the column
/// @param w the width
/// @param buf the text
/// @param size room in buf
/// @param k the key
/// @param pos the cursor
/// @param off the scroll
/// @return the key was used
static bool edit_line(int row, int col, int w, char *buf, int size, int k, int *pos, int *off) {
    int len = strlen(buf);
    if (k == KEY_LEFT && *pos > 0)
        --*pos;
    else if (k == KEY_RIGHT && *pos < len)
        ++*pos;
    else if (k == KEY_HOME || k == CTRL('a'))
        *pos = 0;
    else if (k == KEY_END || k == CTRL('e'))
        *pos = len;
    else if (k == '\b' && *pos > 0) {
        memmove(buf + *pos - 1, buf + *pos, len - *pos + 1);
        --*pos;
    } else if (k == KEY_DEL && *pos < len)
        memmove(buf + *pos, buf + *pos + 1, len - *pos);
    else if (k == CTRL('u')) {
        buf[0] = 0;
        *pos = 0;
    } else if (k >= 0x20 && k < 0x100 && k != 0x7f && len < size - 1) {
        memmove(buf + *pos + 1, buf + *pos, len - *pos + 1);
        buf[(*pos)++] = k;
    } else
        return false;
    if (*pos < *off)
        *off = *pos;
    if (*pos > *off + w - 1)
        *off = *pos - w + 1;
    return true;
}

/// Draws an input field.
/// @param row the row
/// @param col the column
/// @param w the width
/// @param buf the text
/// @param off the scroll
/// @param at its colours
static void draw_field(int row, int col, int w, const char *buf, int off, uint8_t at) {
    int len = strlen(buf);
    for (int i = 0; i < w; ++i)
        scr_put(row, col + i, off + i < len ? (uint8_t)buf[off + i] : ' ', at);
}

bool dlg_input(const char *title, const char *prompt, char *buf, int size) {
    void *saved = scr_save();
    int w = 64, h = 8, row = (SCR_H - h) / 2 - 1, col = (SCR_W - w) / 2;
    int fw = w - 10, pos = strlen(buf), off = pos > fw - 1 ? pos - fw + 1 : 0;
    bool ok = false;
    for (;;) {
        frame(row, col, h, w, title, C_DIALOG);
        scr_text(row + 2, col + 4, w - 8, C_DIALOG, prompt);
        draw_field(row + 3, col + 4, fw, buf, off, C_INPUT);
        draw_buttons(row + h - 3, col, w, "OK|Cancel", -1, false);
        scr_cursor(row + 3, col + 4 + pos - off);
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE) {
            if (e.type != MOUSE_DOWN)
                continue;
            if (e.button == MOUSE_RIGHT)
                break;
            int r, c, b;
            cell_of(&e, &r, &c);
            if ((b = button_at(r, c)) >= 0) {
                ok = b == 0;
                break;
            }
            if (r == row + 3 && c >= col + 4 && c < col + 4 + fw) {
                int len = strlen(buf);
                pos = off + c - (col + 4);
                pos = pos > len ? len : pos;
            }
            continue;
        }
        if (k == '\r') {
            ok = true;
            break;
        }
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        edit_line(row + 3, col + 4, fw, buf, size, k, &pos, &off);
    }
    scr_cursor(-1, 0);
    scr_restore(saved);
    return ok;
}

int dlg_list(const char *title, char *const *items, int n, int start) {
    void *saved = scr_save();
    int longest = 20;
    for (int i = 0; i < n; ++i)
        if ((int)strlen(items[i]) > longest)
            longest = strlen(items[i]);
    int w = longest + 8;
    if (w > SCR_W - 2)
        w = SCR_W - 2;
    int rows = n < SCR_H - 8 ? n : SCR_H - 8;
    if (rows < 1)
        rows = 1;
    int h = rows + 4, row = (SCR_H - h) / 2, col = (SCR_W - w) / 2;
    int cur = start >= 0 && start < n ? start : 0, top = 0, result = -1;
    for (;;) {
        if (cur < top)
            top = cur;
        if (cur >= top + rows)
            top = cur - rows + 1;
        frame(row, col, h, w, title, C_DIALOG);
        for (int i = 0; i < rows; ++i) {
            int k = top + i;
            scr_text(row + 2 + i, col + 3, w - 6, k == cur ? C_DLG_FOCUS : C_DIALOG,
                     k < n ? items[k] : "");
        }
        // A scroll bar on the frame when they don't all fit.
        int bar = col + w - 3;
        if (n > rows) {
            for (int i = 0; i < rows; ++i)
                scr_put(row + 2 + i, bar, BLOCK_LIGHT, C_DIALOG);
            scr_put(row + 2 + (rows > 1 ? top * (rows - 1) / (n - rows) : 0), bar, BLOCK_FULL, C_DIALOG);
        }
        scr_cursor(-1, 0);
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE) {
            int r, c;
            cell_of(&e, &r, &c);
            int i = r - (row + 2);
            if (e.type == MOUSE_WHEEL) {
                top -= e.wheel * 3;
                top = top > n - rows ? n - rows : top;
                top = top < 0 ? 0 : top;
                cur = cur < top ? top : cur >= top + rows ? top + rows - 1 : cur;
                continue;
            }
            bool drag = e.type == MOUSE_MOVE && (e.buttons & MOUSE_LEFT);
            if (e.type != MOUSE_DOWN && !drag)
                continue;
            if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT)
                break;
            if (i < 0 || i >= rows || c < col || c >= col + w)
                continue;
            if (c == bar && n > rows) {
                top = rows > 1 ? i * (n - rows) / (rows - 1) : 0;
                cur = cur < top ? top : cur >= top + rows ? top + rows - 1 : cur;
                continue;
            }
            if (top + i < n)
                cur = top + i;
            if (e.type == MOUSE_DOWN && e.double_click && top + i < n) {
                result = cur;
                break;
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c') || !n)
            break;
        if (k == '\r') {
            result = cur;
            break;
        }
        if (k == KEY_UP && cur > 0)
            --cur;
        else if (k == KEY_DOWN && cur < n - 1)
            ++cur;
        else if (k == KEY_PGUP)
            cur = cur > rows ? cur - rows : 0;
        else if (k == KEY_PGDN)
            cur = cur + rows < n ? cur + rows : n - 1;
        else if (k == KEY_HOME)
            cur = 0;
        else if (k == KEY_END)
            cur = n - 1;
    }
    scr_restore(saved);
    return result;
}

// ---- Progress ---------------------------------------------------------------------------------------

/// The screen under the progress dialog.
static void *progress_saved;
/// Its title.
static const char *progress_title;
/// When it is next redrawn.
static absolute_time_t progress_next;

void dlg_progress_begin(const char *title) {
    progress_saved = scr_save();
    progress_title = title;
    progress_next = nil_time;
}

/// Draws a progress bar.
/// @param row the row
/// @param col the column
/// @param w the width
/// @param done the part done
/// @param total the whole
static void bar(int row, int col, int w, uint64_t done, uint64_t total) {
    int fill = total ? (int)(done * w / total) : 0;
    if (fill > w)
        fill = w;
    for (int i = 0; i < w; ++i)
        scr_put(row, col + i, i < fill ? BLOCK_FULL : BLOCK_LIGHT, A(CYAN, LGRAY));
}

void dlg_progress(const char *line1, const char *line2, uint64_t done1, uint64_t total1,
                  uint64_t done2, uint64_t total2) {
    if (!time_reached(progress_next))
        return;  // a few times a second is plenty
    progress_next = make_timeout_time_ms(150);
    int w = 66, h = 11, row = (SCR_H - h) / 2 - 1, col = (SCR_W - w) / 2;
    frame(row, col, h, w, progress_title, C_DIALOG);
    scr_text(row + 2, col + 4, w - 8, C_DIALOG, line1);
    bar(row + 3, col + 4, w - 15, done1, total1);
    scr_printf(row + 3, col + w - 10, 5, C_DIALOG, "%3d%%",
               total1 ? (int)(done1 * 100 / total1) : 0);
    scr_text(row + 5, col + 4, w - 8, C_DIALOG, line2);
    if (total2) {
        bar(row + 6, col + 4, w - 15, done2, total2);
        scr_printf(row + 6, col + w - 10, 5, C_DIALOG, "%3d%%", (int)(done2 * 100 / total2));
    }
    scr_text(row + 8, col + 4, w - 8, A(DGRAY, LGRAY), "Esc or Ctrl-C stops");
    scr_cursor(-1, 0);
    scr_flush();
}

void dlg_progress_end(void) {
    scr_restore(progress_saved);
    progress_saved = NULL;
}

// ---- Menus -------------------------------------------------------------------------------------------

/// A menu title's column.
/// @param menus the menus
/// @param i the menu
/// @return the column
static int menu_col(menu_t *menus, int i) {
    int c = 2;
    for (int k = 0; k < i; ++k)
        c += strlen(menus[k].title) + 4;
    return c;
}

/// Draws the menu bar.
/// @param menus the menus
/// @param count their count
/// @param open the open one (-1 none)
static void draw_bar(menu_t *menus, int count, int open) {
    scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
    for (int i = 0; i < count; ++i)
        scr_printf(0, menu_col(menus, i), -1, i == open ? C_MENU_SEL : C_MENU, "  %s  ",
                   menus[i].title);
}

/// The menu whose title is at column col, or -1.
/// @param menus the menus
/// @param count their count
/// @param col the column
/// @return the menu
static int menu_at(menu_t *menus, int count, int col) {
    for (int i = 0; i < count; ++i) {
        int c = menu_col(menus, i), w = strlen(menus[i].title) + 4;
        if (col >= c && col < c + w)
            return i;
    }
    return -1;
}

int menu_run(menu_t *menus, int count, int open) {
    void *saved = scr_save();
    int sel = 0, result = -1;
    // Opened by a click on the bar with the button still held: letting go
    // over an item runs it.
    int mx, my, mb;
    mouse_get(&mx, &my, &mb);
    bool armed = (mb & MOUSE_LEFT) != 0;
    for (;;) {
        draw_bar(menus, count, open);
        menu_item_t *items = menus[open].items;
        int n = 0, w = 0;
        for (; items[n].text; ++n) {
            int len = strlen(items[n].text) + (items[n].key ? strlen(items[n].key) + 2 : 0);
            if (len > w)
                w = len;
        }
        w += 6;
        int col = menu_col(menus, open);
        if (col + w > SCR_W)
            col = SCR_W - w;
        while (items[sel].text[0] == '-')
            sel = (sel + 1) % n;
        scr_fill(1, col, n + 2, w, ' ', C_MENU);
        scr_box(1, col, n + 2, w, true, C_MENU);
        for (int i = 0; i < n; ++i) {
            if (items[i].text[0] == '-') {
                scr_hline(2 + i, col, w, DS_L, S_H, DS_R, C_MENU);
                continue;
            }
            uint8_t at = i == sel ? C_MENU_SEL : C_MENU;
            scr_fill(2 + i, col + 1, 1, w - 2, ' ', at);
            if (items[i].check)
                scr_put(2 + i, col + 1, 0xfb, at);
            scr_text(2 + i, col + 2, -1, at, items[i].text);
            if (items[i].key)
                scr_text(2 + i, col + w - 2 - strlen(items[i].key), -1, at, items[i].key);
        }
        scr_cursor(-1, 0);
        scr_flush();
        mouse_event_t e;
        int k = key_get_mouse(&e);
        if (k == KEY_MOUSE) {
            // Hovering or dragging moves the bar across the menus and items;
            // a click (or letting go after a drag) runs an item; a click
            // elsewhere, or the right button, closes the menu.
            int r, c;
            cell_of(&e, &r, &c);
            if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT)
                break;
            if (e.type == MOUSE_WHEEL) {
                k = e.wheel > 0 ? KEY_UP : KEY_DOWN;
            } else {
                if (e.type == MOUSE_DOWN)
                    armed = true;
                if (r == 0) {
                    int m = menu_at(menus, count, c);
                    if (m >= 0 && m != open) {
                        open = m;
                        sel = 0;
                        scr_restore(saved);
                        saved = scr_save();
                    } else if (m < 0 && e.type == MOUSE_DOWN) {
                        break;
                    }
                    if (e.type == MOUSE_UP)
                        armed = false;
                    continue;
                }
                int i = r - 2;
                bool inside = i >= 0 && i < n && c >= col && c < col + w;
                if (inside && items[i].text[0] != '-') {
                    sel = i;
                    if (e.type == MOUSE_DOWN || (e.type == MOUSE_UP && armed)) {
                        result = items[i].id;
                        break;
                    }
                } else if (!inside && e.type == MOUSE_DOWN && (r < 1 || r > n + 2 || c < col || c >= col + w)) {
                    break;  // a click outside the menu
                }
                if (e.type == MOUSE_UP)
                    armed = false;
                continue;
            }
        }
        if (k == KEY_ESC || k == CTRL('c') || k == KEY_F10 || k == KEY_F9)
            break;
        if (k == '\r') {
            result = items[sel].id;
            break;
        }
        if (k == KEY_UP)
            do
                sel = (sel + n - 1) % n;
            while (items[sel].text[0] == '-');
        else if (k == KEY_DOWN)
            do
                sel = (sel + 1) % n;
            while (items[sel].text[0] == '-');
        else if (k == KEY_LEFT || k == KEY_RIGHT) {
            open = (open + (k == KEY_LEFT ? count - 1 : 1)) % count;
            sel = 0;
            scr_restore(saved);
            saved = scr_save();
        }
    }
    scr_restore(saved);
    return result;
}
