/**
 * @file uitest.c
 * @brief The sprite editor drawn on a PC: draws a spinning coin with the mouse as a person would (tools, palette, frames), checks undo, the frame tools, saving, loading and PNG export and import, and writes screenshots and the coin.
 *
 *     ./uitest.sh  (builds this, writes sprite_*.png and COIN.SPR into the folder given)
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/sprite/sprite_ui.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
stdio_driver_t console_stdio_driver;          ///< the console's driver
static uint64_t now_us = 1000000;             ///< the clock

void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
bool mouse_event(mouse_event_t *e) { (void)e; return false; }
void mouse_pointer(int mode) { (void)mode; }
void mouse_pointer_update(void) {}
int key_poll(void) { return KEY_NONE; }
void scr_begin(void) {}
void scr_end(void) {}
absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
void sleep_ms(uint32_t ms) { now_us += ms * 1000ull; }

void *lodepng_malloc(size_t size) { return malloc(size); }  ///< LodePNG's allocator
void *lodepng_realloc(void *p, size_t n) { return realloc(p, n); }  ///< and realloc
void lodepng_free(void *p) { free(p); }  ///< and free

/// The test's folder for a FatFs path ("/X" is "./X").
/// @param path the path
/// @return the local one
static const char *local(const char *path) {
    static char buf[300];
    snprintf(buf, sizeof buf, ".%s%s", path[0] == '/' ? "" : "/", path);
    return buf;
}
FRESULT f_open(FIL *f, const char *path, int mode) {
    f->f = fopen(local(path), mode & FA_WRITE ? "wb" : "rb");
    return f->f ? FR_OK : 1;
}
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, f->f); return FR_OK; }
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put) { *put = (UINT)fwrite(buf, 1, n, f->f); return FR_OK; }
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
    return FR_OK;
}
FRESULT f_closedir(DIR *d) { pc_closedir(d->d); return FR_OK; }
FRESULT f_stat(const char *path, FILINFO *fi) {
    struct stat st;
    if (stat(local(path), &st))
        return 1;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fi->fsize = (unsigned long)st.st_size;
    return FR_OK;
}
FRESULT f_getcwd(char *buf, UINT n) { snprintf(buf, n, "/"); return FR_OK; }

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
    if (F->dirty && !F->drag) {
        F->dirty = false;
        draw();
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

/// The screen x of a sprite pixel's middle.
/// @param x the pixel's x
/// @return the screen's
static int sx(int x) { return F->ox + x * F->cell + F->cell / 2; }
/// The screen y of a sprite pixel's middle.
/// @param y the pixel's y
/// @return the screen's
static int sy(int y) { return F->oy + y * F->cell + F->cell / 2; }

/// Drags across the sprite from one pixel to another, through a few points between.
/// @param x0 from x
/// @param y0 from y
/// @param x1 to x
/// @param y1 to y
/// @param btn the button
static void drag(int x0, int y0, int x1, int y1, int btn) {
    ev(MOUSE_DOWN, btn, sx(x0), sy(y0));
    for (int i = 1; i <= 4; ++i)
        ev(MOUSE_MOVE, btn, sx(x0 + (x1 - x0) * i / 4), sy(y0 + (y1 - y0) * i / 4));
    ev(MOUSE_UP, btn, sx(x1), sy(y1));
}

/// Picks a colour from the palette with a button.
/// @param c the colour
/// @param btn MOUSE_LEFT or _RIGHT
static void colour(int c, int btn) {
    click(PAL_X + (c & 15) * SW + SW / 2, PAL_Y + (c >> 4) * SW + SW / 2, btn);
}

/// Picks a tool with its button.
/// @param t T_*
static void tool(int t) {
    click(PAL_X + (t & 3) * 70 + 30, TOOL_Y + (t >> 2) * 24 + 10, MOUSE_LEFT);
}

/// Clicks a file button.
/// @param b FB_*
static void file_btn(int b) {
    click(4 + b * 63 + 30, FILE_Y + 12, MOUSE_LEFT);
}

/// Types text.
/// @param s it
static void type(const char *s) {
    while (*s)
        press((uint8_t)*s++);
}

/// Pixels drawn in a frame.
/// @param f the frame
/// @return how many
static int drawn(const frame_t *f) {
    int n = 0;
    for (int k = 0; k < F->w * F->h; ++k)
        n += f->on[k];
    return n;
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

/// Draws one frame of the coin: a gold disc as wide as it is turned, its rim, and a shine.
/// @param half its half width (0: edge on)
static void coin_frame(int half) {
    colour(0xf8, MOUSE_LEFT);  // gold
    tool(T_DISC);
    drag(7 - half, 1, 8 + half, 14, MOUSE_LEFT);
    colour(0x8c, MOUSE_LEFT);  // the rim, darker
    tool(T_OVAL);
    drag(7 - half, 1, 8 + half, 14, MOUSE_LEFT);
    if (half >= 4) {  // a shine, and a stamped mark in the middle
        colour(0xff, MOUSE_LEFT);
        tool(T_LINE);
        drag(7 - half / 2, 4, 7 - half / 2, 7, MOUSE_LEFT);
        colour(0xd0, MOUSE_LEFT);
        tool(T_RECT);
        drag(7, 6, 8, 9, MOUSE_LEFT);
    }
}

int main(void) {
    remove("COIN.SPR");
    remove("COIN.PNG");
    start();
    F->g = malloc(W * H);
    draw();
    // The coin: eight frames of 16x16, drawn with the mouse.
    static const int halves[8] = {7, 6, 4, 2, 0, 2, 4, 6};  // turning
    for (int i = 0; i < 8; ++i) {
        if (i)
            click(8 + 20, 412 + 10, MOUSE_LEFT);  // Add
        coin_frame(halves[i]);
        CHECK(drawn(&F->fr[i]) > 0, "frame %d is empty", i);
    }
    CHECK(F->nfr == 8 && F->cur == 7, "%d frames, on %d", F->nfr, F->cur);
    CHECK(F->fr[0].px[8 * 16 + 1] == 0xf8 || F->fr[0].px[8 * 16 + 1] == 0x8c, "frame 1's left edge %02x",
          F->fr[0].px[8 * 16 + 1]);
    CHECK(!F->fr[4].on[8 * 16 + 2], "edge-on frame drawn at x 2");
    // Undo and redo, and the right button drawing see-through.
    frame_t keep = F->fr[F->cur];
    tool(T_PENCIL);
    drag(0, 0, 15, 15, MOUSE_LEFT);
    CHECK(memcmp(&keep, &F->fr[F->cur], sizeof keep), "the pencil drew nothing");
    file_btn(FB_UNDO);
    CHECK(!memcmp(&keep, &F->fr[F->cur], sizeof keep), "undo didn't put it back");
    file_btn(FB_REDO);
    CHECK(F->fr[F->cur].on[0] && F->fr[F->cur].on[255], "redo didn't draw it again");
    press('u');
    drag(5, 5, 5, 5, MOUSE_RIGHT);
    CHECK(!F->fr[F->cur].on[5 * 16 + 5], "the right button (none) left a pixel");
    press('u');
    CHECK(!memcmp(&keep, &F->fr[F->cur], sizeof keep), "undo after the right button");
    // Flips and rotations come back to where they started; a shift round and back too.
    for (int i = 0; i < 2; ++i)
        click(PAL_X + 30, TOOL_Y + 60, MOUSE_LEFT);  // Flip H
    press('v'), press('v');
    for (int i = 0; i < 4; ++i)
        press('t');
    press(KEY_LEFT), press(KEY_UP), press(KEY_RIGHT), press(KEY_DOWN);
    CHECK(!memcmp(&keep, &F->fr[F->cur], sizeof keep), "flips, rotations and shifts changed the frame");
    press('h');
    CHECK(F->fr[F->cur].px[8 * 16 + 1] == keep.px[8 * 16 + 14], "Flip H didn't mirror it");
    press(CTRL('z'));
    CHECK(!memcmp(&keep, &F->fr[F->cur], sizeof keep), "undoing the frame tools");
    // Fill: the background of frame 3 in blue, then undone.
    press(KEY_HOME);
    press(KEY_PGDN), press(KEY_PGDN);
    tool(T_FILL);
    colour(0x03, MOUSE_LEFT);
    click(sx(0), sy(0), MOUSE_LEFT);
    CHECK(F->fr[2].on[0] && F->fr[2].px[0] == 0x03 && F->fr[2].on[15 * 16 + 15], "fill missed the corners");
    CHECK(F->fr[2].px[8 * 16 + 8] != 0x03, "fill went into the coin");
    press('u');
    CHECK(!F->fr[2].on[0], "undo of the fill");
    press(KEY_HOME);
    // The picker takes a colour; the middle button too.
    tool(T_PICK);
    click(sx(8), sy(3), MOUSE_RIGHT);
    CHECK(F->col[1] == px_col(&F->fr[0], 3 * 16 + 8), "picker gave %d", F->col[1]);
    colour(0xff, MOUSE_RIGHT);
    tool(T_PENCIL);
    click(sx(0), sy(0), MOUSE_MIDDLE);
    CHECK(F->col[0] == NONE, "the middle button picked %d from a see-through pixel", F->col[0]);
    colour(0xfc, MOUSE_LEFT);
    colour(0x24, MOUSE_RIGHT);
    // The main screenshot, the pointer over the coin and the preview playing.
    F->playing = true;
    F->anim = 2;
    ev(MOUSE_MOVE, 0, sx(9), sy(5));
    snap("shot_main.raw", sx(9), sy(5));
    F->playing = false;
    // Save as COIN through the file box, then load it back.
    CHECK(F->modified, "not modified after drawing");
    file_btn(FB_SAVE);  // no name yet: the file box
    CHECK(F->dlg == DLG_FILE && F->fd_mode == FD_SAVE, "Save opened %d", F->dlg);
    type("COIN");
    press('\r');
    CHECK(F->dlg == DLG_NONE && !F->modified && !strcmp(F->path, "/COIN.SPR"), "saved as '%s' (modified %d)", F->path,
          F->modified);
    frame_t *saved = malloc(8 * sizeof *saved);
    memcpy(saved, F->fr, 8 * sizeof *saved);
    // Export the PNG, then the file box for loading, with both in it.
    file_btn(FB_PNG);
    CHECK(!strcmp(F->fd_name, "COIN.PNG"), "PNG name '%s'", F->fd_name);
    press('\r');
    FILINFO fi;
    CHECK(f_stat("/COIN.PNG", &fi) == FR_OK, "no COIN.PNG");
    mkdir("SPRITES", 0755);
    file_btn(FB_LOAD);
    CHECK(F->dlg == DLG_FILE && F->fd_n == 3, "the load box lists %d", F->fd_n);
    press(KEY_DOWN);
    snap("shot_load.raw", 300, 140);
    // Load the PNG: the same frames come back (the colours are exact).
    CHECK(!strcmp(F->fd_name, "COIN.PNG"), "chose '%s'", F->fd_name);
    press('\r');
    CHECK(F->nfr == 8 && F->w == 16 && F->h == 16, "PNG: %d frames %dx%d", F->nfr, F->w, F->h);
    for (int i = 0; i < 8; ++i)
        CHECK(!memcmp(&saved[i], &F->fr[i], sizeof *saved), "PNG frame %d differs", i);
    CHECK(F->modified && !strcmp(F->path, "/COIN.SPR"), "after the PNG: '%s' modified %d", F->path, F->modified);
    // A change, then Load asks first: Don't save, then COIN.SPR.
    tool(T_PENCIL);
    drag(0, 0, 3, 0, MOUSE_LEFT);
    file_btn(FB_LOAD);
    CHECK(F->dlg == DLG_ASK, "Load didn't ask about the changes");
    snap("shot_ask.raw", 330, 248);
    click(W / 2 - 230 + 16 + 128 + 60, 180 + 52 + 12, MOUSE_LEFT);  // Don't save
    CHECK(F->dlg == DLG_FILE, "Don't save didn't open the file box");
    type("COIN.SPR");
    press('\r');
    for (int i = 0; i < 8; ++i)
        CHECK(!memcmp(&saved[i], &F->fr[i], sizeof *saved), "SPR frame %d differs", i);
    CHECK(!F->modified, "modified after loading");
    // The size box: 32x32 keeps the coin top left.
    file_btn(FB_SIZE);
    click(W / 2 - 230 + 170 + 3 * 50 + 20, 150 + 40 + 10, MOUSE_LEFT);  // width 32
    snap("shot_size.raw", W / 2 - 230 + 170 + 3 * 50 + 20, 150 + 40 + 10);
    click(W / 2 - 230 + 170 + 3 * 50 + 20, 150 + 70 + 10, MOUSE_LEFT);  // height 32
    press('\r');
    CHECK(F->w == 32 && F->h == 32 && F->fr[0].px[8 * 32 + 1] == saved[0].px[8 * 16 + 1], "resized to %dx%d",
          F->w, F->h);
    press(KEY_F1);
    snap("shot_help.raw", -1, -1);
    press(' ');
    CHECK(F->dlg == DLG_NONE && !F->playing, "a key closes the help without playing");
    // Quit asks; Cancel stays.
    press(KEY_ESC);
    CHECK(F->dlg == DLG_ASK && !F->quit, "Esc with changes didn't ask");
    press(KEY_ESC);
    CHECK(F->dlg == DLG_NONE && !F->quit, "Esc didn't cancel the question");
    // Frames: copy, move, delete.
    press(KEY_HOME);
    press(KEY_INS);
    CHECK(F->nfr == 9 && F->cur == 1 && !memcmp(&F->fr[0], &F->fr[1], sizeof *F->fr), "Ins didn't copy");
    click(8 + 44 + 4 + 48 + 4 + 60 + 4 + 30, 412 + 10, MOUSE_LEFT);  // Move left
    CHECK(F->cur == 0, "moved to %d", F->cur);
    click(8 + 44 + 4 + 48 + 4 + 30, 412 + 10, MOUSE_LEFT);  // Delete asks
    CHECK(F->dlg == DLG_ASK, "Delete didn't ask");
    press('\r');
    CHECK(F->nfr == 8, "%d frames after deleting", F->nfr);
    free(saved);
    stop();
    printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}
