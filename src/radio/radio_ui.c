/**
 * @file radio_ui.c
 * @brief The `radio` command: a graphical station browser in the tracker's style (bevelled panels, a scanout mouse arrow), drawn off the screen and copied on a region at a time so nothing flickers.
 *
 * Favourites, the last station and the volume are kept in /RADIO.INI.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "font8x16.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "radio.h"

/// The screen's width.
#define W DVI_WIDTH
/// Its height.
#define H DVI_HEIGHT
/// The settings file.
#define INI_PATH "/RADIO.INI"
/// The most favourites.
#define MAX_FAVS 60
/// The most search results.
#define MAX_FOUND 100

/// The tracker's colours (RGB332).
#define C_BG     0x00
#define C_FACE   0x25  ///< panels
#define C_LIGHT  0x6e  ///< bevel highlight
#define C_DARK   0x01  ///< bevel shadow
#define C_TEXT   0xff  ///< text
#define C_DIM    0x92  ///< quiet text
#define C_LABEL  0xb6  ///< labels
#define C_YELLOW 0xfc  ///< titles
#define C_CYAN   0x5f  ///< headings
#define C_GREEN  0x3c  ///< good
#define C_ORANGE 0xf4  ///< warnings
#define C_RED    0xe0  ///< errors
#define C_FIELD  0x00  ///< sunken fields
#define C_SEL    0x2b  ///< selected rows
#define C_ROWALT 0x05  ///< every other row
#define C_STRIP  0x0b  ///< the title strip

/// Layout
#define TOP_H    24
#define TABS_Y   28  ///< the tabs' top
#define LIST_X   4  ///< the list's left edge
#define LIST_Y   56  ///< its top
#define LIST_W   392  ///< its width
#define LIST_H   384  ///< its height
#define ROW_H    20  ///< a row's height
#define ROWS     ((LIST_H - 8) / ROW_H)  ///< rows shown
#define PANEL_X  400  ///< the panel's left edge
#define PANEL_W  236  ///< its width
#define STATUS_Y 448  ///< the status line's top
#define BTN_W(chars) ((chars) * 8 + 16)  ///< a button that fits that many characters
#define MUTE_W   BTN_W(5)                 ///< "Sound" / "Mute", the wider of them
#define VOL_W    (PANEL_W - 64 - 8 - MUTE_W - 8)  ///< the volume slider, the mute button after it

/// The built-in station list.
static const radio_station_t starter[] = {
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3", 128, "US", "MP3"},
    {"SomaFM Groove Salad (AAC)", "http://ice1.somafm.com/groovesalad-128-aac", 128, "US", "AAC"},
    {"SomaFM Groove Salad (HE-AAC)", "http://ice1.somafm.com/groovesalad-64-aac", 64, "US", "AAC+"},
    {"SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3", 128, "US", "MP3"},
    {"SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3", 128, "US", "MP3"},
    {"SomaFM Indie Pop Rocks!", "http://ice1.somafm.com/indiepop-128-mp3", 128, "US", "MP3"},
    {"SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3", 128, "US", "MP3"},
    {"SomaFM Space Station Soma", "http://ice1.somafm.com/spacestation-128-mp3", 128, "US", "MP3"},
    {"SomaFM DEF CON Radio", "http://ice1.somafm.com/defcon-128-mp3", 128, "US", "MP3"},
    {"SomaFM Boot Liquor", "http://ice1.somafm.com/bootliquor-128-mp3", 128, "US", "MP3"},
    {"Radio Paradise Main Mix", "http://stream.radioparadise.com/mp3-128", 128, "US", "MP3"},
    {"Radio Paradise Mellow Mix", "http://stream.radioparadise.com/mellow-128", 128, "US", "MP3"},
    {"Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128", 128, "CH", "MP3"},
    {"Radio Swiss Classic", "http://stream.srg-ssr.ch/m/rsc_de/mp3_128", 128, "CH", "MP3"},
    {"Radio Swiss Pop", "http://stream.srg-ssr.ch/m/rsp/mp3_128", 128, "CH", "MP3"},
    {"181.FM The Eagle (classic rock)", "http://listen.181fm.com/181-eagle_128k.mp3", 128, "US", "MP3"},
    {"181.FM Classical Guitar", "http://listen.181fm.com/181-classicalguitar_128k.mp3", 128, "US", "MP3"},
};
/// Its length.
#define NSTARTER ((int)(sizeof starter / sizeof *starter))

/// The lists.
enum { TAB_FAVS, TAB_STATIONS, TAB_FOUND, NTABS };

static struct {
    uint8_t *g;                 ///< the off-screen picture (PSRAM)
    radio_station_t *favs;      ///< PSRAM
    int nfavs;  ///< their count
    radio_station_t *found;     ///< search results (PSRAM)
    int nfound;  ///< their count
    char search[64];  ///< the search text
    /// The list shown, and each list's selected row and top row.
    ///@{
    int tab, sel[NTABS], top[NTABS];
    ///@}
    /// The last station played: its address and name.
    ///@{
    char last_url[200], last_name[64];
    ///@}
    char msg[96];  ///< a message for the status line
    uint32_t msg_until;  ///< until when
    bool dragging_volume;  ///< the volume slider is being dragged
    bool quit;  ///< leaving
} U;  ///< the browser's state

// ---- Drawing into the off-screen picture -------------------------------------------------------------

/// Where drawing goes: the off-screen picture, or (for the meters, drawn a
/// pixel once each so nothing flickers) the screen itself.
static uint8_t *dst;

/// Fills a rectangle (clipped).
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int r = 0; r < h; ++r)
        memset(dst + (y + r) * W + x, c, w);
}

/// Draws a character.
/// @param x left
/// @param y top
/// @param c the character
/// @param fg its colour
/// @param bg the background
static void ch8(int x, int y, uint8_t c, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x > W - 8 || y > H - 16)
        return;
    const uint8_t *gl = font8x16[c];
    uint8_t *d = dst + y * W + x;
    for (int r = 0; r < 16; ++r, d += W)
        for (int i = 0; i < 8; ++i)
            d[i] = gl[r] & (0x80 >> i) ? fg : bg;
}

/// Text cut to n characters (padded with the background if pad).
/// @param x left
/// @param y top
/// @param s the text
/// @param n the most characters
/// @param fg its colour
/// @param bg the background
/// @param pad pad to n
/// @return the x after it
static int text(int x, int y, const char *s, int n, uint8_t fg, uint8_t bg, bool pad) {
    int i = 0;
    for (; s[i] && i < n; ++i)
        ch8(x + i * 8, y, (uint8_t)s[i], fg, bg);
    if (pad)
        for (int k = i; k < n; ++k)
            ch8(x + k * 8, y, ' ', fg, bg);
    return x + i * 8;
}

/// A bevelled rectangle.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param raised raised (else sunken)
/// @param face its colour
static void bevel(int x, int y, int w, int h, bool raised, uint8_t face) {
    fill(x, y, w, h, face);
    uint8_t a = raised ? C_LIGHT : C_DARK, b = raised ? C_DARK : C_LIGHT;
    fill(x, y, w, 1, a);
    fill(x, y, 1, h, a);
    fill(x, y + h - 1, w, 1, b);
    fill(x + w - 1, y, 1, h, b);
}

/// A button.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param label its text
/// @param lit pressed
static void button(int x, int y, int w, int h, const char *label, bool lit) {
    uint8_t face = lit ? C_SEL : C_FACE;
    bevel(x, y, w, h, !lit, face);
    int n = strlen(label);
    text(x + (w - n * 8) / 2, y + (h - 16) / 2, label, n, lit ? C_YELLOW : C_TEXT, face, false);
}

/// Copies a region of the picture onto the screen.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
static void show(int x, int y, int w, int h) {
    for (int r = 0; r < h; ++r)
        memcpy(dvi_framebuf + (y + r) * W + x, U.g + (y + r) * W + x, w);
}

// ---- Places to click ---------------------------------------------------------------------------------

/// What a click hits.
enum { HIT_TAB = 1, HIT_ROW, HIT_STAR, HIT_STOP, HIT_PLAY, HIT_FAV, HIT_VOLUME, HIT_MUTE, HIT_SEARCH, HIT_URL, HIT_QUIT };

typedef struct {
    ///@{
    int16_t x, y, w, h, id, arg;
    ///@}
} hit_t;
static hit_t *hits;  ///< 64 of them, while the browser is open (PSRAM)
/// Their count.
static int nhits;

/// Remembers a place to click.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param id HIT_*
/// @param arg which
static void hit(int x, int y, int w, int h, int id, int arg) {
    if (hits && nhits < 64)
        hits[nhits++] = (hit_t){x, y, w, h, id, arg};
}

/// The place under a point.
/// @param x its x
/// @param y its y
/// @return the place, or NULL
static const hit_t *hit_at(int x, int y) {
    for (int i = nhits - 1; i >= 0; --i)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return &hits[i];
    return NULL;
}

// ---- Stations and the INI file ---------------------------------------------------------------------------

/// A list's length.
/// @param tab the list
/// @return its length
static int list_len(int tab) {
    return tab == TAB_FAVS ? U.nfavs : tab == TAB_STATIONS ? NSTARTER : U.nfound;
}

/// A list's station.
/// @param tab the list
/// @param i the row
/// @return it, or NULL
static const radio_station_t *list_at(int tab, int i) {
    if (i < 0 || i >= list_len(tab))
        return NULL;
    return tab == TAB_FAVS ? &U.favs[i] : tab == TAB_STATIONS ? &starter[i] : &U.found[i];
}

/// A station's place in the favourites.
/// @param url its address
/// @return the index, or -1
static int fav_index(const char *url) {
    for (int i = 0; i < U.nfavs; ++i)
        if (!strcmp(U.favs[i].url, url))
            return i;
    return -1;
}

/// Sets the status line's message (5 seconds).
/// @param fmt printf's format, one %s
/// @param arg its argument
static void message(const char *fmt, const char *arg) {
    snprintf(U.msg, sizeof U.msg, fmt, arg);
    U.msg_until = to_ms_since_boot(get_absolute_time()) + 5000;
}

/// name|url|bitrate|country per favourite; last=url|name; volume=n
static void save_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    char line[320];
    UINT put;
    snprintf(line, sizeof line, "; Pico Radio: favourites (name|url|kbit/s|country), the last station, the volume\r\n");
    f_write(&f, line, strlen(line), &put);
    for (int i = 0; i < U.nfavs; ++i) {
        const radio_station_t *s = &U.favs[i];
        snprintf(line, sizeof line, "fav=%s|%s|%u|%s|%s\r\n", s->name, s->url, s->bitrate, s->country, s->codec);
        f_write(&f, line, strlen(line), &put);
    }
    if (U.last_url[0]) {
        snprintf(line, sizeof line, "last=%s|%s\r\n", U.last_url, U.last_name);
        f_write(&f, line, strlen(line), &put);
    }
    snprintf(line, sizeof line, "volume=%d\r\n", radio_volume());
    f_write(&f, line, strlen(line), &put);
    f_close(&f);
}

/// Takes a field of a
/// line.
/// @param p the text (moved past it)
/// @param out the field
/// @param size room in out
static void field(char **p, char *out, size_t size) {
    size_t n = strcspn(*p, "|\r\n");
    snprintf(out, size, "%.*s", (int)n, *p);
    *p += n;
    if (**p == '|')
        ++*p;
}

/// Loads /RADIO.INI: favourites, the last station and the volume.
static void load_ini(void) {
    FIL f;
    if (f_open(&f, INI_PATH, FA_READ) != FR_OK)
        return;
    char line[320];
    while (f_gets(line, sizeof line, &f)) {
        char *p = line;
        if (!strncmp(p, "fav=", 4) && U.nfavs < MAX_FAVS) {
            p += 4;
            radio_station_t *s = &U.favs[U.nfavs];
            memset(s, 0, sizeof *s);
            char num[12];
            field(&p, s->name, sizeof s->name);
            field(&p, s->url, sizeof s->url);
            field(&p, num, sizeof num);
            s->bitrate = (uint16_t)atoi(num);
            field(&p, s->country, sizeof s->country);
            field(&p, s->codec, sizeof s->codec);
            if (!s->codec[0])  // (from before AAC)
                strcpy(s->codec, "MP3");
            if (s->url[0])
                ++U.nfavs;
        } else if (!strncmp(p, "last=", 5)) {
            p += 5;
            field(&p, U.last_url, sizeof U.last_url);
            field(&p, U.last_name, sizeof U.last_name);
        } else if (!strncmp(p, "volume=", 7)) {
            radio_set_volume(atoi(p + 7));
        }
    }
    f_close(&f);
}

/// Adds or removes a favourite.
/// @param s the station
static void toggle_fav(const radio_station_t *s) {
    if (!s)
        return;
    int i = fav_index(s->url);
    if (i >= 0) {
        memmove(&U.favs[i], &U.favs[i + 1], (U.nfavs - i - 1) * sizeof *U.favs);
        --U.nfavs;
        message("Removed from favourites: %s", s->name);
    } else if (U.nfavs < MAX_FAVS) {
        U.favs[U.nfavs++] = *s;
        message("Added to favourites: %s", s->name);
    }
    save_ini();
}

// ---- Drawing the screen ----------------------------------------------------------------------------------

/// The tabs' names.
static const char *const tab_names[NTABS] = {"Favourites", "Stations", "Search"};

/// Draws the title strip and the tabs.
static void draw_top(void) {
    fill(0, 0, W, TOP_H, C_STRIP);
    text(8, 4, "Pico Radio", 10, C_YELLOW, C_STRIP, false);
    text(100, 4, "internet radio, MP3 and AAC streams", 36, C_LABEL, C_STRIP, false);
    fill(0, TOP_H, W, LIST_Y - TOP_H, C_BG);
    int x = 4;
    for (int t = 0; t < NTABS; ++t) {
        char label[24];
        snprintf(label, sizeof label, t == TAB_FOUND && U.nfound ? "%s (%d)" : "%s", tab_names[t],
                 t == TAB_FAVS ? U.nfavs : U.nfound);
        if (t == TAB_FAVS)
            snprintf(label, sizeof label, "Favourites (%d)", U.nfavs);
        int w = (int)strlen(label) * 8 + 20;
        button(x, TABS_Y, w, 24, label, U.tab == t);
        hit(x, TABS_Y, w, 24, HIT_TAB, t);
        x += w + 4;
    }
    button(x, TABS_Y, BTN_W(7), 24, "Find...", false);
    hit(x, TABS_Y, BTN_W(7), 24, HIT_SEARCH, 0);
    x += BTN_W(7) + 4;
    button(x, TABS_Y, BTN_W(6), 24, "URL...", false);
    hit(x, TABS_Y, BTN_W(6), 24, HIT_URL, 0);
    button(W - 4 - BTN_W(4), TABS_Y, BTN_W(4), 24, "Quit", false);
    hit(W - 4 - BTN_W(4), TABS_Y, BTN_W(4), 24, HIT_QUIT, 0);
}

/// Draws the station list.
/// @param st the status (for the playing mark)
static void draw_list(const radio_status_t *st) {
    bevel(LIST_X, LIST_Y, LIST_W, LIST_H, false, C_FIELD);
    int n = list_len(U.tab), *sel = &U.sel[U.tab], *top = &U.top[U.tab];
    if (*sel >= n)
        *sel = n ? n - 1 : 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + ROWS)
        *top = *sel - ROWS + 1;
    for (int r = 0; r < ROWS; ++r) {
        int i = *top + r, y = LIST_Y + 4 + r * ROW_H;
        const radio_station_t *s = list_at(U.tab, i);
        bool on = i == *sel && s;
        bool playing = s && st->state != RADIO_OFF && !strcmp(s->url, st->url);
        uint8_t bg = on ? C_SEL : (i & 1) ? C_ROWALT : C_FIELD;
        fill(LIST_X + 3, y, LIST_W - 6, ROW_H, bg);
        if (!s)
            continue;
        bool fav = fav_index(s->url) >= 0;
        ch8(LIST_X + 6, y + 2, fav ? '*' : 0xfa, fav ? C_YELLOW : C_DIM, bg);
        hit(LIST_X + 3, y, 18, ROW_H, HIT_STAR, i);
        ch8(LIST_X + 18, y + 2, playing ? 0x10 : ' ', C_GREEN, bg);
        text(LIST_X + 30, y + 2, s->name, 33, playing ? C_GREEN : on ? C_TEXT : C_LABEL, bg, false);
        char right[24], br[4] = "";
        if (s->bitrate)
            snprintf(br, sizeof br, "%3u", s->bitrate);
        snprintf(right, sizeof right, "%3s %-4.4s %s", br, s->codec, s->country);
        text(LIST_X + LIST_W - 8 - 11 * 8, y + 2, right, 11, C_DIM, bg, false);
        hit(LIST_X + 21, y, LIST_W - 24, ROW_H, HIT_ROW, i);
    }
    if (!n) {
        const char *empty = U.tab == TAB_FAVS ? "No favourites yet: F (or the star) adds one."
                                              : "Press / (or Find...) to search radio-browser.info.";
        text(LIST_X + 12, LIST_Y + 12, empty, 46, C_DIM, C_FIELD, false);
    }
}

/// Word-wrapped text in `lines` lines of `cols` characters.
/// @param x left
/// @param y top
/// @param s the text
/// @param cols columns
/// @param lines lines
/// @param fg its colour
/// @param bg the background
static void wrapped(int x, int y, const char *s, int cols, int lines, uint8_t fg, uint8_t bg) {
    for (int l = 0; l < lines; ++l) {
        int n = (int)strlen(s);
        if (n > cols) {
            n = cols;
            for (int k = cols; k > cols / 2; --k)
                if (s[k] == ' ') {
                    n = k;
                    break;
                }
        }
        text(x, y + l * 16, s, n, fg, bg, true);
        s += n;
        while (*s == ' ')
            ++s;
    }
}

/// The parts that change all the time: the buffer, the meters, the volume.
/// @param st the status
/// @param quick just the values (else the labels and frames too)
static void draw_live(const radio_status_t *st, bool quick) {
    int x = PANEL_X, y = LIST_Y, cols = (PANEL_W - 16) / 8;
    char line[48];
    int bw = PANEL_W - 76, fillw = st->buffer_pct * (bw - 2) / 100, mw = PANEL_W - 40;
    if (!quick) {
        fill(x + 4, y + 162, PANEL_W - 8, 130, C_FACE);
        text(x + 8, y + 166, "Buffer", 6, C_LABEL, C_FACE, false);
        bevel(x + 64, y + 166, bw, 14, false, C_FIELD);
        for (int c = 0; c < 2; ++c) {
            text(x + 8, y + 210 + c * 18, c ? "R" : "L", 1, C_LABEL, C_FACE, false);
            bevel(x + 24, y + 210 + c * 18, mw, 14, false, C_BG);
        }
        text(x + 8, y + 254, "Volume", 6, C_LABEL, C_FACE, false);
        int vx = x + 64, vw = VOL_W;
        bevel(vx, y + 256, vw, 10, false, C_FIELD);
        int knob = vx + radio_volume() * (vw - 12) / 100;
        fill(vx + 1, y + 257, knob - vx, 8, C_SEL);
        bevel(knob, y + 252, 12, 18, true, C_LABEL);
        snprintf(line, sizeof line, radio_muted() ? "%3d%%  MUTED" : "%3d%%       ", radio_volume());
        text(x + 8, y + 272, line, 11, radio_muted() ? C_RED : C_TEXT, C_FACE, false);
        button(vx + vw + 8, y + 250, MUTE_W, 22, radio_muted() ? "Sound" : "Mute", radio_muted());
    }
    // The buffer's fill and figures, the meters' segments: each pixel once.
    fill(x + 65, y + 167, fillw, 12, st->buffer_ms < 2000 ? C_ORANGE : C_GREEN);
    fill(x + 65 + fillw, y + 167, bw - 2 - fillw, 12, C_FIELD);
    snprintf(line, sizeof line, "%d.%d s  CPU %u.%u%%  drops %lu", st->buffer_ms / 1000,
             st->buffer_ms % 1000 / 100, st->load / 10, st->load % 10, (unsigned long)st->underruns);
    text(x + 8, y + 184, line, cols, C_DIM, C_FACE, true);
    for (int c = 0; c < 2; ++c) {
        int my = y + 210 + c * 18, len = st->level[c] * (mw - 2) / 64;
        for (int k = 0; k < mw - 2; k += 4) {
            uint8_t col = k >= len ? C_DARK : k < (mw - 2) * 6 / 10 ? C_GREEN : k < (mw - 2) * 85 / 100 ? C_YELLOW : C_RED;
            fill(x + 25 + k, my + 1, 3, 12, col);
        }
    }
}

/// Draws the playing panel.
/// @param st the status
static void draw_panel(const radio_status_t *st) {
    int x = PANEL_X, y = LIST_Y;
    bevel(x, y, PANEL_W, LIST_H, true, C_FACE);
    int cols = (PANEL_W - 16) / 8;
    static const char *const states[] = {"Stopped", "Connecting...", "Buffering...", "Playing", "Ended",
                                         "Can't play"};
    uint8_t sc = st->state == RADIO_PLAYING ? C_GREEN : st->state == RADIO_ERROR ? C_RED : C_YELLOW;
    text(x + 8, y + 8, states[st->state], cols, sc, C_FACE, true);
    wrapped(x + 8, y + 30, st->state == RADIO_OFF ? "" : st->station, cols, 2, C_TEXT, C_FACE);
    fill(x + 8, y + 66, PANEL_W - 16, 1, C_DARK);
    const char *song = st->state == RADIO_ERROR || st->state == RADIO_ENDED ? st->error : st->title;
    wrapped(x + 8, y + 72, song, cols, 4, st->state == RADIO_ERROR ? C_RED : C_CYAN, C_FACE);
    char line[40];
    if (st->samplerate)
        snprintf(line, sizeof line, "%s %d kbit/s %d.%d kHz %s", st->codec, st->bitrate, st->samplerate / 1000,
                 st->samplerate % 1000 / 100, st->channels == 1 ? "mono" : "");
    else
        line[0] = 0;
    text(x + 8, y + 144, line, cols, C_LABEL, C_FACE, true);
    draw_live(st, false);
    hit(x + 64 - 6, y + 248, VOL_W + 12, 26, HIT_VOLUME, 0);
    hit(x + 64 + VOL_W + 8, y + 250, MUTE_W, 22, HIT_MUTE, 0);
    // Where it comes from
    fill(x + 8, y + 296, PANEL_W - 16, 1, C_DARK);
    if (st->state != RADIO_OFF) {
        const char *u = st->url;
        if (!strncmp(u, "http://", 7))
            u += 7;
        else if (!strncmp(u, "https://", 8))
            u += 8;
        wrapped(x + 8, y + 302, u, cols, 3, C_DIM, C_FACE);
    } else {
        wrapped(x + 8, y + 302, "", cols, 3, C_DIM, C_FACE);
    }
    // Buttons
    int by = y + LIST_H - 34;
    bool on = radio_active();
    button(x + 8, by, BTN_W(4), 26, on ? "Stop" : "Play", on);
    hit(x + 8, by, BTN_W(4), 26, on ? HIT_STOP : HIT_PLAY, 0);
    const radio_station_t *s = list_at(U.tab, U.sel[U.tab]);
    bool fav = s && fav_index(s->url) >= 0;
    int fx = x + 8 + BTN_W(4) + 6;
    button(fx, by, BTN_W(13), 26, fav ? "* Favourite" : "Add favourite", fav);
    hit(fx, by, BTN_W(13), 26, HIT_FAV, 0);
}

/// Draws the status line.
static void draw_status(void) {
    fill(0, STATUS_Y - 4, W, H - STATUS_Y + 4, C_FACE);
    fill(0, STATUS_Y - 4, W, 1, C_LIGHT);
    const char *s = U.msg[0] && to_ms_since_boot(get_absolute_time()) < U.msg_until
                        ? U.msg
                        : "Enter play  S stop  M mute  F favourite  / find  U url  +/- vol  Tab  Esc";
    text(8, STATUS_Y + 6, s, 78, U.msg[0] && s == U.msg ? C_YELLOW : C_LABEL, C_FACE, true);
}

/// The live parts: through the picture when the volume has moved, else
/// the meters straight onto the screen.
/// @param st the status
static void redraw_live(const radio_status_t *st) {
    static int drawn_volume = -1;
    int vol_now = radio_volume() + (radio_muted() ? 1000 : 0);
    if (vol_now != drawn_volume) {  // the slider: through the picture
        drawn_volume = vol_now;
        draw_live(st, false);
        show(PANEL_X, LIST_Y + 162, PANEL_W, 130);
        return;
    }
    dst = dvi_framebuf;
    draw_live(st, true);
    dst = U.g;
}

/// Draws parts of the screen and shows them (the station list brings the
/// playing panel with it).
/// @param list the station list too
/// @param panel the playing panel
/// @param status the status line
/// @param st the status
static void redraw(bool list, bool panel, bool status, const radio_status_t *st) {
    if (list) {
        nhits = 0;
        draw_top();
        draw_list(st);
        show(0, 0, W, LIST_Y + LIST_H);
        // (the panel's places are added again below)
        panel = true;
    }
    if (panel) {
        // Its places go after the list's: drop the old ones first.
        for (int i = 0; i < nhits;)
            if (hits[i].id == HIT_VOLUME || hits[i].id == HIT_MUTE || hits[i].id == HIT_STOP || hits[i].id == HIT_PLAY ||
                hits[i].id == HIT_FAV)
                hits[i] = hits[--nhits];
            else
                ++i;
        draw_panel(st);
        show(PANEL_X, LIST_Y, PANEL_W, LIST_H);
    }
    if (status) {
        draw_status();
        show(0, STATUS_Y - 4, W, H - STATUS_Y + 4);
    }
}

// ---- A line of input ------------------------------------------------------------------------------------

/// A line of input in a box.
/// @param title its title
/// @param prompt the prompt
/// @param buf the text
/// @param size room in buf
/// @return Enter (else Esc)
static bool ask(const char *title, const char *prompt, char *buf, int size) {
    int w = 520, h = 112, x = (W - w) / 2, y = 150, pos = strlen(buf);
    // Kept to put back afterwards.
    uint8_t *under = psram_malloc((size_t)(w + 6) * (h + 6));
    for (int r = 0; under && r < h + 6; ++r)
        memcpy(under + r * (w + 6), U.g + (y + r) * W + x, w + 6);
    bool ok = false;
    for (;;) {
        fill(x + 6, y + 6, w, h, C_BG);
        bevel(x, y, w, h, true, C_FACE);
        fill(x + 2, y + 2, w - 4, 18, C_STRIP);
        text(x + (w - (int)strlen(title) * 8) / 2, y + 3, title, 60, C_YELLOW, C_STRIP, false);
        text(x + 16, y + 30, prompt, 60, C_LABEL, C_FACE, false);
        bevel(x + 16, y + 52, w - 32, 20, false, C_FIELD);
        int cols = (w - 40) / 8, off = pos > cols - 1 ? pos - cols + 1 : 0, len = strlen(buf);
        for (int i = 0; i < cols; ++i) {
            int k = off + i;
            ch8(x + 20 + i * 8, y + 54, k < len ? (uint8_t)buf[k] : ' ', k == pos ? C_BG : C_TEXT,
                k == pos ? 0xfe : C_FIELD);
        }
        text(x + 16, y + 84, "Enter: OK   Esc: cancel", 30, C_DIM, C_FACE, false);
        show(x, y, w + 6, h + 6);
        int k;
        while ((k = key_poll()) == KEY_NONE) {
            mouse_event_t e;
            while (mouse_event(&e))
                ;
            mouse_pointer_update();
            sleep_ms(5);
        }
        if (k == '\r') {
            ok = true;
            break;
        }
        if (k == KEY_ESC)
            break;
        if (k == '\b' && pos > 0) {
            memmove(buf + pos - 1, buf + pos, len - pos + 1);
            --pos;
        } else if (k == KEY_LEFT && pos > 0) {
            --pos;
        } else if (k == KEY_RIGHT && pos < len) {
            ++pos;
        } else if (k == KEY_HOME) {
            pos = 0;
        } else if (k == KEY_END) {
            pos = len;
        } else if (k == KEY_DEL && pos < len) {
            memmove(buf + pos, buf + pos + 1, len - pos);
        } else if (k >= 0x20 && k < 0x7f && len < size - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char)k;
        }
    }
    for (int r = 0; under && r < h + 6; ++r)
        memcpy(U.g + (y + r) * W + x, under + r * (w + 6), w + 6);
    psram_free(under);
    show(x, y, w + 6, h + 6);
    return ok;
}

// ---- Doing things --------------------------------------------------------------------------------------------

/// A message on the status line, at once (before something slow).
/// @param text_now the message
static void busy_message(const char *text_now) {
    snprintf(U.msg, sizeof U.msg, "%s", text_now);
    U.msg_until = to_ms_since_boot(get_absolute_time()) + 60000;
    draw_status();
    show(0, STATUS_Y - 4, W, H - STATUS_Y + 4);
}

/// Plays a station (the panel shows connecting).
/// @param s the station
static void play(const radio_station_t *s) {
    if (!s)
        return;
    char m[96];
    snprintf(m, sizeof m, "Connecting to %s... (Esc gives up)", s->name);
    busy_message(m);
    if (radio_play(s->url, s->name)) {
        snprintf(U.last_url, sizeof U.last_url, "%s", s->url);
        snprintf(U.last_name, sizeof U.last_name, "%s", s->name);
        save_ini();
        message("Buffering %s...", s->name);
    } else {
        radio_status_t st;
        radio_get_status(&st);
        message("Can't play it: %s", st.error);
    }
}

static void search(void) {
    if (!ask("Find stations", "A name, or tag:jazz, country:DE, lang:french", U.search, sizeof U.search) ||
        !U.search[0])
        return;
    char m[96];
    snprintf(m, sizeof m, "Searching radio-browser.info for \"%s\"...", U.search);
    busy_message(m);
    const char *err = "";
    int n = radio_search(U.search, U.found, MAX_FOUND, &err);
    if (n <= 0) {
        message("Search: %s", err);
        if (n < 0)
            return;
    }
    U.nfound = n;
    U.tab = TAB_FOUND;
    U.sel[TAB_FOUND] = U.top[TAB_FOUND] = 0;
    if (n > 0) {
        char c[16];
        snprintf(c, sizeof c, "%d", n);
        message("%s stations found", c);
    }
}

/// URL...: plays an address typed in.
static void play_url(void) {
    static char url[200] = "http://";
    if (!ask("Play an address", "An http:// or https:// stream, .pls or .m3u", url, sizeof url))
        return;
    radio_station_t s = {0};
    snprintf(s.url, sizeof s.url, "%s", url);
    snprintf(s.name, sizeof s.name, "%s", url);
    play(&s);
}

/// Sets the volume.
/// @param v 0-100
static void volume_to(int v) {
    radio_set_volume(v);
}

// ---- The loop -------------------------------------------------------------------------------------------------

/// Handles a key.
/// @param k the key
/// @param list_dirty set if the list must be drawn again
static void key(int k, bool *list_dirty) {
    int n = list_len(U.tab), *sel = &U.sel[U.tab];
    const radio_station_t *s = list_at(U.tab, *sel);
    switch (k) {
    case KEY_UP: if (*sel > 0) --*sel; break;
    case KEY_DOWN: if (*sel < n - 1) ++*sel; break;
    case KEY_PGUP: *sel = *sel > ROWS ? *sel - ROWS : 0; break;
    case KEY_PGDN: *sel = *sel + ROWS < n ? *sel + ROWS : n - 1; break;
    case KEY_HOME: *sel = 0; break;
    case KEY_END: *sel = n - 1; break;
    case '\t': U.tab = (U.tab + 1) % NTABS; break;
    case '\r': play(s); break;
    case 'f': case 'F': toggle_fav(s); break;
    case KEY_DEL: if (U.tab == TAB_FAVS) toggle_fav(s); break;
    case 's': case 'S': radio_stop(); message("Stopped%s", ""); break;
    case 'm': case 'M':
        radio_set_mute(!radio_muted());
        message(radio_muted() ? "Muted (M again for sound)%s" : "Sound on%s", "");
        return;
    case '/': case '?': search(); break;
    case 'u': case 'U': play_url(); break;
    case '+': case '=': case KEY_RIGHT: volume_to(radio_volume() + 5); save_ini(); return;
    case '-': case '_': case KEY_LEFT: volume_to(radio_volume() - 5); save_ini(); return;
    case KEY_ESC: case 'q': case 'Q': case CTRL('c'): U.quit = true; return;
    default: return;
    }
    if (*sel < 0)
        *sel = 0;
    *list_dirty = true;
}

/// Handles a mouse event.
/// @param e the event
/// @param list_dirty set if the list must be drawn again
static void click(const mouse_event_t *e, bool *list_dirty) {
    if (e->type == MOUSE_WHEEL) {
        int n = list_len(U.tab), *sel = &U.sel[U.tab];
        *sel -= e->wheel * 3;
        *sel = *sel < 0 ? 0 : *sel >= n ? n - 1 : *sel;
        *list_dirty = true;
        return;
    }
    if (e->type == MOUSE_UP) {
        if (U.dragging_volume)
            save_ini();
        U.dragging_volume = false;
        return;
    }
    if (e->type == MOUSE_MOVE) {
        if (U.dragging_volume && (e->buttons & MOUSE_LEFT)) {
            int vx = PANEL_X + 64, vw = VOL_W;
            volume_to((e->x - vx - 6) * 100 / (vw - 12));
        }
        return;
    }
    const hit_t *h = hit_at(e->x, e->y);
    if (!h)
        return;
    switch (h->id) {
    case HIT_TAB: U.tab = h->arg; break;
    case HIT_ROW:
        U.sel[U.tab] = h->arg;
        if (e->double_click || e->button == MOUSE_RIGHT)
            play(list_at(U.tab, h->arg));
        break;
    case HIT_STAR: toggle_fav(list_at(U.tab, h->arg)); break;
    case HIT_STOP: radio_stop(); break;
    case HIT_MUTE: radio_set_mute(!radio_muted()); return;
    case HIT_PLAY: play(list_at(U.tab, U.sel[U.tab])); break;
    case HIT_FAV: toggle_fav(list_at(U.tab, U.sel[U.tab])); break;
    case HIT_SEARCH: search(); break;
    case HIT_URL: play_url(); break;
    case HIT_QUIT: U.quit = true; return;
    case HIT_VOLUME: {
        U.dragging_volume = true;
        int vx = PANEL_X + 64, vw = VOL_W;
        volume_to((e->x - vx - 6) * 100 / (vw - 12));
        return;
    }
    }
    *list_dirty = true;
}

/// The station browser.
static void browser(void) {
    memset(&U, 0, sizeof U);
    U.g = psram_malloc(W * H);
    dst = U.g;
    U.favs = psram_malloc(MAX_FAVS * sizeof *U.favs);
    U.found = psram_malloc(MAX_FOUND * sizeof *U.found);
    hits = psram_malloc(64 * sizeof *hits);
    nhits = 0;
    if (!U.g || !U.favs || !U.found || !hits) {
        psram_free(hits);
        hits = NULL;
        printf("radio: not enough memory\n");
        psram_free(U.g);
        psram_free(U.favs);
        psram_free(U.found);
        return;
    }
    load_ini();
    U.tab = U.nfavs ? TAB_FAVS : TAB_STATIONS;
    // The screen is ours: printing (from connecting) would draw over it.
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);  // no cursor blinking into the picture
    memset(U.g, C_BG, W * H);
    mouse_pointer(MOUSE_POINTER_ARROW);
    radio_status_t st;
    radio_get_status(&st);
    redraw(true, true, true, &st);
    show(0, 0, W, H);  // (and the gaps between the panels)
    uint32_t next_panel = 0;
    int last_state = -1;
    char last_title[128] = "";
    while (!U.quit) {
        bool list_dirty = false;
        int k;
        while (!U.quit && (k = key_poll()) != KEY_NONE)
            key(k, &list_dirty);
        mouse_event_t e;
        while (!U.quit && mouse_event(&e))
            click(&e, &list_dirty);
        mouse_pointer_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        radio_get_status(&st);
        bool changed = st.state != last_state || strcmp(st.title, last_title);
        if (list_dirty || changed) {
            redraw(true, true, true, &st);
            last_state = st.state;
            snprintf(last_title, sizeof last_title, "%s", st.title);
            next_panel = now + 100;
        } else if ((int32_t)(now - next_panel) >= 0) {
            // The meters and buffer every 100 ms; the status line when its
            // message comes or goes.
            redraw_live(&st);
            bool msg_on = U.msg[0] && now < U.msg_until;
            static bool was_on;
            static char was_msg[96];
            if (msg_on != was_on || strcmp(was_msg, U.msg)) {
                redraw(false, false, true, &st);
                was_on = msg_on;
                snprintf(was_msg, sizeof was_msg, "%s", U.msg);
            }
            next_panel = now + 100;
        }
        sleep_ms(10);
    }
    mouse_pointer(MOUSE_POINTER_OFF);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
    psram_free(U.g);
    psram_free(U.favs);
    psram_free(U.found);
    psram_free(hits);
    hits = NULL;
    U.g = NULL;
    if (radio_active())
        printf("The radio plays on: `radio stop` stops it, `radio` opens the station list.\n");
}

// ---- The command ------------------------------------------------------------------------------------------

/// Prints what is playing.
static void print_status(void) {
    radio_status_t st;
    radio_get_status(&st);
    static const char *const states[] = {"off", "connecting", "buffering", "playing", "ended", "error"};
    printf("radio: %s", states[st.state]);
    if (st.state != RADIO_OFF)
        printf(" - %s", st.station);
    printf("\n");
    if (st.title[0] && st.state != RADIO_OFF)
        printf("  %s\n", st.title);
    if (st.samplerate && st.state != RADIO_OFF)
        printf("  %s %d kbit/s, %d Hz, %s; buffer %d.%d s; decoding %u.%u%% of core 1 (%d of 24 KB in SRAM); dropouts %lu\n",
               st.codec, st.bitrate, st.samplerate, st.channels == 1 ? "mono" : "stereo", st.buffer_ms / 1000,
               st.buffer_ms % 1000 / 100, st.load / 10, st.load % 10,
               st.decoder_sram_kb, (unsigned long)st.underruns);
    if (st.error[0] && (st.state == RADIO_ERROR || st.state == RADIO_ENDED))
        printf("  %s\n", st.error);
    printf("  volume %d%%%s\n", radio_volume(), radio_muted() ? ", muted" : "");
}

void radio_command(int argc, char **argv) {
    if (argc < 2) {
        browser();
        return;
    }
    if (!strcmp(argv[1], "stop")) {
        radio_stop();
    } else if (!strcmp(argv[1], "status")) {
        print_status();
    } else if (!strcmp(argv[1], "mute")) {
        radio_set_mute(argc > 2 ? strcmp(argv[2], "off") != 0 : !radio_muted());
        printf("radio: %s\n", radio_muted() ? "muted" : "sound on");
    } else if (!strcmp(argv[1], "volume") && argc > 2) {
        radio_set_volume(atoi(argv[2]));
    } else if (!strcmp(argv[1], "play") && argc > 2) {
        if (radio_play(argv[2], argc > 3 ? argv[3] : NULL))
            printf("Playing (buffering first). `radio status` tells more.\n");
        else
            print_status();
    } else if (!strcmp(argv[1], "last")) {
        memset(&U, 0, sizeof U);
        U.favs = psram_malloc(MAX_FAVS * sizeof *U.favs);
        if (U.favs)
            load_ini();
        psram_free(U.favs);
        U.favs = NULL;
        if (!U.last_url[0])
            printf("radio: no last station yet\n");
        else if (radio_play(U.last_url, U.last_name))
            printf("Playing %s\n", U.last_name);
        else
            print_status();
    } else {
        printf("usage: radio [stop | status | last | play URL | volume 0-100 | mute [on|off]]\n");
    }
}
