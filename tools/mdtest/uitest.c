/**
 * @file uitest.c
 * @brief The Markdown viewer on a PC: a sample page (and its pictures) laid out and drawn, links followed,
 *        Back, Find, the contents, the menus and the mouse; random Markdown and random keys and clicks
 *        (under the address sanitizer); and screenshots.
 *
 *     ./uitest.sh  (builds this, writes md_*.png into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/md/md_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock
static uint8_t mods;                          ///< the modifier keys held
bool pc_psram_full;                           ///< PSRAM has run out
static int www_runs;                          ///< www_command was called
static char www_url[400];                     ///< with this

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return 'q'; }  // (show_picture: a key closes it)
bool key_stop(void) { return false; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }
uint8_t usb_keyboard_modifiers(void) { return mods; }
void www_command(int argc, char **argv) {
    ++www_runs;
    snprintf(www_url, sizeof www_url, "%s", argc > 1 ? argv[1] : "");
}

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
    struct stat st;
    if (!stat(local(path), &st) && S_ISDIR(st.st_mode))
        return 1;
    f->f = fopen(local(path), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
FRESULT f_lseek(FIL *f, FSIZE_t at) { return fseek(f->f, (long)at, SEEK_SET) ? 1 : FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->f) ? 1 : FR_OK; }
FSIZE_t f_tell(FIL *f) { return (FSIZE_t)ftell(f->f); }
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
static char dir_open[300];  ///< the folder f_opendir opened (for the sizes)
FRESULT f_opendir(DIR *d, const char *path) {
    snprintf(dir_open, sizeof dir_open, "%s", local(path));
    d->d = pc_opendir(dir_open);
    return d->d ? FR_OK : 1;
}
FRESULT f_readdir(DIR *d, FILINFO *fi) {
    int is_dir = 0;
    pc_readdir(d->d, fi->fname, sizeof fi->fname, &is_dir);
    fi->fattrib = is_dir ? AM_DIR : 0;
    fi->fsize = 0;
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir_open, fi->fname);
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
FRESULT f_rename(const char *from, const char *to) { return rename(local(from), local(to)) ? 1 : FR_OK; }

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

/// What the viewer's loop does: draws again, and draws the pictures on the screen.
static void settle(void) {
    for (int guard = 0; guard < 50; ++guard) {
        if (M->dirty) {
            M->dirty = false;
            draw();
        } else if (M->pic_wanted && M->menu < 0 && M->dlg == DLG_NONE) {
            if (!pic_load_one())
                M->pic_wanted = false;
            M->dirty = true;
        } else {
            break;
        }
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

/// Saves the screen (with an arrow drawn where the pointer would be, if x >= 0).
/// @param name the file
/// @param x the pointer
/// @param y the pointer
static void snap(const char *name, int x, int y) {
    static uint8_t shot[DVI_WIDTH * DVI_HEIGHT];
    memcpy(shot, dvi_framebuf, sizeof shot);
    for (int r = 0; x >= 0 && r < 12; ++r)
        for (int c = 0; c <= r / 2 + 1 && x + c < DVI_WIDTH && y + r < DVI_HEIGHT; ++c)
            shot[(y + r) * DVI_WIDTH + x + c] = c == 0 || c == r / 2 + 1 || r == 11 ? 0x00 : 0xff;
    char path[300];
    snprintf(path, sizeof path, "../%s", name);  // beside the pretend drive
    FILE *f = fopen(path, "wb");
    fwrite(shot, 1, sizeof shot, f);
    fclose(f);
}

/// The first text item with this text in it.
/// @param s the text
/// @return its index, or -1
static int find_text(const char *s) {
    const md_doc_t *d = M->doc;
    char buf[1100];
    for (int i = 0; i < d->nitems; ++i) {
        if (d->items[i].kind != MI_TEXT)
            continue;
        snprintf(buf, sizeof buf, "%.*s", d->items[i].len, d->pool + d->items[i].ref);
        if (strstr(buf, s))
            return i;
    }
    return -1;
}

/// The items' text from first to last, joined.
/// @param out it
/// @param size room
static void all_text(char *out, int size) {
    const md_doc_t *d = M->doc;
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < d->nitems && n < size - 2; ++i)
        if (d->items[i].kind == MI_TEXT)
            n += snprintf(out + n, size - n, "%.*s|", d->items[i].len, d->pool + d->items[i].ref);
}

/// Checks that the document is well formed: items in order, on the page, text where it says.
/// @param what for messages
static void check_doc(const char *what) {
    const md_doc_t *d = M->doc;
    if (!d)
        return;
    for (int i = 0; i < d->nitems; ++i) {
        const md_item_t *it = &d->items[i];
        CHECK(!i || it->y >= d->items[i - 1].y, "%s: item %d out of order", what, i);
        CHECK(it->h <= MD_ITEM_MAX_H, "%s: item %d too tall (%d)", what, i, it->h);
        CHECK(it->x >= 0 && it->x + it->w <= d->width + 2, "%s: item %d off the page (x %d w %d)", what, i, it->x,
              it->w);
        CHECK(it->y >= 0 && it->y + it->h <= d->height, "%s: item %d below the end", what, i);
        if (it->kind == MI_TEXT)
            CHECK(it->ref + it->len <= d->npool, "%s: item %d's text out of the pool", what, i);
        if (it->kind == MI_IMG)
            CHECK((int)it->ref < d->nimgs, "%s: item %d's picture %u", what, i, (unsigned)it->ref);
        CHECK(it->link <= d->nlinks, "%s: item %d's link", what, i);
    }
}

/// Random Markdown, made of pieces that each mean something.
/// @param out it
/// @param size room
/// @return its length
static int random_md(char *out, int size) {
    static const char *const bits[] = {
        "# ", "## ", "###### ", "#hash", "\n", "\n\n", "  \n", "- ", "* ", "+ ", "1. ", "2) ", "10. ", "> ", ">",
        "    ", "  ", "```", "```c\n", "~~~", "---", "***", "___", "===", "|", " | ", "|---|", ":-:", "*", "**",
        "***", "_", "__", "~~", "`", "``", "[", "]", "(", ")", "![", "](pic.png)", "](sub/OTHER.md)", "[x] ",
        "[ ] ", "<", ">", "<br>", "<img src=\"pic.png\" width=\"40\">", "<b>", "</b>", "<a href=\"#x\">", "</a>",
        "<!--", "-->", "&amp;", "&#169;", "&", "\\", "\\*", "http://a.b/c ", "word ", "longerwordthanmost ",
        "caf\xc3\xa9 ", "\xe2\x80\x94", "\xff", "\t", "[r]: /x.md\n", "[r]", "[r][]", "\x01",
    };
    int n = 0, parts = rand() % 200;
    for (int i = 0; i < parts; ++i) {
        const char *b = bits[rand() % (sizeof bits / sizeof *bits)];
        int l = (int)strlen(b);
        if (n + l >= size)
            break;
        memcpy(out + n, b, l);
        n += l;
    }
    out[n] = 0;
    return n;
}

int main(void) {
    srand(1);
    // The pictures and pages are made by uitest.sh in the folder this runs in.
    CHECK(start(), "start");
    CHECK(open_doc("/README.md", NULL, true), "open README.md");
    settle();
    check_doc("README");
    const md_doc_t *d = M->doc;
    CHECK(d->nheads >= 5, "headings: %d", d->nheads);
    CHECK(d->nimgs == 3, "pictures that can be shown: %d", d->nimgs);
    CHECK(d->nlinks == 5, "links: %d", d->nlinks);
    char t[16384];
    all_text(t, sizeof t);
    CHECK(strstr(t, "Pico 2 mini OS") != NULL, "the setext heading");
    CHECK(strstr(t, "**") == NULL, "no ** left: %s", t);
    CHECK(strstr(t, "inline") != NULL, "code span");
    CHECK(strstr(t, "[no such picture]") != NULL, "a missing picture's alt text");
    CHECK(strstr(t, "[build]") != NULL, "a web picture's alt text");
    CHECK(strstr(t, "caf\x82") != NULL, "UTF-8 \xc3\xa9 as CP437 0x82");
    CHECK(strstr(t, "[wiki]") == NULL, "reference definitions are not text");
    int i = find_text("Bullets");
    CHECK(i >= 0 && d->items[i].x >= 20, "a bullet's indent");
    i = find_text("This paragraph belongs");
    CHECK(i >= 0 && d->items[i].x >= 30, "a list item's second paragraph is indented (%d)", i >= 0 ? d->items[i].x : -1);
    i = find_text("hello, world");
    CHECK(i >= 0 && (d->items[i].flags & MF_CODE), "a fenced code block");
    i = find_text("keeps its spaces");
    CHECK(i >= 0, "indented code");
    i = find_text("Shows Markdown");
    CHECK(i >= 0, "a table cell");
    i = find_text("A task that is done");
    CHECK(i >= 0, "a task");
    int pics_drawn = 0;
    for (int k = 0; k < MAX_PICS; ++k)
        pics_drawn += M->pics[k].px != NULL;
    CHECK(pics_drawn >= 1, "the picture at the top is drawn");
    snap("shot_top.raw", -1, -1);

    // Scrolling and the pictures further down.
    press(KEY_PGDN);
    snap("shot_lists.raw", -1, -1);
    goto_anchor("tables-and-pictures");
    settle();
    CHECK(M->top > 0, "#tables-and-pictures scrolled (%d)", (int)M->top);
    snap("shot_table.raw", -1, -1);
    press(KEY_END);
    CHECK(M->top == top_max(), "End");
    snap("shot_end.raw", -1, -1);
    press(KEY_HOME);
    CHECK(M->top == 0, "Home");

    // Links: Tab chooses the first, Enter follows it to sub/OTHER.md; Back comes back.
    press('\t');
    CHECK(M->cur_link > 0, "Tab chose a link");
    CHECK(!strcmp(link_url(M->cur_link), "sub/OTHER.md"), "the first link: %s", link_url(M->cur_link));
    snap("shot_link.raw", -1, -1);
    press('\r');
    CHECK(!strcmp(M->path, "/sub/OTHER.md"), "followed to %s", M->path);
    CHECK(M->nhist == 2 && M->hpos == 1, "history %d %d", M->nhist, M->hpos);
    i = find_text("Back up");
    CHECK(i >= 0, "OTHER.md's text");
    snap("shot_other.raw", -1, -1);
    press(8);
    CHECK(!strcmp(M->path, "/README.md"), "back to %s", M->path);
    press(']');
    CHECK(!strcmp(M->path, "/sub/OTHER.md"), "forward to %s", M->path);
    press('[');
    // A reference link with a #name in another file.
    d = M->doc;
    int ref = -1;
    for (int k = 0; k < d->nlinks; ++k)
        if (!strcmp(d->pool + d->links[k], "sub/OTHER.md#second-heading"))
            ref = k + 1;
    CHECK(ref > 0, "the reference link");
    M->cur_link = ref;
    press('\r');
    CHECK(!strcmp(M->path, "/sub/OTHER.md") && M->top > 0, "to OTHER.md#second-heading (top %d)", (int)M->top);
    press('[');
    // A web link asks, then runs www.
    d = M->doc;
    for (int k = 0; k < d->nlinks; ++k)
        if (!strncmp(d->pool + d->links[k], "https://example.com", 19))
            M->cur_link = k + 1;
    press('\r');
    CHECK(M->dlg == DLG_ASK, "a web link asks");
    snap("shot_ask.raw", -1, -1);
    press('\r');
    CHECK(M->www, "www is to run");
    M->www = false;
    run_www(M->ask_url);
    CHECK(www_runs == 1 && !strcmp(www_url, "https://example.com/"), "www ran (%d, %s)", www_runs, www_url);

    // Find.
    press(CTRL('f'));
    type("WRAP");
    press('\r');
    CHECK(M->find_item >= 0, "found 'wrap'");
    snap("shot_find.raw", -1, -1);
    press(KEY_F3);
    // The contents.
    press(CTRL('t'));
    CHECK(M->dlg == DLG_TOC, "contents");
    press(KEY_DOWN);
    press(KEY_DOWN);
    snap("shot_toc.raw", -1, -1);
    press('\r');
    CHECK(M->dlg == DLG_NONE && M->top > 0, "went to a heading");
    // The menus.
    press(KEY_F10);
    press(KEY_RIGHT);
    snap("shot_menu.raw", bar_x(1) + 60, 70);
    press(KEY_ESC);
    // The source view.
    press(KEY_F6);
    CHECK(M->source, "source view");
    check_doc("source");
    all_text(t, sizeof t);
    CHECK(strstr(t, "**Raspberry Pi Pico 2**") != NULL, "the source as it is");
    snap("shot_source.raw", -1, -1);
    press(KEY_F6);
    // Help and About.
    press(KEY_F1);
    snap("shot_help.raw", -1, -1);
    press(' ');
    // The file box.
    press(CTRL('o'));
    CHECK(M->dlg == DLG_FILE && M->fd_n >= 3, "the file box (%d)", M->fd_n);
    snap("shot_open.raw", -1, -1);
    press(KEY_ESC);
    // The mouse: a click on a link follows it; the right button's menu.
    press(KEY_HOME);
    i = find_text("a link to another page");
    if (i >= 0) {
        const md_item_t *it = &M->doc->items[i];
        int x = MARGIN + it->x + 4, y = VIEW_Y + (int)(it->y - M->top) + 4;
        ev(MOUSE_MOVE, 0, x, y);
        CHECK(M->hover_link == it->link, "hover");
        CHECK(M->hover_link > 0, "hover on a link");
        click(x, y, MOUSE_LEFT);
        CHECK(!strcmp(M->path, "/sub/OTHER.md"), "clicked to %s", M->path);
        press('[');
    }
    click(300, 200, MOUSE_RIGHT);
    CHECK(M->menu == NMENUS, "the right button's menu");
    snap("shot_popup.raw", 330, 230);
    press(KEY_ESC);
    // The scroll bar.
    click(SB_X + 5, STAT_Y - 4, MOUSE_LEFT);
    CHECK(M->top > 0, "the scroll bar paged");

    // Random keys and clicks.
    static const int keys[] = {KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, '\t', '\r', KEY_LEFT,
                               KEY_RIGHT, '[', ']', 't', 'n', KEY_F6, '*', KEY_ESC, KEY_F10, 'x', 'a', ' ', 8};
    for (int r = 0; r < 3000; ++r) {
        if (M->quit)
            M->quit = false;
        if (M->dlg == DLG_ASK)
            press(KEY_ESC);
        if (rand() % 4) {
            int k = keys[rand() % (sizeof keys / sizeof *keys)];
            mods = rand() % 5 == 0 ? 0x02 : 0;
            press(k);
        } else {
            int x = rand() % W, y = rand() % H;
            int ty = rand() % 4;
            ev(ty == 0 ? MOUSE_MOVE : ty == 1 ? MOUSE_DOWN : ty == 2 ? MOUSE_UP : MOUSE_WHEEL,
               rand() % 2 ? MOUSE_LEFT : MOUSE_RIGHT, x, y);
        }
        if (M->doc && M->path[0])
            check_doc("random keys");
    }
    mods = 0;

    // Random Markdown laid out.
    static char md[8192];
    for (int r = 0; r < 3000; ++r) {
        int n = random_md(md, sizeof md);
        md_doc_t *rd = md_layout(md, n, r % 7 ? PAGE_W : 200 + r % 300, r % 13 == 0, size_of, NULL);
        CHECK(rd != NULL, "random layout %d", r);
        md_doc_t *keep = M->doc;
        M->doc = rd;
        check_doc("random markdown");
        if (r % 50 == 0)
            draw();
        M->doc = keep;
        md_free(rd);
    }
    // Running out of memory part of the way.
    pc_psram_full = false;
    stop();
    printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
