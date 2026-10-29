/**
 * @file uitest.c
 * @brief The hex editor on a PC: random edits (typing, insert and overwrite, delete, cut, copy, paste, bit flips) checked byte for byte against a plain copy, undo and redo all the way back and forth, saving, Find, Goto, the menus and the mouse, and screenshots.
 *
 *     ./uitest.sh  (builds this, writes hex_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/hex/hex_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock
static uint8_t mods;                          ///< the modifier keys held
static long heap = 1L << 30;                  ///< SRAM free, as the editor sees it

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return KEY_NONE; }
bool key_stop(void) { return false; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }
uint8_t usb_keyboard_modifiers(void) { return mods; }
long hex_heap_free(void) { return heap; }

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param path the path
/// @return the local one
static const char *local(const char *path) {
    static char buf[2][300];
    static int k;
    k ^= 1;
    snprintf(buf[k], sizeof buf[k], ".%s%s", path[0] == '/' ? "" : "/", path);
    return buf[k];
}
FRESULT f_open(FIL *f, const char *path, int mode) {
    f->f = fopen(local(path), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
FRESULT f_lseek(FIL *f, FSIZE_t at) { return fseek(f->f, (long)at, SEEK_SET) ? 1 : FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }
FSIZE_t f_size(FIL *f) {
    long at = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long n = ftell(f->f);
    fseek(f->f, at, SEEK_SET);
    return (FSIZE_t)n;
}
void *pc_opendir(const char *path);                          ///< dirstub.c
void pc_readdir(void *d, char *name, int n, int *is_dir);    ///< dirstub.c
void pc_closedir(void *d);                                   ///< dirstub.c
FRESULT f_opendir(DIR *d, const char *path) { d->d = pc_opendir(local(path)); return d->d ? FR_OK : 1; }
FRESULT f_readdir(DIR *d, FILINFO *fi) {
    int is_dir = 0;
    pc_readdir(d->d, fi->fname, sizeof fi->fname, &is_dir);
    fi->fattrib = is_dir ? AM_DIR : 0;
    fi->fsize = 0;
    struct stat st;
    char p[300];
    snprintf(p, sizeof p, "%s", fi->fname);
    if (fi->fname[0] && !stat(p, &st))
        fi->fsize = (unsigned long long)st.st_size;
    return FR_OK;
}
FRESULT f_closedir(DIR *d) { pc_closedir(d->d); return FR_OK; }
FRESULT f_stat(const char *path, FILINFO *fi) {
    struct stat st;
    if (stat(local(path), &st))
        return 1;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fi->fsize = (unsigned long long)st.st_size;
    return FR_OK;
}
FRESULT f_getcwd(char *buf, UINT n) { snprintf(buf, n, "/"); return FR_OK; }
FRESULT f_unlink(const char *path) { return remove(local(path)) ? 1 : FR_OK; }
FRESULT f_rename(const char *from, const char *to) {
    const char *a = local(from), *b = local(to);
    struct stat st;
    if (!stat(b, &st))
        return 1;  // FatFs won't rename over a file
    return rename(a, b) ? 1 : FR_OK;
}

static int failures;  ///< checks that failed

/// Reports a failed check.
#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            ++failures;                                 \
            printf("FAIL uitest.c:%d: ", __LINE__);     \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/// What the editor's loop does after events: draws again if it needs to.
static void settle(void) {
    if (F->dirty) {
        F->dirty = false;
        draw();
    } else if (F->view_dirty && !F->dlg && F->menu < 0) {
        F->view_dirty = false;
        F->t = F->g;
        draw_view();
        F->t = dvi_framebuf;
    }
}

/// A mouse event.
/// @param type MOUSE_*
/// @param btn the button
/// @param x where
/// @param y where
static void ev(int type, int btn, int x, int y) {
    mouse_event_t e = {(uint8_t)type, (uint8_t)btn, (uint8_t)(type == MOUSE_UP ? 0 : btn), 0, (int16_t)x,
                       (int16_t)y, false};
    mouse(&e);
    settle();
}

/// A click.
/// @param x where
/// @param y where
/// @param btn the button
static void click(int x, int y, int btn) {
    ev(MOUSE_DOWN, btn, x, y);
    ev(MOUSE_UP, btn, x, y);
}

/// A key.
/// @param k the key
static void press(int k) {
    key(k);
    settle();
}

/// Types text.
/// @param s it
static void type(const char *s) {
    while (*s)
        press((uint8_t)*s++);
}

/// The whole document.
/// @param n set to its length
/// @return it (malloc)
static uint8_t *doc_all(uint32_t *n) {
    *n = F->size;
    uint8_t *b = malloc(F->size + 1);
    doc_read(0, b, NULL, F->size);
    return b;
}

/// A file's bytes.
/// @param name it
/// @param n set to its length
/// @return them (malloc), or NULL
static uint8_t *file_all(const char *name, uint32_t *n) {
    FILE *f = fopen(name, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1);
    *n = (uint32_t)fread(b, 1, *n, f);
    fclose(f);
    return b;
}

/// Writes a file.
/// @param name it
/// @param b its bytes
/// @param n how many
static void file_put(const char *name, const uint8_t *b, uint32_t n) {
    FILE *f = fopen(name, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
}

/// The plain copy the random edits are checked against.
static uint8_t *m;
static uint32_t mn, mcap;  ///< its length and room
static uint8_t *mclip;     ///< its clipboard
static uint32_t mclipn;    ///< that's length

/// The copy: replaces bytes.
/// @param pos where
/// @param len how many go
/// @param b what comes in
/// @param n how many
static void m_replace(uint32_t pos, uint32_t len, const uint8_t *b, uint32_t n) {
    if (mn - len + n > mcap)
        m = realloc(m, mcap = (mn - len + n) * 2 + 16);
    memmove(m + pos + n, m + pos + len, mn - pos - len);
    if (n)
        memcpy(m + pos, b, n);
    mn = mn - len + n;
}

/// Whether the document is the copy.
/// @return it is
static bool same(void) {
    uint32_t n;
    uint8_t *b = doc_all(&n);
    bool ok = n == mn && !memcmp(b, m, n);
    free(b);
    return ok;
}

/// Saves the picture as raw RGB332, with an arrow where the mouse is.
/// @param name the file
/// @param ax the arrow's x (-1: none)
/// @param ay its y
static void snap(const char *name, int ax, int ay) {
    static const char *const a[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
        "X....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
    };
    settle();
    draw();
    if (ax >= 0)
        for (int r = 0; r < 16; ++r)
            for (int c = 0; a[r][c]; ++c)
                if (a[r][c] != ' ')
                    F->g[(ay + r) * W + ax + c] = a[r][c] == 'X' ? C_BG : C_TEXT;
    FILE *f = fopen(name, "wb");
    fwrite(F->g, 1, W * H, f);
    fclose(f);
}

/// The screen place of a byte shown (its first hex digit, or its character).
/// @param pos the byte
/// @param text the text column
/// @param x set to the x
/// @param y set to the y
static void byte_xy(uint32_t pos, bool text, int *x, int *y) {
    *x = (text ? ASC_X + (int)(pos % 16) * 8 : hex_x(pos % 16)) + 3;
    *y = ROW_Y + (int)(pos / 16 - F->top) * 16 + 8;
}

/// Opens a menu with the mouse and clicks an item.
/// @param menu the menu
/// @param item its item
static void menu_click(int menu, int item) {
    click(bar_x(menu) + 20, 8, MOUSE_LEFT);
    int x, y, w, h, iy[MAX_ITEMS];
    int8_t acc[MAX_ITEMS];
    menu_layout(&x, &y, &w, &h, iy, acc);
    click(x + 30, iy[item] + 8, MOUSE_LEFT);
}

static int last_op;            ///< the last random edit
static uint32_t last_state[8];  ///< the cursor's state before it

/// One random edit, done on the document and on the copy.
/// @param r the random number generator's state
static void random_edit(unsigned *r) {
    int op = rand_r(r) % 14;
    last_op = op;
    last_state[0] = F->cur, last_state[1] = F->ins, last_state[2] = F->sel, last_state[3] = F->anchor,
    last_state[4] = F->nib, last_state[5] = F->lo, last_state[6] = F->typing, last_state[7] = F->text;
    uint32_t lo, hi;
    bool sel = sel_range(&lo, &hi);
    switch (op) {
    case 0: case 1: {  // the cursor somewhere, or a selection
        uint32_t a = mn ? (uint32_t)rand_r(r) % (mn + 1) : 0;
        move_to(a, false);
        if (rand_r(r) & 1) {
            uint32_t b = mn ? (uint32_t)rand_r(r) % (mn + 1) : 0;
            if (rand_r(r) & 1)
                b = a + rand_r(r) % 40 > mn ? mn : a + rand_r(r) % 40;
            mods = SHIFT_BITS;
            move_to(b, true);
            mods = 0;
        }
        break;
    }
    case 2: press(KEY_INS); break;
    case 3: case 4: {  // two hex digits
        if (F->text)
            press('\t');
        int d1 = rand_r(r) % 16, d2 = rand_r(r) % 16;
        uint8_t v = (uint8_t)(d1 << 4 | d2);
        uint32_t c = F->cur;
        if (sel)
            m_replace(lo, hi - lo, NULL, 0), c = lo;
        if (!F->ins && c < mn)
            m[c] = v;
        else
            m_replace(c, 0, &v, 1);
        press("0123456789abcdef"[d1]);
        press("0123456789ABCDEF"[d2]);
        break;
    }
    case 5: {  // text
        if (!F->text)
            press('\t');
        uint8_t v = (uint8_t)(' ' + rand_r(r) % 95);
        uint32_t c = F->cur;
        if (sel)
            m_replace(lo, hi - lo, NULL, 0), c = lo;
        if (!F->ins && c < mn)
            m[c] = v;
        else
            m_replace(c, 0, &v, 1);
        press(v);
        break;
    }
    case 6:  // Del
        if (sel)
            m_replace(lo, hi - lo, NULL, 0);
        else if (F->cur < mn)
            m_replace(F->cur, 1, NULL, 0);
        press(KEY_DEL);
        break;
    case 7:  // Backspace
        if (sel)
            m_replace(lo, hi - lo, NULL, 0);
        else if (F->cur && F->ins)
            m_replace(F->cur - 1, 1, NULL, 0);
        press(8);
        break;
    case 8: case 9:  // copy or cut
        if (sel) {
            mclip = realloc(mclip, hi - lo);
            memcpy(mclip, m + lo, hi - lo);
            mclipn = hi - lo;
            if (op == 9)
                m_replace(lo, hi - lo, NULL, 0);
        }
        press(op == 8 ? CTRL('c') : CTRL('x'));
        break;
    case 10: case 11:  // paste
        if (mclipn) {
            uint32_t at = F->cur, len = 0;
            if (sel)
                at = lo, len = hi - lo;
            else if (!F->ins)
                len = mn - at < mclipn ? mn - at : mclipn;
            m_replace(at, len, mclip, mclipn);
        }
        press(CTRL('v'));
        break;
    case 12:  // a bit
        if (F->cur < mn) {
            int b = rand_r(r) % 8;
            m[F->cur] ^= 0x80 >> b;
            flip_bit(b);
        }
        break;
    case 13:  // Select all, sometimes
        if (rand_r(r) % 4 == 0)
            press(CTRL('a'));
        break;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);  // seen even if the leak check ends it
    remove("TEST.BIN");
    remove("COPY.BIN");
    remove(TMP_NAME);
    uint8_t orig[5000];
    for (int i = 0; i < 5000; ++i)
        orig[i] = (uint8_t)(i * 7 + 3);
    memcpy(orig + 1000, "Hello, World", 12);
    file_put("TEST.BIN", orig, sizeof orig);
    start();
    F->g = malloc(W * H);
    CHECK(load("TEST.BIN"), "load");
    CHECK(F->size == 5000 && !strcmp(F->path, "/TEST.BIN"), "size %lu path %s", (unsigned long)F->size, F->path);
    draw();

    // Random edits, checked after each one; then undone all the way and redone.
    unsigned r = 12345;
    m = malloc(mcap = 8192);
    for (int round = 0; round < 300; ++round) {
        uint32_t n0 = 0;
        if (F->size < 2000) {  // deletes win in the long run: start again from the file
            new_doc();  // the clipboard leaves the file before it is written
            if (mclipn > CLIP_KEEP)
                mclipn = 0;
            file_put("TEST.BIN", orig, sizeof orig);
            load("TEST.BIN");
        }
        undo_forget();  // so undoing all goes back to here
        uint8_t *start_doc = doc_all(&n0);
        memcpy(m, start_doc, n0);
        mn = n0;
        for (int i = 0; i < 40; ++i) {
            random_edit(&r);
            {
                uint8_t *cb = calloc(1, F->cliplen + 1);
                for (int j = 0, o = 0; j < F->nclip; o += F->clip[j++].len)
                    piece_read(&F->clip[j], 0, cb + o, F->clip[j].len);
                if (F->cliplen != mclipn || (mclipn && memcmp(cb, mclip, mclipn))) {
                    printf("round %d edit %d op %d: the clipboards differ (%lu, %lu) state cur %u ins %u sel %u nib %u lo %u text %u\n", round, i, last_op,
                           (unsigned long)F->cliplen, (unsigned long)mclipn, last_state[0], last_state[1], last_state[2], last_state[4], last_state[5], last_state[7]);
                    free(cb);
                    goto out;
                }
                free(cb);
            }
            if (!same()) {
                CHECK(false, "round %d edit %d: the document and the copy differ (size %lu, %lu)", round, i,
                      (unsigned long)F->size, (unsigned long)mn);
                uint32_t dn;
                uint8_t *dd = doc_all(&dn);
                for (uint32_t j = 0; j < dn && j < mn; ++j)
                    if (dd[j] != m[j]) {
                        printf("  first difference at %lu: %02x, the copy %02x\n", (unsigned long)j, dd[j], m[j]);
                        break;
                    }
                printf("  op %d; before: cur %u ins %u sel %u anchor %u nib %u lo %u typing %u text %u\n", last_op,
                       last_state[0], last_state[1], last_state[2], last_state[3], last_state[4], last_state[5],
                       last_state[6], last_state[7]);
                free(dd);
                goto out;
            }
        }
        uint32_t nf;
        uint8_t *fin = doc_all(&nf);
        bool full = F->nundo < MAX_UNDO;  // dropped steps can't come back
        while (F->nundo)
            press(CTRL('z'));
        uint32_t nb;
        uint8_t *back = doc_all(&nb);
        if (full)
            CHECK(nb == n0 && !memcmp(back, start_doc, n0), "round %d: undo all didn't go back", round);
        while (F->nredo)
            press(CTRL('y'));
        free(back);
        back = doc_all(&nb);
        CHECK(nb == nf && !memcmp(back, fin, nf), "round %d: redo all didn't come back", round);
        free(back);
        free(fin);
        free(start_doc);
        // Every so often a save, and the file is the document.
        if (round % 10 == 9) {
            mn = 0;
            uint8_t *d = doc_all(&nf);
            press(CTRL('s'));
            settle();
            if (mclipn > CLIP_KEEP)  // too big to keep over a save
                mclipn = 0;
            CHECK(!F->modified && !F->dlg, "round %d: Ctrl-S didn't save: %s", round, F->msg);
            uint32_t nfile;
            uint8_t *fb = file_all("TEST.BIN", &nfile);
            CHECK(fb && nfile == nf && !memcmp(fb, d, nf), "round %d: the file saved isn't the document", round);
            CHECK(!F->modified && !F->nundo && F->np <= 1 && F->addlen <= mclipn, "round %d: after a save: %d undo, %d pieces",
                  round, F->nundo, F->np);
            CHECK(F->cliplen == mclipn, "round %d: the clipboard went in the save (%lu, %lu)", round,
                  (unsigned long)F->cliplen, (unsigned long)mclipn);
            struct stat st;
            CHECK(stat(TMP_NAME, &st), "the save left %s", TMP_NAME);
            free(fb);
            free(d);
        }
    }
out:
    printf("random edits: %d pieces, %lu bytes typed, %lu bytes\n", F->np, (unsigned long)F->addlen,
           (unsigned long)F->size);

    // A fresh start for the rest.
    file_put("TEST.BIN", orig, sizeof orig);
    CHECK(load("TEST.BIN"), "load again");
    F->ins = false;
    F->text = false;
    F->sel = false;

    // Typing hex digits by hand: overwrite, then insert, then the second digit only.
    press(KEY_HOME);
    type("de");
    CHECK(doc_byte(0) == 0xde && F->cur == 1, "typing DE: %02x at %lu", doc_byte(0), (unsigned long)F->cur);
    press(KEY_INS);
    type("A");
    CHECK(F->size == 5001 && doc_byte(1) == 0xa0 && F->cur == 1, "insert A: %02x", doc_byte(1));
    type("5");
    CHECK(doc_byte(1) == 0xa5 && doc_byte(2) == orig[1] && F->cur == 2, "insert A5");
    press(KEY_INS);
    press(CTRL('z'));
    CHECK(F->size == 5000 && doc_byte(0) == orig[0], "undo of typing DE A5 as one step: %02x", doc_byte(0));
    press(CTRL('y'));
    CHECK(doc_byte(0) == 0xde && doc_byte(1) == 0xa5, "redo of it");

    // The mouse: a click on the second digit, typing changes only that half.
    int x, y;
    byte_xy(20, false, &x, &y);
    click(x + 8, y, MOUSE_LEFT);
    CHECK(F->cur == 20 && F->lo, "a click on the second digit of byte 20: %lu %d", (unsigned long)F->cur, F->lo);
    uint8_t was = doc_byte(20);
    type("7");
    CHECK(doc_byte(20) == ((was & 0xf0) | 7) && F->cur == 21, "the second digit alone: %02x", doc_byte(20));
    // Dragging selects; Del deletes the selection.
    int x2, y2;
    byte_xy(32, false, &x, &y);
    byte_xy(47, false, &x2, &y2);
    ev(MOUSE_DOWN, MOUSE_LEFT, x, y);
    ev(MOUSE_MOVE, MOUSE_LEFT, x2, y2);
    ev(MOUSE_UP, MOUSE_LEFT, x2, y2);
    uint32_t lo, hi;
    CHECK(sel_range(&lo, &hi) && lo == 32 && hi == 48, "drag selected %lu-%lu", (unsigned long)lo, (unsigned long)hi);
    snap("shot_main.raw", x2 + 4, y2 + 4);
    press(KEY_DEL);
    CHECK(F->size == 5001 - 16 && doc_byte(32) == orig[47], "Del of the selection");  // A5 went in at 1
    press(CTRL('z'));
    // The text column: a click there and typing.
    byte_xy(64, true, &x, &y);
    click(x, y, MOUSE_LEFT);
    CHECK(F->text && F->cur == 64, "a click in the text column");
    type("Hi!");
    CHECK(doc_byte(64) == 'H' && doc_byte(66) == '!', "typed text");
    // A bit flipped with the mouse.
    was = doc_byte(F->cur);
    click(BITS_X + 7 * 12 + 2, INS_Y + 28, MOUSE_LEFT);
    CHECK(doc_byte(F->cur) == (was ^ 1), "the bottom bit: %02x was %02x", doc_byte(F->cur), was);

    // Find: text without case, then hex bytes; Goto.
    press(KEY_HOME), press(KEY_HOME);
    press(CTRL('f'));
    CHECK(F->dlg == DLG_FIND, "Ctrl-F opens Find");
    press('\t');
    type("hello, world");
    snap("shot_find.raw", -1, -1);
    press('\r');
    CHECK(sel_range(&lo, &hi) && lo == 1001 && hi == 1013, "found hello at %lu", (unsigned long)lo);
    F->match_case = true;
    press(KEY_F3);
    CHECK(!sel_range(&lo, &hi) || lo != 1000 || strstr(F->msg, "Not found"), "matching case: %s", F->msg);
    F->match_case = false;
    move_to(0, false);
    press(CTRL('f'));
    F->in_text = false;
    F->in[0] = 0;
    char hx[40];
    snprintf(hx, sizeof hx, "%02x %02X%02x", orig[3000], orig[3001], orig[3002]);
    type(hx);
    press('\r');
    CHECK(sel_range(&lo, &hi) && hi - lo == 3, "hex find: %lu (%s)", (unsigned long)lo, F->msg);
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(doc_byte(lo + i) == orig[3000 + i], "hex find at %lu", (unsigned long)lo);
    press(CTRL('g'));
    type("1F4");
    press('\r');
    CHECK(F->cur == 0x1f4 && !F->dlg, "goto 1F4: %lu", (unsigned long)F->cur);
    press(CTRL('g'));
    press('\t');
    type("+100");
    press('\r');
    CHECK(F->cur == 0x1f4 + 100, "goto +100 decimal: %lu", (unsigned long)F->cur);

    // The menus: a click on Edit, Select all; F10, Right, Right, Enter (Search, Find...); Alt-H; the right button.
    menu_click(1, 7);
    CHECK(sel_range(&lo, &hi) && lo == 0 && hi == F->size && F->menu < 0, "Edit, Select all");
    press(KEY_F10);
    CHECK(F->menu == 0, "F10 opens File");
    press(KEY_RIGHT), press(KEY_RIGHT);
    press(KEY_DOWN);
    press('\r');
    CHECK(F->menu < 0 && F->dlg == DLG_FIND, "Search, Find... from the keys: menu %d dlg %d", F->menu, F->dlg);
    press(KEY_ESC);
    move_to(10, false);
    click(bar_x(1) + 20, 8, MOUSE_LEFT);
    snap("shot_menu.raw", bar_x(1) + 60, 120);
    press(KEY_ESC);
    CHECK(F->menu < 0, "Esc closes the menu");
    press(KEY_ALT + 'e');
    press('i');  // Insert mode by its letter
    CHECK(F->ins && F->menu < 0, "Alt-E, I: insert mode");
    press(KEY_INS);
    byte_xy(40, false, &x, &y);
    ev(MOUSE_DOWN, MOUSE_RIGHT, x, y);
    CHECK(F->menu == NMENUS, "the right button's menu");
    ev(MOUSE_UP, MOUSE_RIGHT, x, y);
    CHECK(F->menu == NMENUS, "letting go where it opened keeps it");
    press(KEY_ESC);
    // A greyed item does nothing.
    F->sel = false;
    menu_click(1, 3);  // Cut
    CHECK(F->menu >= 0, "a greyed Cut ran");
    press(KEY_ESC);

    // Saving under another name, and the file box.
    press(CTRL('n'));
    CHECK(F->dlg == DLG_ASK, "New asks about the changes");
    press(KEY_ESC);
    command(CMD_SAVEAS);
    CHECK(F->dlg == DLG_FILE, "Save as opens the file box");
    snap("shot_save.raw", -1, -1);
    F->fd_name[0] = 0;
    type("COPY.BIN");
    press('\r');
    uint32_t nf, nd;
    uint8_t *fb = file_all("COPY.BIN", &nf), *d = doc_all(&nd);
    CHECK(fb && nf == nd && !memcmp(fb, d, nd) && !strcmp(F->path, "/COPY.BIN"), "Save as COPY.BIN");
    free(fb);
    free(d);
    uint8_t *t = file_all("TEST.BIN", &nf);
    CHECK(t && nf == 5000 && !memcmp(t, orig, 5000), "TEST.BIN untouched by Save as");
    free(t);

    // Checksums: of "123456789" selected, against the catalogue's check values; then of the file.
    move_to(F->size, false);
    F->ins = true;
    if (!F->text)
        press('\t');
    uint32_t at9 = F->cur;
    type("123456789");
    move_to(at9, false);
    mods = SHIFT_BITS;
    move_to(at9 + 9, true);
    mods = 0;
    press(CTRL('k'));
    {
        const sums_t *k = &F->sums;
        CHECK(F->dlg == DLG_SUMS && k->hi - k->lo == 9, "Ctrl-K: dlg %d, %lu bytes", F->dlg,
              (unsigned long)(k->hi - k->lo));
        CHECK(k->crc32 == 0xcbf43926 && k->ccitt == 0x29b1 && k->xmodem == 0x31c3 && k->arc == 0xbb3d &&
                  k->modbus == 0x4b37 && k->crc8 == 0xf4 && k->adler == 0x091e01de && k->fletcher == 0x1ede,
              "check values: %08x %04x %04x %04x %04x %02x %08x %04x", k->crc32, k->ccitt, k->xmodem, k->arc,
              k->modbus, k->crc8, k->adler, k->fletcher);
        CHECK(k->sum32 == 477 && k->sum8 == (477 & 255) && k->xor8 == 0x31, "sums %u %02x xor %02x", k->sum32,
              k->sum8, k->xor8);
    }
    snap("shot_sums.raw", -1, -1);
    press(' ');
    F->sel = false;
    menu_click(2, 4);  // Search, Checksums: the whole file
    CHECK(F->dlg == DLG_SUMS && F->sums.lo == 0 && F->sums.hi == F->size, "Checksums of the file");
    press(KEY_ESC);
    F->ins = false;
    press(CTRL('z'));

    // Help and About.
    press(KEY_F1);
    snap("shot_help.raw", -1, -1);
    press(' ');
    CHECK(!F->dlg, "help closes");

    // No memory: edits say so and change nothing.
    move_to(0, false);
    F->typing = false;
    heap = RESERVE;
    uint8_t b0 = doc_byte(0);
    F->ins = false;  // overwriting needs memory for the undo step
    type("12");
    CHECK(F->size == nd && doc_byte(0) == b0, "an edit with no memory changed the document");
    heap = 1L << 30;
    F->ins = false;

    // A bigger file: the end, the scroll bar, a search across chunks.
    {
        uint32_t big = 3u << 20;
        uint8_t *bb = malloc(big);
        for (uint32_t i = 0; i < big; ++i)
            bb[i] = (uint8_t)(i ^ i >> 8);
        memcpy(bb + big - 100, "NEEDLE", 6);
        file_put("BIG.BIN", bb, big);
        CHECK(load("/BIG.BIN") && F->size == big, "the big file");
        press(KEY_END), press(KEY_END);
        CHECK(F->cur == big && F->top == top_max(), "End End: %lu top %lu", (unsigned long)F->cur,
              (unsigned long)F->top);
        click(SB_X + 4, ROW_Y + 2, MOUSE_LEFT);
        CHECK(F->top == top_max() - ROWS, "a click above the thumb: a page up");
        move_to(0, false);
        F->in_text = true;
        F->dlg = DLG_FIND;
        snprintf(F->in, sizeof F->in, "needle");
        press('\r');
        CHECK(sel_range(&lo, &hi) && lo == big - 100, "found the needle: %lu", (unsigned long)lo);
        snap("shot_big.raw", -1, -1);
        free(bb);
        remove("BIG.BIN");
    }

    stop();
    free(m);
    free(mclip);
    remove("TEST.BIN");
    remove("COPY.BIN");
    if (failures)
        printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
    else
        printf("all checks passed\n");
    return failures != 0;
}
