/**
 * @file trk_ui.c
 * @brief The tracker's menus and dialogs, drawn in its own style (bevelled panels with a shadow, a title strip, its buttons and colours) instead of mc's text boxes.
 *
 * The same calls as mc's dlg_* and menu_run (mc keeps those). Each one keeps
 * the pixels it covers and puts them back when it closes.
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

/// The shadows' offset.
#define SHADOW 6
/// Their colour.
#define C_SHADOW 0x00
#define C_STRIP  0x0b  ///< the title strip
#define C_SEL    0x2b  ///< the item or button with the keyboard

// ---- Keeping what is under a box ------------------------------------------------------------------

/// What was under a box.
typedef struct {
    /// The box.
    ///@{
    int x, y, w, h;
    ///@}
    uint8_t *pixels;  ///< the pixels (PSRAM)
} under_t;

/// (From and back into wherever drawing goes now: the screen, or the
/// off-screen copy below.)
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @return the copy
static under_t keep(int x, int y, int w, int h) {
    under_t u = {x, y, w, h, NULL};
    if (x < 0) u.w += x, u.x = 0;
    if (y < 0) u.h += y, u.y = 0;
    if (u.x + u.w > DVI_WIDTH) u.w = DVI_WIDTH - u.x;
    if (u.y + u.h > DVI_HEIGHT) u.h = DVI_HEIGHT - u.y;
    mouse_pointer_hide();
    u.pixels = psram_malloc((size_t)u.w * u.h);
    for (int r = 0; u.pixels && r < u.h; ++r)
        memcpy(u.pixels + r * u.w, gfx_target + (u.y + r) * DVI_WIDTH + u.x, u.w);
    return u;
}

/// Puts what was under a box back.
/// @param u the copy
static void put_back(under_t *u) {
    mouse_pointer_hide();
    for (int r = 0; u->pixels && r < u->h; ++r)
        memcpy(gfx_target + (u->y + r) * DVI_WIDTH + u->x, u->pixels + r * u->w, u->w);
    psram_free(u->pixels);
    u->pixels = NULL;
    mouse_pointer_show();
}

// ---- Drawing off the screen ---------------------------------------------------------------------------
// A menu or dialog is drawn into a copy of the screen and then copied onto
// it, so each pixel goes straight from old to new: a label is never seen
// cleared and drawn again (which flickered on every mouse move).

static uint8_t *offscr;  ///< a whole-screen copy (PSRAM) while a menu or dialog is up
/// Boxes using the off-screen copy.
static int off_users;

/// Clips an area to the screen.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
static void clamp_area(int *x, int *y, int *w, int *h) {
    if (*x < 0) *w += *x, *x = 0;
    if (*y < 0) *h += *y, *y = 0;
    if (*x + *w > DVI_WIDTH) *w = DVI_WIDTH - *x;
    if (*y + *h > DVI_HEIGHT) *h = DVI_HEIGHT - *y;
}

/// The off-screen copy of an area of the screen; drawing goes there.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
static void off_begin(int x, int y, int w, int h) {
    if (!off_users++)
        offscr = psram_malloc(DVI_WIDTH * DVI_HEIGHT);
    if (!offscr)
        return;  // no memory: draw on the screen itself
    clamp_area(&x, &y, &w, &h);
    for (int r = 0; r < h; ++r)
        memcpy(offscr + (y + r) * DVI_WIDTH + x, dvi_framebuf + (y + r) * DVI_WIDTH + x, w);
    gfx_target = offscr;
}

/// What was drawn in the area, onto the screen.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
static void off_show(int x, int y, int w, int h) {
    if (!offscr)
        return;
    clamp_area(&x, &y, &w, &h);
    for (int r = 0; r < h; ++r)
        memcpy(dvi_framebuf + (y + r) * DVI_WIDTH + x, offscr + (y + r) * DVI_WIDTH + x, w);
}

/// Ends drawing off the screen.
static void off_end(void) {
    gfx_target = dvi_framebuf;
    if (--off_users)
        return;
    psram_free(offscr);
    offscr = NULL;
}

/// A panel with a shadow and a title strip; the inside starts 22 down.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param title its title
/// @param error red
static void panel(int x, int y, int w, int h, const char *title, bool error) {
    gfx_fill(x + SHADOW, y + SHADOW, w, h, C_SHADOW);
    gfx_bevel(x, y, w, h, true, C_FACE);
    if (title) {
        gfx_fill(x + 2, y + 2, w - 4, 18, error ? C_RED : C_STRIP);
        int n = strlen(title);
        gfx_text(x + (w - n * 8) / 2, y + 3, title, n, C_YELLOW, error ? C_RED : C_STRIP);
    }
}

/// Places to click inside a dialog.
typedef struct {
    /// Its rectangle and what it is.
    ///@{
    int x, y, w, h, id;
    ///@}
} spot_t;

/// The place at a point.
/// @param s the places
/// @param n their count
/// @param x the x
/// @param y the y
/// @return its id, or -1
static int spot_at(const spot_t *s, int n, int x, int y) {
    for (int i = 0; i < n; ++i)
        if (x >= s[i].x && x < s[i].x + s[i].w && y >= s[i].y && y < s[i].y + s[i].h)
            return s[i].id;
    return -1;
}

/// Waits for a key or a mouse event (moves only with a button held, or if
/// `moves`), moving the pointer meanwhile.
/// @param e set to a mouse event
/// @param moves report moves
/// @return the key, or KEY_MOUSE
static int next_input(mouse_event_t *e, bool moves) {
    for (;;) {
        int k = key_poll();
        if (k != KEY_NONE)
            return k;
        while (mouse_event(e))
            if (e->type != MOUSE_MOVE || moves || (e->buttons & MOUSE_LEFT))
                return KEY_MOUSE;
        mouse_pointer_update();
        sleep_ms(5);
    }
}

// ---- Buttons --------------------------------------------------------------------------------------

/// Splits "Yes|No" into names.
/// @param buttons the text
/// @param names set to them
/// @return their count
static int split_buttons(const char *buttons, char names[8][16]) {
    int n = 0;
    for (const char *p = buttons; *p && n < 8;) {
        int k = 0;
        while (*p && *p != '|' && k < 15)
            names[n][k++] = *p++;
        names[n][k] = 0;
        ++n;
        if (*p == '|')
            ++p;
    }
    return n;
}

/// The row of buttons centred in a box; their places into spots (ids 0..n-1).
/// @param x left
/// @param y top
/// @param w the box's width
/// @param names the names
/// @param n their count
/// @param focus the button with the keyboard
/// @param spots set to their places
/// @return n
static int draw_buttons(int x, int y, int w, char names[8][16], int n, int focus, spot_t *spots) {
    int bw[8], total = 0;
    for (int i = 0; i < n; ++i) {
        bw[i] = (int)strlen(names[i]) * 8 + 24;
        bw[i] = bw[i] < 72 ? 72 : bw[i];
        total += bw[i] + 8;
    }
    int bx = x + (w - total + 8) / 2;
    for (int i = 0; i < n; ++i) {
        uint8_t face = i == focus ? C_SEL : C_FACE;
        int len = strlen(names[i]);
        gfx_bevel(bx, y, bw[i], 22, true, face);
        gfx_text(bx + (bw[i] - len * 8) / 2, y + 3, names[i], len, i == focus ? C_YELLOW : C_TEXT,
                 face);
        gfx_hline(bx + (bw[i] - len * 8) / 2, y + 17, 8, i == focus ? C_YELLOW : C_LABEL);
        spots[i] = (spot_t){bx, y, bw[i], 22, i};
        bx += bw[i] + 8;
    }
    return n;
}

// ---- A message with buttons -----------------------------------------------------------------------

int gui_message(const char *title, const char *text, const char *buttons, bool error) {
    char names[8][16];
    int nb = split_buttons(buttons, names);
    int lines = 1, longest = 0, len = 0;
    for (const char *p = text;; ++p) {
        if (!*p || *p == '\n') {
            longest = len > longest ? len : longest;
            if (!*p)
                break;
            ++lines;
            len = 0;
        } else {
            ++len;
        }
    }
    int w = longest * 8 + 48, bw = 0;
    for (int i = 0; i < nb; ++i)
        bw += ((int)strlen(names[i]) * 8 + 24 < 72 ? 72 : (int)strlen(names[i]) * 8 + 24) + 8;
    w = w < bw + 32 ? bw + 32 : w;
    w = w < (int)strlen(title) * 8 + 48 ? (int)strlen(title) * 8 + 48 : w;
    w = w > DVI_WIDTH - 32 ? DVI_WIDTH - 32 : w;
    int h = 22 + 12 + lines * 16 + 12 + 22 + 12;
    int x = (DVI_WIDTH - w) / 2, y = (DVI_HEIGHT - h) / 2 - 20;
    under_t u = keep(x, y, w + SHADOW, h + SHADOW);
    off_begin(x, y, w + SHADOW, h + SHADOW);
    int focus = 0, result = -1;
    spot_t spots[8];
    for (;;) {
        panel(x, y, w, h, title, error);
        const char *p = text;
        for (int i = 0; i < lines; ++i) {
            const char *nl = strchr(p, '\n');
            int n = nl ? nl - p : (int)strlen(p);
            char line[96];
            snprintf(line, sizeof line, "%.*s", n < 95 ? n : 95, p);
            gfx_text(x + (w - n * 8) / 2, y + 34 + i * 16, line, -1, C_TEXT, C_FACE);
            p = nl ? nl + 1 : p + n;
        }
        draw_buttons(x, y + h - 34, w, names, nb, focus, spots);
        off_show(x, y, w + SHADOW, h + SHADOW);
        mouse_event_t e;
        int k = next_input(&e, false);
        if (k == KEY_MOUSE) {
            if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT)
                break;
            if (e.type == MOUSE_DOWN) {
                int b = spot_at(spots, nb, e.x, e.y);
                if (b >= 0) {
                    result = b;
                    break;
                }
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
            focus = (focus + 1) % nb;
        else if (k == KEY_LEFT)
            focus = (focus + nb - 1) % nb;
        else if (k >= 0x20 && k < 0x7f) {
            for (int i = 0; i < nb; ++i)
                if ((names[i][0] | 0x20) == (k | 0x20)) {
                    result = i;
                    goto done;
                }
        }
    }
done:
    off_end();
    put_back(&u);
    return result;
}

// ---- One line of input ------------------------------------------------------------------------------

bool gui_input(const char *title, const char *prompt, char *buf, int size) {
    int w = 480, h = 22 + 12 + 16 + 8 + 20 + 12 + 22 + 12;
    int x = (DVI_WIDTH - w) / 2, y = (DVI_HEIGHT - h) / 2 - 20;
    int fx = x + 16, fy = y + 22 + 12 + 16 + 8, fw = w - 32, cols = (fw - 8) / 8;
    under_t u = keep(x, y, w + SHADOW, h + SHADOW);
    int pos = strlen(buf), off = 0;
    bool ok = false;
    char names[8][16] = {"OK", "Cancel"};
    spot_t spots[3];
    off_begin(x, y, w + SHADOW, h + SHADOW);
    for (;;) {
        if (pos < off)
            off = pos;
        if (pos > off + cols - 1)
            off = pos - cols + 1;
        panel(x, y, w, h, title, false);
        gfx_text(fx, y + 34, prompt, (w - 32) / 8, C_LABEL, C_FACE);
        gfx_bevel(fx, fy, fw, 20, false, C_FIELD);
        int len = strlen(buf);
        for (int i = 0; i < cols; ++i) {
            int k = off + i;
            bool at = k == pos;
            gfx_char(fx + 4 + i * 8, fy + 2, k < len ? (uint8_t)buf[k] : ' ', at ? C_BG : C_TEXT,
                     at ? C_CURSOR : C_FIELD);
        }
        draw_buttons(x, y + h - 34, w, names, 2, 0, spots);
        spots[2] = (spot_t){fx, fy, fw, 20, 2};
        off_show(x, y, w + SHADOW, h + SHADOW);
        mouse_event_t e;
        int k = next_input(&e, false);
        if (k == KEY_MOUSE) {
            if (e.type != MOUSE_DOWN)
                continue;
            if (e.button == MOUSE_RIGHT)
                break;
            int s = spot_at(spots, 3, e.x, e.y);
            if (s == 0 || s == 1) {
                ok = s == 0;
                break;
            }
            if (s == 2) {
                int c = off + (e.x - fx - 4) / 8;
                pos = c < 0 ? 0 : c > len ? len : c;
            }
            continue;
        }
        if (k == '\r') {
            ok = true;
            break;
        }
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        if (k == KEY_LEFT && pos > 0)
            --pos;
        else if (k == KEY_RIGHT && pos < len)
            ++pos;
        else if (k == KEY_HOME || k == CTRL('a'))
            pos = 0;
        else if (k == KEY_END || k == CTRL('e'))
            pos = len;
        else if (k == '\b' && pos > 0) {
            memmove(buf + pos - 1, buf + pos, len - pos + 1);
            --pos;
        } else if (k == KEY_DEL && pos < len)
            memmove(buf + pos, buf + pos + 1, len - pos);
        else if (k == CTRL('u')) {
            buf[0] = 0;
            pos = 0;
        } else if (k >= 0x20 && k < 0x7f && len < size - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char)k;
        }
    }
    off_end();
    put_back(&u);
    return ok;
}

// ---- A list to choose from ---------------------------------------------------------------------------

int gui_list(const char *title, char *const *items, int n, int start) {
    int longest = (int)strlen(title) + 4;
    for (int i = 0; i < n; ++i)
        longest = (int)strlen(items[i]) > longest ? (int)strlen(items[i]) : longest;
    int rows = n < 16 ? n : 16;
    rows = rows < 1 ? 1 : rows;
    int w = longest * 8 + 48 < 224 ? 224 : longest * 8 + 48, h = 22 + 8 + rows * 16 + 8 + 4;
    w = w > DVI_WIDTH - 32 ? DVI_WIDTH - 32 : w;
    int x = (DVI_WIDTH - w) / 2, y = (DVI_HEIGHT - h) / 2 - 20, lx = x + 12, ly = y + 30;
    under_t u = keep(x, y, w + SHADOW, h + SHADOW);
    int cur = start >= 0 && start < n ? start : 0, top = 0, result = -1;
    off_begin(x, y, w + SHADOW, h + SHADOW);
    for (;;) {
        if (cur < top)
            top = cur;
        if (cur >= top + rows)
            top = cur - rows + 1;
        panel(x, y, w, h, title, false);
        gfx_bevel(lx - 2, ly - 2, w - 20, rows * 16 + 4, false, C_FIELD);
        for (int i = 0; i < rows; ++i) {
            int k = top + i;
            bool on = k == cur;
            gfx_text(lx, ly + i * 16, k < n ? items[k] : "", (w - 24) / 8, on ? C_TEXT : C_LABEL,
                     on ? C_SEL : C_FIELD);
        }
        off_show(x, y, w + SHADOW, h + SHADOW);
        mouse_event_t e;
        int k = next_input(&e, false);
        if (k == KEY_MOUSE) {
            int i = (e.y - ly) / 16;
            bool inside = e.x >= lx && e.x < x + w - 8 && e.y >= ly && i < rows;
            if (e.type == MOUSE_WHEEL) {
                cur -= e.wheel;
                cur = cur < 0 ? 0 : cur >= n ? n - 1 : cur;
            } else if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT) {
                break;
            } else if (e.type == MOUSE_DOWN && inside && top + i < n) {
                cur = top + i;
                if (e.double_click) {
                    result = cur;
                    break;
                }
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
    off_end();
    put_back(&u);
    return result;
}

// ---- The menus --------------------------------------------------------------------------------------
// Items whose text starts with '~' are shown greyed and can't be chosen.

/// A menu title's x.
/// @param menus the menus
/// @param i the menu
/// @return its x
static int bar_x(menu_t *menus, int i) {
    int x = 16;
    for (int k = 0; k < i; ++k)
        x += ((int)strlen(menus[k].title) + 4) * 8;
    return x;
}

/// Whether an item can be chosen (not a line, not greyed).
/// @param it the item
/// @return it can
static bool usable(const menu_item_t *it) {
    return it->text[0] != '-' && it->text[0] != '~';
}

/// A drop-down's items, size and where each is.
typedef struct {
    menu_item_t *items;  ///< the items
    /// rh: an item's height (less if it wouldn't fit)
    ///@{
    int n, x, y, w, h, rh;
    ///@}
    int item_y[40];  ///< each item's y
    int8_t acc[40];  ///< each item's key letter (its place in the text), -1 none
} drop_t;

/// Measures a drop-down.
/// @param d set
/// @param items the items
/// @param x left
/// @param y top
static void drop_measure(drop_t *d, menu_item_t *items, int x, int y) {
    int tw = 0, kw = 0;
    d->items = items;
    for (d->n = 0; items[d->n].text && d->n < 40; ++d->n) {
        const char *t = items[d->n].text + (items[d->n].text[0] == '~');
        tw = (int)strlen(t) > tw ? (int)strlen(t) : tw;
        if (items[d->n].key && (int)strlen(items[d->n].key) > kw)
            kw = strlen(items[d->n].key);
    }
    d->w = (tw + kw + (kw ? 6 : 4)) * 8;
    int seps = 0;
    for (int i = 0; i < d->n; ++i)
        seps += items[i].text[0] == '-';
    d->rh = 6 + seps * 8 + (d->n - seps) * 18 <= DVI_HEIGHT - 16 - SHADOW ? 18 : 16;
    d->h = 6;
    for (int i = 0; i < d->n; ++i) {
        d->item_y[i] = y + 3 + d->h - 6;
        d->h += items[i].text[0] == '-' ? 8 : d->rh;
    }
    // Key letters: the first letter of each item not taken by one above it.
    bool used[26] = {false};
    for (int i = 0; i < d->n; ++i) {
        d->acc[i] = -1;
        const char *t = items[i].text;
        if (!usable(&items[i]))
            continue;
        for (int c = 0; t[c] && d->acc[i] < 0; ++c) {
            int l = (t[c] | 0x20) - 'a';
            if (l >= 0 && l < 26 && !used[l] && (c == 0 || t[c - 1] == ' '))
                used[l] = true, d->acc[i] = c;
        }
        for (int c = 0; t[c] && d->acc[i] < 0; ++c) {
            int l = (t[c] | 0x20) - 'a';
            if (l >= 0 && l < 26 && !used[l])
                used[l] = true, d->acc[i] = c;
        }
    }
    if (x + d->w + SHADOW > DVI_WIDTH)
        x = DVI_WIDTH - d->w - SHADOW;
    if (y + d->h + SHADOW > DVI_HEIGHT) {
        int up = DVI_HEIGHT - d->h - SHADOW - y;
        y += up;
        for (int i = 0; i < d->n; ++i)
            d->item_y[i] += up;
    }
    d->x = x;
    d->y = y;
}

/// Draws a drop-down.
/// @param d it
/// @param sel the item chosen
static void drop_draw(const drop_t *d, int sel) {
    int x = d->x, w = d->w;
    gfx_fill(x + SHADOW, d->y + SHADOW, w, d->h, C_SHADOW);
    gfx_bevel(x, d->y, w, d->h, true, C_FACE);
    for (int i = 0; i < d->n; ++i) {
        const menu_item_t *it = &d->items[i];
        int iy = d->item_y[i];
        if (it->text[0] == '-') {
            gfx_hline(x + 6, iy + 3, w - 12, C_DARK);
            gfx_hline(x + 6, iy + 4, w - 12, C_LIGHT);
            continue;
        }
        bool on = i == sel, off = it->text[0] == '~';
        uint8_t bg = on ? C_SEL : C_FACE, fg = off ? C_DIM : on ? C_TEXT : C_LABEL;
        int ty = iy + (d->rh - 16) / 2;
        gfx_fill(x + 3, iy, w - 6, d->rh, bg);
        if (it->check)
            gfx_char(x + 6, ty, 0xfb, on ? C_YELLOW : C_GREEN, bg);
        gfx_text(x + 22, ty, it->text + off, -1, fg, bg);
        if (d->acc[i] >= 0)
            gfx_hline(x + 22 + d->acc[i] * 8, ty + 14, 8, on ? C_YELLOW : C_DIM);  // its key letter
        if (it->key && it->key[0])
            gfx_text(x + w - 8 - (int)strlen(it->key) * 8, ty, it->key, -1,
                     off ? C_DIM : on ? C_YELLOW : C_DIM, bg);
    }
}

/// The item under the mouse (-1: none; -2: outside the drop-down).
/// @param d the drop-down
/// @param x the x
/// @param y the y
/// @return the item
static int drop_at(const drop_t *d, int x, int y) {
    if (x < d->x || x >= d->x + d->w || y < d->y || y >= d->y + d->h)
        return -2;
    for (int i = 0; i < d->n; ++i)
        if (y >= d->item_y[i] && y < d->item_y[i] + (d->items[i].text[0] == '-' ? 8 : d->rh))
            return i;
    return -1;
}

/// The item chosen, or the first that can be.
/// @param d the drop-down
/// @param sel the item
/// @return it, or -1
static int first_usable(const drop_t *d, int sel) {
    if (sel >= 0 && sel < d->n && usable(&d->items[sel]))
        return sel;
    for (int i = 0; i < d->n; ++i)
        if (usable(&d->items[i]))
            return i;
    return -1;
}

/// The next item that can be chosen.
/// @param d the drop-down
/// @param sel the item
/// @param dir +1 or -1
/// @return it
static int step_sel(const drop_t *d, int sel, int dir) {
    for (int tries = 0; tries < d->n; ++tries) {
        sel = (sel + dir + d->n) % d->n;
        if (usable(&d->items[sel]))
            break;
    }
    return sel;
}

/// The item whose key letter is k.
/// @param d the drop-down
/// @param k the key
/// @return the item, or -1
static int letter_item(const drop_t *d, int k) {
    for (int i = 0; i < d->n; ++i)
        if (d->acc[i] >= 0 && (d->items[i].text[d->acc[i]] | 0x20) == (k | 0x20))
            return i;
    return -1;
}

/// Draws the menu bar.
/// @param menus the menus
/// @param count their count
/// @param open the menu open (-1 none)
static void draw_bar(menu_t *menus, int count, int open) {
    for (int i = 0; i < count; ++i) {
        int bx = bar_x(menus, i), bw = ((int)strlen(menus[i].title) + 4) * 8;
        uint8_t bg = i == open ? C_SEL : C_FACE;
        gfx_fill(bx, 1, bw, 14, bg);
        gfx_text(bx + 16, 0, menus[i].title, -1, i == open ? C_YELLOW : C_TEXT, i == open ? bg : C_FACE);
        gfx_hline(bx, 0, bw, C_LIGHT);
        gfx_hline(bx, 15, bw, C_DARK);
        gfx_hline(bx + 16, 14, 8, i == open ? C_YELLOW : C_LABEL);  // its key letter
    }
}

int gui_menu(menu_t *menus, int count, int open) {
    int sel = -1, result = -1, mx, my, mb;
    mouse_get(&mx, &my, &mb);
    bool armed = (mb & MOUSE_LEFT) != 0;  // opened by a press: letting go on an item runs it
    under_t u = {0};
    drop_t d;
    bool fresh = true;
    // Drawn off the screen, and drawn again only when something changes.
    off_begin(0, 0, DVI_WIDTH, DVI_HEIGHT);
    int shown_h = 0, drawn_sel = -2, drawn_open = -1;
    for (;;) {
        if (fresh) {
            drop_measure(&d, menus[open].items, bar_x(menus, open), 16);
            u = keep(0, 0, DVI_WIDTH, d.y + d.h + SHADOW);
            sel = first_usable(&d, sel);
            fresh = false;
            drawn_sel = -2;
        }
        if (sel != drawn_sel || open != drawn_open) {
            draw_bar(menus, count, open);
            drop_draw(&d, sel);
            int hh = d.y + d.h + SHADOW;
            off_show(0, 0, DVI_WIDTH, hh > shown_h ? hh : shown_h);  // the old menu's place too
            shown_h = hh;
            drawn_sel = sel;
            drawn_open = open;
        }
        mouse_event_t e;
        int k = next_input(&e, true);
        if (k == KEY_MOUSE) {
            if (e.type == MOUSE_DOWN && e.button == MOUSE_RIGHT)
                break;
            if (e.type == MOUSE_WHEEL) {
                sel = step_sel(&d, sel, e.wheel > 0 ? -1 : 1);
                continue;
            }
            if (e.type == MOUSE_DOWN)
                armed = true;
            if (e.y < 16) {
                // Over the bar: moving (or clicking) along it opens the others.
                for (int i = 0; i < count; ++i) {
                    int bx = bar_x(menus, i), bw = ((int)strlen(menus[i].title) + 4) * 8;
                    if (e.x >= bx && e.x < bx + bw && i != open) {
                        put_back(&u);
                        open = i;
                        sel = -1;
                        fresh = true;
                    }
                }
                if (e.type == MOUSE_UP)
                    armed = false;
                continue;
            }
            int over = drop_at(&d, e.x, e.y);
            if (over >= 0 && usable(&d.items[over])) {
                sel = over;
                if (e.type == MOUSE_DOWN || (e.type == MOUSE_UP && armed)) {
                    result = d.items[over].id;
                    break;
                }
            } else if (over == -2 && e.type == MOUSE_DOWN) {
                break;
            }
            if (e.type == MOUSE_UP)
                armed = false;
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c') || k == KEY_F10 || k == KEY_F9)
            break;
        if (k == '\r') {
            if (sel >= 0 && usable(&d.items[sel])) {
                result = d.items[sel].id;
                break;
            }
        } else if (k == KEY_UP || k == KEY_DOWN) {
            sel = step_sel(&d, sel, k == KEY_UP ? -1 : 1);
        } else if (k == KEY_LEFT || k == KEY_RIGHT) {
            put_back(&u);
            open = (open + (k == KEY_LEFT ? count - 1 : 1)) % count;
            sel = -1;
            fresh = true;
        } else if (k >= KEY_ALT + 'a' && k <= KEY_ALT + 'z') {
            for (int i = 0; i < count; ++i)
                if ((menus[i].title[0] | 0x20) == k - KEY_ALT && i != open) {
                    put_back(&u);
                    open = i;
                    sel = -1;
                    fresh = true;
                    break;
                }
        } else if (k >= 0x20 && k < 0x7f) {
            int i = letter_item(&d, k);
            if (i >= 0) {
                result = d.items[i].id;
                break;
            }
        }
    }
    put_back(&u);
    off_show(0, 0, DVI_WIDTH, shown_h);
    off_end();
    return result;
}

int gui_popup(menu_item_t *items, int x, int y) {
    drop_t d;
    drop_measure(&d, items, x, y);
    under_t u = keep(d.x, d.y, d.w + SHADOW, d.h + SHADOW);
    int sel = -1, result = -1, drawn = -2;
    bool moved = false;  // a release picks only after the pointer moved onto an item
    off_begin(d.x, d.y, d.w + SHADOW, d.h + SHADOW);
    for (;;) {
        if (sel != drawn) {
            drop_draw(&d, sel);
            off_show(d.x, d.y, d.w + SHADOW, d.h + SHADOW);
            drawn = sel;
        }
        mouse_event_t e;
        int k = next_input(&e, true);
        if (k == KEY_MOUSE) {
            if (e.type == MOUSE_WHEEL) {
                sel = step_sel(&d, sel < 0 ? 0 : sel, e.wheel > 0 ? -1 : 1);
                continue;
            }
            int over = drop_at(&d, e.x, e.y);
            if (e.type == MOUSE_MOVE) {
                sel = over >= 0 && usable(&d.items[over]) ? over : -1;
                moved = true;
                continue;
            }
            if (over == -2 && e.type == MOUSE_DOWN)
                break;
            // A click on an item, or letting go of the button that opened it
            // on one.
            if (over >= 0 && usable(&d.items[over]) && (e.type == MOUSE_DOWN || moved)) {
                result = d.items[over].id;
                break;
            }
            continue;
        }
        if (k == KEY_ESC || k == CTRL('c'))
            break;
        if (k == '\r') {
            if (sel >= 0) {
                result = d.items[sel].id;
                break;
            }
        } else if (k == KEY_UP || k == KEY_DOWN) {
            sel = step_sel(&d, sel < 0 ? (k == KEY_UP ? 0 : d.n - 1) : sel, k == KEY_UP ? -1 : 1);
        } else if (k >= 0x20 && k < 0x7f) {
            int i = letter_item(&d, k);
            if (i >= 0) {
                result = d.items[i].id;
                break;
            }
        }
    }
    off_end();
    put_back(&u);
    return result;
}
