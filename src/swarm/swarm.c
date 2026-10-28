/**
 * @file swarm.c
 * @brief Swarm, an arcade space shooter (see swarm.h): the game, its drawing, and the command.
 *
 * The screen: while the game runs the scanout doubles pixels both ways, so
 * the picture is 320x240 (dvi_framebuf's first quarter). Each frame is
 * drawn in a second 320x240 buffer straight after it, also in SRAM, and
 * copied over when the vertical sync starts (dvi_frames), so nothing
 * flickers or tears. The rest of the framebuffer holds everything else the
 * game needs (world_t: the turned sprites, the aliens, the sound's state),
 * so the game keeps no SRAM of its own once it ends.
 *
 * The play area is the left 232 columns; the panel to its right shows the
 * scores, the fighters in reserve and the stage badges.
 *
 * How it plays: each stage, 40 aliens fly in along curved paths in five
 * waves of eight and take their places in a formation (4 flagships, 16
 * moths, 20 drones), which sways while they arrive and then breathes in and
 * out. Then they dive: a half loop out of the formation, then down at the
 * fighter, firing, and back in from the top. A flagship takes two hits, may
 * bring two moths along as escorts, or may stop low down and send out a
 * tractor beam: a fighter caught in it is carried off and sits on top of the
 * flagship. Shooting that flagship while it dives frees the fighter, which
 * docks beside the player's: a twin fighter with twice the fire (and twice
 * the size). Every fourth stage from the third is a challenge stage: 40
 * aliens fly patterns without firing, for a bonus for each one hit.
 *
 * Paths are lists of segments (frames, turn each frame, speed) followed
 * from a start point and heading, and mirrored for the other side. Headings
 * are 0-256 a turn, 0 up and 64 right.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "swarm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "spr.h"
#include "swarm_art.h"
#include "swarm_snd.h"
#include "tusb.h"
#include "usb_keyboard.h"

// ---- Sizes and places ---------------------------------------------------------------------------------

#define SW 320          ///< the picture's width
#define SH 240          ///< and height
#define PF_W 232        ///< the play area's width (the panel is to its right)
#define PANEL_X 240     ///< where the panel's text starts
#define PLAYER_Y 222    ///< the fighter's centre row
#define FORM_TOP 34     ///< the formation's top row (flagships)
#define FORM_CX 116     ///< the formation's centre column
#define BEAM_Y 132      ///< where a flagship stops to send its beam
#define SPR 16          ///< sprites are SPR x SPR
#define ANGLES 16       ///< turned copies of each sprite

#define MAX_ENEMIES 48  ///< 40 aliens, a captured fighter, and room
#define MAX_PSHOTS 4    ///< the player's shots on the screen at once
#define MAX_ESHOTS 24   ///< the aliens' shots
#define MAX_BOOMS 24    ///< explosions and score pop-ups
#define STARS 80        ///< the starfield

#define HI_FILE "/SWARM.HI"    ///< the high scores
#define SPR_FILE "/SWARM.SPR"  ///< sprites used in place of the built-in ones

/// The picture being shown, and the one being drawn.
///@{
#define FRONT (dvi_framebuf)
#define BACK (dvi_framebuf + SW * SH)
///@}

/// RGB332 colours used in the drawing.
enum {
    C_BLACK = 0,
    C_WHITE = RGB332(7, 7, 3),
    C_RED = RGB332(7, 0, 0),
    C_YELLOW = RGB332(7, 7, 0),
    C_CYAN = RGB332(0, 7, 3),
    C_BLUE = RGB332(1, 2, 3),
    C_DKBLUE = RGB332(0, 1, 2),
    C_GREEN = RGB332(0, 6, 0),
    C_ORANGE = RGB332(7, 4, 0),
    C_PURPLE = RGB332(5, 1, 3),
    C_GREY = RGB332(4, 4, 2),
    C_PINK = RGB332(7, 3, 2),
};

// ---- Pictures ---------------------------------------------------------------------------------------

/// The pictures that can be drawn turned (an image is ANGLES copies of SPRxSPR).
enum {
    IMG_SHIP,
    IMG_CAPTIVE,                 ///< the fighter, red, once captured
    IMG_DRONE, IMG_DRONE_B,
    IMG_MOTH, IMG_MOTH_B,
    IMG_BOSS, IMG_BOSS_B,
    IMG_BOSS_HIT, IMG_BOSS_HIT_B,  ///< a flagship hit once
    IMG_DART, IMG_DART_B,
    IMG_DISC, IMG_DISC_B,
    IMG_COUNT
};

// ---- The world ----------------------------------------------------------------------------------------

/// Kinds of alien.
enum { K_DRONE, K_MOTH, K_BOSS, K_CAPTIVE, K_RENEGADE, K_DART, K_DISC };

/// What an alien is doing.
enum {
    E_NONE,     ///< nothing: a free entry
    E_WAIT,     ///< waiting to fly in (timer counts down)
    E_PATH,     ///< flying in (or past, on a challenge stage) along its path
    E_HOME,     ///< flying to its place in the formation
    E_FORM,     ///< in the formation
    E_DIVE,     ///< the half loop out of the formation
    E_ATTACK,   ///< diving at the fighter
    E_ESCORT,   ///< a moth flying beside its flagship
    E_BEAM,     ///< a flagship stopped with its tractor beam out
    E_RETURN,   ///< back in from the top after a dive
    E_CAPTIVE,  ///< a captured fighter riding on its flagship
    E_RESCUE,   ///< a freed fighter on its way to dock
};

/// The player's fighter.
enum {
    P_ALIVE,     ///< flying
    P_DYING,     ///< blowing up
    P_WAIT,      ///< gone; the next one comes when the aliens are back in formation
    P_READY,     ///< "READY": the next fighter about to start
    P_CAPTURED,  ///< being pulled up the beam
    P_GONE,      ///< carried off by the flagship
    P_FREE,      ///< let go by the beam, sinking back
    P_OVER,      ///< none left
};

/// What the game is showing.
enum { M_TITLE, M_PLAY, M_GAMEOVER, M_RESULTS, M_ENTRY };

/// The parts of a stage.
enum { PH_INTRO, PH_WAVES, PH_CLEAR, PH_BONUS };

/// One stretch of a path: for `frames` frames turn by `turn` each frame
/// (mirrored on the other side), at `speed` (0: as before).
typedef struct {
    int16_t frames;  ///< how long (0 ends the path)
    float turn;      ///< heading change a frame (256 a turn)
    float speed;     ///< pixels a frame (0: unchanged)
} seg_t;

/// A path: where it starts, which way, and its stretches.
typedef struct {
    float x, y;       ///< the start (for the left-hand version)
    float ang;        ///< the starting heading
    seg_t seg[6];     ///< the stretches, ended by frames 0
} path_t;

/// An alien (or a captured fighter).
typedef struct {
    float x, y;        ///< centre
    float ang;         ///< heading (0-256, 0 up)
    float speed;       ///< pixels a frame
    float tx;          ///< where a dive aims across
    float ox, oy;      ///< an escort's place beside its flagship
    const path_t *path;  ///< the path it is on
    int16_t seg_left;  ///< frames left of the stretch
    int16_t timer;     ///< frames in the state (or before appearing, in E_WAIT)
    uint8_t state;     ///< E_ value
    uint8_t kind;      ///< K_ value
    uint8_t slot;      ///< its place in the formation (0-39)
    uint8_t seg;       ///< the stretch it is on
    int8_t mirror;     ///< 1, or -1 for the path's other side
    uint8_t hits;      ///< hits still needed
    int8_t link;       ///< a flagship's captive, a captive's or escort's flagship (-1: none)
    uint8_t kills;     ///< a diving flagship's escorts shot
    uint8_t shots;     ///< shots it may still fire on this dive
    bool beam;         ///< a flagship's dive is a tractor-beam one
    bool flyby;        ///< a challenge-stage alien (flies off, no formation)
    uint8_t wobble;    ///< a moth zigzags on its dive
} enemy_t;

/// A shot.
typedef struct {
    float x, y, dx, dy;  ///< where and how fast
    bool on;             ///< in flight
} shot_t;

/// An explosion, or points shown where an alien was hit.
typedef struct {
    float x, y;        ///< where
    int16_t t;         ///< frames so far (-1: free)
    uint8_t big;       ///< 0 an alien's, 1 the fighter's, 2 points
    uint16_t points;   ///< points shown
} boom_t;

/// A star.
typedef struct {
    float y, speed;   ///< row and how fast it falls
    uint8_t x;        ///< column
    uint8_t colour;   ///< RGB332
    uint8_t phase;    ///< when it twinkles
} star_t;

/// A high score.
typedef struct {
    uint32_t score;  ///< the score
    char name[4];    ///< three letters
} hiscore_t;

/// Everything (placed in the framebuffer's free part while the game runs).
typedef struct {
    uint8_t img[IMG_COUNT][ANGLES][SPR * SPR];  ///< the pictures, turned (0: see-through)
    uint8_t font[sizeof font_chars - 1][7];     ///< the font (copied from flash)
    float sin_t[256];                           ///< sine of a heading
    enemy_t e[MAX_ENEMIES];                     ///< the aliens
    shot_t ps[MAX_PSHOTS];                      ///< the player's shots
    shot_t es[MAX_ESHOTS];                      ///< the aliens' shots
    boom_t boom[MAX_BOOMS];                     ///< explosions
    star_t star[STARS];                         ///< the starfield
    hiscore_t hi[5];                            ///< the high scores
    uint8_t snd[1024];                          ///< the synthesizer's state

    uint32_t tick;        ///< frames since the game started
    uint32_t rng;         ///< random numbers
    int mode;             ///< M_ value
    int mode_t;           ///< frames in the mode
    bool paused;          ///< P
    bool quit;            ///< back to the shell
    bool sound;           ///< sound on

    // Input
    unsigned in, in_prev;  ///< IN_ bits held now and last frame
    char typed;            ///< a letter pressed this frame (initials), or 0
    int fire_hold;         ///< frames fire has been held

    // Player
    float px;              ///< the fighter's centre (the left one of a twin)
    float py;              ///< and row (moves while captured)
    float pang;            ///< heading while spinning
    int pstate, pt;        ///< P_ value and frames in it
    bool dual;             ///< a twin fighter
    int lives;             ///< fighters in reserve
    uint32_t score;        ///< the score
    uint32_t next_extra;   ///< the score that gives the next extra fighter
    uint32_t shots_fired, hits;  ///< for the results
    int captor;            ///< the flagship pulling the fighter up (-1: none)

    // Stage
    int stage;             ///< 1 ...
    int level;             ///< normal stages cleared: how hard it is
    bool challenge;        ///< a challenge stage
    int phase, phase_t;    ///< PH_ value and frames in it
    int wave;              ///< the next wave to send
    int wave_t;            ///< frames until it goes
    bool attack;           ///< the formation is diving
    int dive_t;            ///< frames until the next dive
    int chal_hits;         ///< aliens hit on a challenge stage
    float form_dx;         ///< the formation's sway
    float spread;          ///< and how far it has breathed out (1: not)
    float breathe;         ///< where the breathing is
    char msg[24];          ///< a message in the middle (FIGHTER CAPTURED ...)
    int msg_t;             ///< frames it stays

    // High score entry
    int entry_rank;        ///< the place being entered
    int entry_pos;         ///< which letter
    char entry_name[4];    ///< the letters so far
    int title_page;        ///< which title page shows
} world_t;

/// The world.
static world_t *G;

/// Input bits.
enum {
    IN_LEFT = 1, IN_RIGHT = 2, IN_UP = 4, IN_DOWN = 8, IN_FIRE = 16, IN_START = 32,
    IN_PAUSE = 64, IN_QUIT = 128, IN_SOUND = 256, IN_BACK = 512, IN_EXIT = 1024,
};

/// Bits pressed this frame (held now, not last frame).
/// @param bits IN_ bits
/// @return true if one of them was just pressed
static bool pressed(unsigned bits) {
    return (G->in & ~G->in_prev & bits) != 0;
}

// ---- Small helpers ------------------------------------------------------------------------------------

/// A random number (xorshift).
/// @return 32 random bits
static uint32_t rnd(void) {
    uint32_t x = G->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return G->rng = x;
}

/// A random number below n.
/// @param n the limit
/// @return 0 .. n-1
static int rnd_n(int n) {
    return (int)(rnd() % (uint32_t)n);
}

/// The sine of a heading (0-256 a turn).
/// @param a the heading
/// @return its sine
static float sin_a(float a) {
    return G->sin_t[(int)(a + 256.5f) & 255];
}

/// The cosine of a heading.
/// @param a the heading
/// @return its cosine
static float cos_a(float a) {
    return G->sin_t[(int)(a + 64 + 256.5f) & 255];
}

/// The heading from (0,0) towards (dx, dy) (screen rows grow downwards).
/// @param dx across
/// @param dy down
/// @return 0-256, 0 up
static float heading(float dx, float dy) {
    float a = atan2f(dx, -dy) * (128.0f / 3.14159265f);
    return a < 0 ? a + 256 : a;
}

/// Turns heading `a` towards `want` by at most `step`.
/// @param a the heading now
/// @param want the heading wanted
/// @param step the most it may turn
/// @return the new heading
static float turn_to(float a, float want, float step) {
    float d = fmodf(want - a + 384.0f, 256.0f) - 128.0f;  // -128..128
    if (d > step)
        d = step;
    if (d < -step)
        d = -step;
    a += d;
    return a < 0 ? a + 256 : a >= 256 ? a - 256 : a;
}

// ---- Drawing --------------------------------------------------------------------------------------------

/// A pixel in the picture being drawn.
/// @param x column
/// @param y row
/// @param c colour
static inline void pset(int x, int y, uint8_t c) {
    if ((unsigned)x < SW && (unsigned)y < SH)
        BACK[y * SW + x] = c;
}

/// A filled rectangle, clipped.
/// @param x left
/// @param y top
/// @param w width
/// @param h height
/// @param c colour
static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (int r = 0; r < h; ++r)
        memset(BACK + (y + r) * SW + x, c, w > 0 ? (size_t)w : 0);
}

/// A picture centred on (cx, cy), turned to heading `ang`, see-through
/// pixels left alone, clipped to the play area (or the whole screen when
/// `panel` is set).
/// @param img IMG_ value
/// @param cx centre column
/// @param cy centre row
/// @param ang heading
/// @param panel draw over the panel too
static void sprite_at(int img, float cx, float cy, float ang, bool panel) {
    int k = ((int)(ang + 8.0f + 256.0f) >> 4) & (ANGLES - 1);
    const uint8_t *p = G->img[img][k];
    int x0 = (int)floorf(cx) - SPR / 2, y0 = (int)floorf(cy) - SPR / 2;
    int right = panel ? SW : PF_W;
    for (int r = 0; r < SPR; ++r) {
        int y = y0 + r;
        if ((unsigned)y >= SH)
            continue;
        uint8_t *row = BACK + y * SW;
        for (int c = 0; c < SPR; ++c) {
            int x = x0 + c;
            uint8_t v = p[r * SPR + c];
            if (v && x >= 0 && x < right)
                row[x] = v;
        }
    }
}

/// Text in the 5x7 font, 6 pixels a character, at scale 1 or more.
/// @param x left
/// @param y top
/// @param s the text
/// @param c colour
/// @param scale pixels a font pixel
static void text_s(int x, int y, const char *s, uint8_t c, int scale) {
    for (; *s; ++s, x += 6 * scale) {
        const char *f = strchr(font_chars, *s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        if (!f || !*s)
            continue;
        const uint8_t *g = G->font[f - font_chars];
        for (int r = 0; r < 7; ++r)
            for (int b = 0; b < 5; ++b)
                if (g[r] & (0x10 >> b))
                    fill(x + b * scale, y + r * scale, scale, scale, c);
    }
}

/// Text at scale 1.
/// @param x left
/// @param y top
/// @param s the text
/// @param c colour
static void text(int x, int y, const char *s, uint8_t c) {
    text_s(x, y, s, c, 1);
}

/// Text centred in the play area.
/// @param y top
/// @param s the text
/// @param c colour
static void text_mid(int y, const char *s, uint8_t c) {
    text((PF_W - 6 * (int)strlen(s) + 1) / 2, y, s, c);
}

// ---- Making the pictures -----------------------------------------------------------------------------

/// The colour of an art letter (swarm_art.h); 0 for see-through.
/// @param ch the letter
/// @return RGB332
static uint8_t art_colour(char ch) {
    switch (ch) {
    case 'W': return C_WHITE;
    case 'R': return C_RED;
    case 'B': return C_BLUE;
    case 'b': return C_DKBLUE;
    case 'Y': return C_YELLOW;
    case 'G': return C_GREEN;
    case 'g': return RGB332(0, 3, 1);
    case 'C': return RGB332(0, 6, 3);
    case 'P': return C_PURPLE;
    case 'O': return C_ORANGE;
    case 'L': return RGB332(3, 5, 3);
    case 'K': return C_GREY;
    default: return 0;
    }
}

/// A built-in sprite, upright (0: see-through; a drawn black pixel is made
/// the darkest blue so it isn't).
/// @param n ART_ value
/// @param out SPR * SPR bytes
static void art_get(int n, uint8_t *out) {
    for (int r = 0; r < SPR; ++r)
        for (int c = 0; c < SPR / 2; ++c) {
            uint8_t v = art_colour(art_rows[n][r][c]);
            out[r * SPR + c] = out[r * SPR + SPR - 1 - c] = v;
        }
}

/// Recolours a picture: a captured fighter's red, or a hit flagship's purple.
/// @param p SPR * SPR bytes, changed in place
/// @param purple purple (else red)
static void tint(uint8_t *p, bool purple) {
    for (int i = 0; i < SPR * SPR; ++i) {
        uint8_t v = p[i];
        if (!v)
            continue;
        int r = v >> 5, g = (v >> 2) & 7, b = v & 3;
        int nr, ng, nb;
        if (purple) {
            nr = g * 5 / 6 + r / 3;
            ng = r / 2;
            nb = g / 2 > b ? g / 2 : b;
        } else {
            nr = r > g ? r : g;
            if (b * 2 > nr)
                nr = b * 2;
            ng = g / 3;
            nb = b / 3;
        }
        v = RGB332(nr > 7 ? 7 : nr, ng, nb > 3 ? 3 : nb);
        p[i] = v ? v : 1;
    }
}

/// Fills in one image's ANGLES turned copies from an upright picture.
/// @param img IMG_ value
/// @param up the picture, upright
static void make_turns(int img, const uint8_t *up) {
    for (int k = 0; k < ANGLES; ++k) {
        float t = k * (6.2831853f / ANGLES), cs = cosf(t), sn = sinf(t);
        uint8_t *out = G->img[img][k];
        for (int y = 0; y < SPR; ++y)
            for (int x = 0; x < SPR; ++x) {
                float rx = x - 7.5f, ry = y - 7.5f;
                int sx = (int)floorf(cs * rx + sn * ry + 7.5f + 0.5f);
                int sy = (int)floorf(-sn * rx + cs * ry + 7.5f + 0.5f);
                out[y * SPR + x] = (unsigned)sx < SPR && (unsigned)sy < SPR ? up[sy * SPR + sx] : 0;
            }
    }
}

/// Frames read from /SWARM.SPR (spr_load's callback).
/// @param ctx the ART_COUNT x SPR*SPR pictures to fill
/// @param frame which
/// @param px its colours
/// @param on which are drawn
/// @return true to go on
static bool spr_put(void *ctx, int frame, const uint8_t *px, const uint8_t *on) {
    uint8_t *art = ctx;
    if (frame >= ART_COUNT)
        return false;
    for (int i = 0; i < SPR * SPR; ++i)
        art[frame * SPR * SPR + i] = on[i] ? (px[i] ? px[i] : 1) : 0;
    return true;
}

/// Makes every picture: from /SWARM.SPR if there is a usable one (16x16,
/// at least ART_COUNT frames), else the built-in art.
/// @param scratch ART_COUNT * SPR * SPR bytes to work in
/// @return true if /SWARM.SPR was used
static bool make_pictures(uint8_t *scratch) {
    bool from_file = false;
    FILINFO fi;
    if (f_stat(SPR_FILE, &fi) == FR_OK) {
        spr_info_t info;
        const char *err;
        from_file = spr_load(SPR_FILE, &info, spr_put, scratch, &err) && info.w == SPR && info.h == SPR &&
                    info.frames >= ART_COUNT;
    }
    if (!from_file)
        for (int n = 0; n < ART_COUNT; ++n)
            art_get(n, scratch + n * SPR * SPR);
    static const uint8_t from[IMG_COUNT] = {
        ART_SHIP, ART_SHIP, ART_DRONE_A, ART_DRONE_B, ART_MOTH_A, ART_MOTH_B, ART_BOSS_A,
        ART_BOSS_B, ART_BOSS_A, ART_BOSS_B, ART_DART_A, ART_DART_B, ART_DISC_A, ART_DISC_B,
    };
    uint8_t up[SPR * SPR];
    for (int i = 0; i < IMG_COUNT; ++i) {
        memcpy(up, scratch + from[i] * SPR * SPR, sizeof up);
        if (i == IMG_CAPTIVE)
            tint(up, false);
        if (i == IMG_BOSS_HIT || i == IMG_BOSS_HIT_B)
            tint(up, true);
        make_turns(i, up);
    }
    return from_file;
}

// ---- High scores -----------------------------------------------------------------------------------

/// Reads /SWARM.HI ("score name" lines), or sets the starting table.
static void hi_load(void) {
    static const hiscore_t start[5] = {
        {30000, "ACE"}, {25000, "PIX"}, {20000, "RGB"}, {15000, "USB"}, {10000, "OS2"},
    };
    memcpy(G->hi, start, sizeof G->hi);
    FIL f;
    if (f_open(&f, HI_FILE, FA_READ) != FR_OK)
        return;
    char buf[200];
    UINT got = 0;
    f_read(&f, buf, sizeof buf - 1, &got);
    f_close(&f);
    buf[got] = 0;
    char *p = buf;
    for (int i = 0; i < 5 && *p; ++i) {
        unsigned long s = strtoul(p, &p, 10);
        while (*p == ' ')
            ++p;
        char name[4] = "   ";
        for (int k = 0; k < 3 && *p && *p != '\n' && *p != '\r'; ++k)
            name[k] = *p++;
        while (*p && *p != '\n')
            ++p;
        if (*p)
            ++p;
        G->hi[i].score = (uint32_t)s;
        memcpy(G->hi[i].name, name, 4);
    }
}

/// Writes the table to /SWARM.HI.
/// @return true if it was written
static bool hi_save(void) {
    char buf[200];
    int n = 0;
    for (int i = 0; i < 5; ++i)
        n += snprintf(buf + n, sizeof buf - n, "%lu %.3s\n", (unsigned long)G->hi[i].score, G->hi[i].name);
    FIL f;
    if (f_open(&f, HI_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT put = 0;
    f_write(&f, buf, (UINT)n, &put);
    return f_close(&f) == FR_OK && put == (UINT)n;
}

// ---- The formation ---------------------------------------------------------------------------------

/// A formation place's row (0 flagships, 1-2 moths, 3-4 drones) and column (0-9).
/// @param slot 0-39
/// @param row set to the row
/// @param col set to the column
static void slot_rc(int slot, int *row, int *col) {
    if (slot < 4) {
        *row = 0, *col = 3 + slot;
    } else if (slot < 20) {
        *row = 1 + (slot - 4) / 8, *col = 1 + (slot - 4) % 8;
    } else {
        *row = 3 + (slot - 20) / 10, *col = (slot - 20) % 10;
    }
}

/// The place at row, column.
/// @param row 0-4
/// @param col 0-9
/// @return 0-39
static int slot_of(int row, int col) {
    return row == 0 ? col - 3 : row < 3 ? 4 + (row - 1) * 8 + col - 1 : 20 + (row - 3) * 10 + col;
}

/// Where a formation place is now (it sways and breathes).
/// @param slot 0-39
/// @param x set to its centre column
/// @param y set to its centre row
static void slot_pos(int slot, float *x, float *y) {
    int row, col;
    slot_rc(slot, &row, &col);
    *x = FORM_CX + G->form_dx + (col - 4.5f) * 17.0f * G->spread;
    *y = FORM_TOP + row * 15.0f * (1.0f + (G->spread - 1.0f) * 0.7f);
}

// ---- Paths --------------------------------------------------------------------------------------------

/// Flying in from the top middle, a three-quarter loop out to the side.
static const path_t path_top = {PF_W / 2 - 10, -10, 128, {{44, 0, 2.3f}, {66, 3.0f, 0}, {0, 0, 0}}};
/// Flying in from low on the side, a loop up and over.
static const path_t path_side = {-10, 200, 44, {{34, 0, 2.3f}, {70, -2.6f, 0}, {0, 0, 0}}};
/// Flying in from a top corner, down and round.
static const path_t path_corner = {PF_W - 34, -10, 150, {{36, 0, 2.3f}, {78, 3.0f, 0}, {0, 0, 0}}};
/// Out of the formation: the half loop to start a dive.
static const path_t path_lift = {0, 0, 0, {{6, 0, 1.4f}, {30, 4.2f, 1.7f}, {0, 0, 0}}};
/// Challenge: down the middle, a full loop, and out at the bottom.
static const path_t chal_loop = {PF_W / 2 - 14, -10, 128, {{46, 0, 2.5f}, {86, 3.0f, 0}, {200, 0, 0}, {0, 0, 0}}};
/// Challenge: up from low on the side, over, and down out the far side.
static const path_t chal_arch = {-10, 190, 40, {{40, 0, 2.5f}, {60, -2.4f, 0}, {30, 0, 0}, {40, 1.4f, 0}, {200, 0, 0}, {0, 0, 0}}};
/// Challenge: in from high on the side, weaving across and down.
static const path_t chal_weave = {PF_W + 10, 46, 196, {{26, 0, 2.5f}, {44, -1.8f, 0}, {44, 1.8f, 0}, {44, -1.8f, 0}, {200, 0, 0}, {0, 0, 0}}};

/// Puts an alien on a path (mirror -1: the right-hand version).
/// @param e the alien
/// @param p the path
/// @param mirror 1 or -1
/// @param from_here start where the alien is, heading as it is (else at the path's start)
static void set_path(enemy_t *e, const path_t *p, int mirror, bool from_here) {
    e->path = p;
    e->seg = 0;
    e->seg_left = p->seg[0].frames;
    e->mirror = (int8_t)mirror;
    if (!from_here) {
        e->x = mirror > 0 ? p->x : PF_W - p->x;
        e->y = p->y;
        e->ang = mirror > 0 ? p->ang : fmodf(256.0f - p->ang, 256.0f);
    }
    if (p->seg[0].speed)
        e->speed = p->seg[0].speed;
}

/// One frame along the path: false once it has ended.
/// @param e the alien
/// @return true while on the path
static bool follow(enemy_t *e) {
    const seg_t *s = &e->path->seg[e->seg];
    if (!s->frames)
        return false;
    if (s->speed)
        e->speed = s->speed;
    e->ang = fmodf(e->ang + s->turn * e->mirror + 256.0f, 256.0f);
    if (--e->seg_left <= 0) {
        ++e->seg;
        e->seg_left = e->path->seg[e->seg].frames;
    }
    return true;
}

/// Moves an alien a frame along its heading.
/// @param e the alien
static void advance(enemy_t *e) {
    e->x += sin_a(e->ang) * e->speed;
    e->y -= cos_a(e->ang) * e->speed;
}

// ---- Effects --------------------------------------------------------------------------------------------

/// Starts an explosion (or a points pop-up).
/// @param x where
/// @param y where
/// @param big 0 an alien's, 1 the fighter's, 2 points
/// @param points the points shown (big 2)
static void boom(float x, float y, int big, unsigned points) {
    for (int i = 0; i < MAX_BOOMS; ++i)
        if (G->boom[i].t < 0) {
            G->boom[i] = (boom_t){x, y, 0, (uint8_t)big, (uint16_t)points};
            return;
        }
}

/// Adds to the score, with the extra fighters it earns (20000, 70000, then
/// every 70000).
/// @param n points
static void add_score(unsigned n) {
    G->score += n;
    if (G->score >= G->next_extra) {
        G->next_extra = G->next_extra == 20000 ? 70000 : G->next_extra + 70000;
        if (G->lives < 9)
            ++G->lives;
        snd_tune(TUNE_EXTRA);
    }
}

/// A message in the middle of the play area for a while.
/// @param s the message
/// @param frames how long
static void message(const char *s, int frames) {
    snprintf(G->msg, sizeof G->msg, "%s", s);
    G->msg_t = frames;
}

// ---- Aliens ---------------------------------------------------------------------------------------------

/// Whether an alien is out of the formation, on the attack.
/// @param e the alien
/// @return true while it dives
static bool diving(const enemy_t *e) {
    return e->state == E_DIVE || e->state == E_ATTACK || e->state == E_ESCORT || e->state == E_BEAM ||
           e->state == E_RETURN;
}

/// How many aliens are out diving.
/// @return the count
static int divers(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        n += diving(&G->e[i]);
    return n;
}

/// How many aliens are left to shoot this stage.
/// @return the count
static int aliens_left(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        n += e->state != E_NONE && e->state != E_RESCUE && e->state != E_CAPTIVE;
    }
    return n;
}

/// Whether there is a captured fighter (riding a flagship, or on its way back).
/// @return true if there is one
static bool captive_exists(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state == E_CAPTIVE || G->e[i].state == E_RESCUE)
            return true;
    return false;
}

/// Whether a flagship has its beam out (or is on its way to).
/// @return true if so
static bool beam_active(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state != E_NONE && G->e[i].beam)
            return true;
    return false;
}

/// A free alien entry.
/// @return its index, or -1
static int free_enemy(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state == E_NONE)
            return i;
    return -1;
}

/// The fighter's centre column, for aiming (the middle of a twin).
/// @return the column
static float aim_x(void) {
    return G->px + (G->dual ? 8.0f : 0.0f);
}

/// An alien fires at the fighter.
/// @param e the alien
static void enemy_fire(const enemy_t *e) {
    if (G->pstate != P_ALIVE || e->y > PLAYER_Y - 40)
        return;
    for (int i = 0; i < MAX_ESHOTS; ++i) {
        shot_t *s = &G->es[i];
        if (s->on)
            continue;
        float speed = 2.6f + (G->level > 8 ? 8 : G->level) * 0.12f;
        float t = (PLAYER_Y - e->y) / speed;
        float dx = (aim_x() - e->x) / t;
        if (dx > 1.1f) dx = 1.1f;
        if (dx < -1.1f) dx = -1.1f;
        *s = (shot_t){e->x, e->y + 6, dx, speed, true};
        return;
    }
}

/// The picture for an alien (its wings beat).
/// @param e the alien
/// @return IMG_ value
static int enemy_img(const enemy_t *e) {
    int flap = diving(e) || e->state == E_PATH || e->state == E_HOME ? (G->tick >> 3) & 1 : (G->tick >> 5) & 1;
    switch (e->kind) {
    case K_DRONE: return IMG_DRONE + flap;
    case K_MOTH: return IMG_MOTH + flap;
    case K_BOSS: return (e->hits > 1 ? IMG_BOSS : IMG_BOSS_HIT) + flap;
    case K_DART: return IMG_DART + flap;
    case K_DISC: return IMG_DISC + ((G->tick >> 2) & 1);
    default: return IMG_CAPTIVE;
    }
}

/// Sends an alien out of the formation on a dive.
/// @param i the alien
/// @param beam a flagship's tractor-beam dive
static void start_dive(int i, bool beam) {
    enemy_t *e = &G->e[i];
    int row, col;
    slot_rc(e->slot, &row, &col);
    int side = col < 5 ? -1 : 1;
    if (e->x < 30) side = 1;
    if (e->x > PF_W - 30) side = -1;
    e->state = E_DIVE;
    e->timer = 0;
    e->beam = beam;
    e->kills = 0;
    e->wobble = e->kind == K_MOTH ? (uint8_t)(1 + rnd_n(2)) : 0;
    int lv = G->level > 10 ? 10 : G->level;
    e->shots = (uint8_t)(beam ? 0 : 1 + (lv >= 2) + (lv >= 6) + (e->kind == K_BOSS));
    e->ang = 0;
    set_path(e, &path_lift, side, true);
    snd_fx(FX_DIVE);
}

/// Picks aliens to dive, now and then, harder each stage.
static void launch_dives(void) {
    if (!G->attack || G->pstate != P_ALIVE || G->challenge)
        return;
    if (--G->dive_t > 0)
        return;
    int lv = G->level > 12 ? 12 : G->level;
    int left = aliens_left();
    int period = 100 - lv * 6;
    if (left < 12)
        period /= 2;
    if (period < 24)
        period = 24;
    G->dive_t = period + rnd_n(period / 2 + 1);
    int max_div = 2 + lv / 2;
    if (max_div > 7)
        max_div = 7;
    if (left <= 6)
        max_div = left;
    if (divers() >= max_div)
        return;
    // A random alien in the formation.
    int pick[MAX_ENEMIES], n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state == E_FORM)
            pick[n++] = i;
    if (!n)
        return;
    int i = pick[rnd_n(n)];
    enemy_t *e = &G->e[i];
    if (e->kind == K_BOSS) {
        bool can_beam = e->link < 0 && !captive_exists() && !G->dual && !beam_active() && G->lives >= 0;
        if (can_beam && rnd_n(100) < 45) {
            start_dive(i, true);
            return;
        }
        start_dive(i, false);
        // Up to two moths from just below come along.
        int row, col, got = 0;
        slot_rc(e->slot, &row, &col);
        for (int dc = -1; dc <= 1 && got < 2; ++dc) {
            for (int j = 0; j < MAX_ENEMIES && got < 2; ++j) {
                enemy_t *m = &G->e[j];
                int r2, c2;
                if (m->state != E_FORM || m->kind != K_MOTH)
                    continue;
                slot_rc(m->slot, &r2, &c2);
                if (r2 != 1 || c2 != col + dc)
                    continue;
                m->state = E_ESCORT;
                m->link = (int8_t)i;
                m->ox = m->x - e->x;
                m->oy = 10.0f;
                if (fabsf(m->ox) < 4.0f)
                    m->oy = 16.0f;  // the one straight below follows behind
                m->shots = e->shots;
                ++got;
            }
        }
        return;
    }
    start_dive(i, false);
}

/// Back to the formation (or off the screen, on a challenge stage) after
/// leaving the bottom of the screen.
/// @param e the alien
static void off_bottom(enemy_t *e) {
    e->state = E_RETURN;
    e->y = -12;
    e->ang = 128;
    e->beam = false;
    float sx, sy;
    slot_pos(e->slot, &sx, &sy);
    e->x = sx;
}

/// One frame of homing on its formation place; true once there.
/// @param e the alien
/// @return true when it has arrived
static bool home(enemy_t *e) {
    float sx, sy;
    slot_pos(e->slot, &sx, &sy);
    float dx = sx - e->x, dy = sy - e->y, d = sqrtf(dx * dx + dy * dy);
    if (d < 1.5f) {
        e->x = sx, e->y = sy;
        return true;
    }
    float speed = 2.4f;
    if (d < 26.0f) {
        float step = d < speed ? d : speed;
        e->x += dx / d * step;
        e->y += dy / d * step;
        e->ang = turn_to(e->ang, 0, 10);
    } else {
        e->speed = speed;
        e->ang = turn_to(e->ang, heading(dx, dy), 7);
        advance(e);
    }
    return false;
}

/// Frees the fighter the beam is pulling up (its flagship was shot).
static void release_player(void) {
    if (G->pstate == P_CAPTURED) {
        G->pstate = P_FREE;
        G->pt = 0;
    }
    G->captor = -1;
}

/// Ends a flagship's beam.
/// @param e the flagship
static void beam_off(enemy_t *e) {
    if (e->beam && e->state == E_BEAM)
        snd_fx_stop(FX_BEAM);
    e->beam = false;
}

/// An alien is shot (or rammed).
/// @param i which
static void kill_enemy(int i) {
    enemy_t *e = &G->e[i];
    bool dive = diving(e) || (e->state == E_PATH && !e->flyby);
    if (e->kind == K_BOSS && e->hits > 1) {
        --e->hits;
        snd_fx(FX_BOSS_HIT);
        return;
    }
    unsigned pts = 0;
    switch (e->kind) {
    case K_DRONE: pts = dive ? 100 : 50; break;
    case K_MOTH: pts = dive ? 160 : 80; break;
    case K_BOSS: pts = dive ? 400u << e->kills : 150; break;
    case K_RENEGADE: pts = dive ? 1000 : 500; break;
    case K_CAPTIVE: pts = 1000; break;
    default: pts = 100; break;
    }
    if (e->flyby) {
        pts = 100;
        ++G->chal_hits;
    }
    add_score(pts);
    ++G->hits;
    boom(e->x, e->y, 0, 0);
    if (pts >= 400 || e->kind == K_CAPTIVE)
        boom(e->x, e->y, 2, pts);
    snd_fx(e->kind == K_BOSS ? FX_BOSS_BOOM : e->kind == K_CAPTIVE ? FX_CAPTIVE_HIT : FX_HIT);

    if (e->state == E_ESCORT && e->link >= 0 && diving(&G->e[e->link]))
        ++G->e[e->link].kills;
    if (e->kind == K_BOSS) {
        if (G->captor == i)
            release_player();
        beam_off(e);
        // Its escorts carry on alone.
        for (int j = 0; j < MAX_ENEMIES; ++j)
            if (G->e[j].state == E_ESCORT && G->e[j].link == i) {
                G->e[j].state = E_ATTACK;
                G->e[j].tx = aim_x();
                G->e[j].link = -1;
            }
        // Its captive: freed if it was diving, else it turns on the player.
        if (e->link >= 0) {
            enemy_t *c = &G->e[e->link];
            if (dive) {
                c->state = E_RESCUE;
                c->timer = 0;
                c->link = -1;
                message("FIGHTER RESCUED", 120);
                snd_tune(TUNE_RESCUE);
            } else {
                c->kind = K_RENEGADE;
                c->state = E_HOME;
                c->slot = e->slot;
                c->hits = 1;
                c->link = -1;
            }
        }
    }
    if (e->kind == K_CAPTIVE && e->link >= 0)
        G->e[e->link].link = -1;
    e->state = E_NONE;
    e->link = -1;
    e->beam = false;
}

/// The beam reaches down from the flagship: how far, by its timer.
/// @param e the flagship
/// @return pixels
static int beam_len(const enemy_t *e) {
    int t = e->timer, full = PLAYER_Y + 10 - (int)e->y - 8;
    if (t < 50)
        return full * t / 50;
    if (t < 230)
        return full;
    if (t < 280)
        return full * (280 - t) / 50;
    return 0;
}

/// One frame of an alien.
/// @param i which
static void update_enemy(int i) {
    enemy_t *e = &G->e[i];
    ++e->timer;
    switch (e->state) {
    case E_WAIT:
        if (e->timer >= 0) {
            e->state = E_PATH;
            e->timer = 0;
        }
        break;
    case E_PATH:
        if (!follow(e)) {
            if (e->flyby) {
                e->state = E_NONE;  // flew off unhit
                break;
            }
            e->state = E_HOME;
        }
        advance(e);
        if (e->flyby && e->timer > 20 && (e->x < -20 || e->x > PF_W + 20 || e->y > SH + 20 || e->y < -30))
            e->state = E_NONE;
        // From the fourth stage on, some fire as they fly in.
        if (!e->flyby && G->level >= 2 && e->timer == 50 && rnd_n(100) < 25 + G->level * 3)
            enemy_fire(e);
        break;
    case E_HOME:
    case E_RETURN:
        if (home(e)) {
            e->state = E_FORM;
            e->beam = false;
        }
        break;
    case E_FORM:
        slot_pos(e->slot, &e->x, &e->y);
        e->ang = turn_to(e->ang, 0, 10);
        break;
    case E_DIVE:
        if (!follow(e)) {
            e->state = E_ATTACK;
            e->timer = 0;
            e->tx = aim_x() + (e->beam ? 0 : (float)(rnd_n(61) - 30));
        }
        advance(e);
        break;
    case E_ATTACK: {
        float speed = 1.9f + (G->level > 10 ? 10 : G->level) * 0.09f;
        e->speed = e->speed < speed ? e->speed + 0.05f : speed;
        if (e->y < PLAYER_Y - 50) {
            float tx = e->tx;
            if (e->wobble)
                tx += 40.0f * sinf(e->timer * 0.055f * e->wobble);
            if (e->beam)
                tx = e->tx = e->tx + (aim_x() - e->tx) * 0.02f;
            e->ang = turn_to(e->ang, heading(tx - e->x, SH + 40 - e->y), 1.7f);
        }
        advance(e);
        if (e->shots && e->y > 60 && e->y < 160 && rnd_n(28) == 0) {
            --e->shots;
            enemy_fire(e);
        }
        if (e->beam && e->y >= BEAM_Y) {
            e->state = E_BEAM;
            e->timer = 0;
            e->ang = 128;
            snd_fx(FX_BEAM);
        }
        if (e->y > SH + 12 || e->x < -40 || e->x > PF_W + 40) {
            // Its escorts come back round with it.
            for (int j = 0; j < MAX_ENEMIES; ++j)
                if (G->e[j].state == E_ESCORT && G->e[j].link == i) {
                    G->e[j].link = -1;
                    off_bottom(&G->e[j]);
                }
            off_bottom(e);
        }
        break;
    }
    case E_ESCORT: {
        const enemy_t *b = e->link >= 0 ? &G->e[e->link] : NULL;
        if (!b || !diving(b) || b->state == E_BEAM) {
            e->state = E_ATTACK;
            e->tx = aim_x();
            e->link = -1;
            break;
        }
        e->x = b->x + e->ox * cos_a(b->ang) - e->oy * sin_a(b->ang);
        e->y = b->y + e->ox * sin_a(b->ang) + e->oy * cos_a(b->ang);
        e->ang = b->ang;
        e->speed = b->speed;
        if (e->shots && e->y > 60 && e->y < 160 && rnd_n(30) == 0) {
            --e->shots;
            enemy_fire(e);
        }
        break;
    }
    case E_BEAM:
        // The beam stays out while it pulls the fighter up.
        if (G->captor == i && G->pstate == P_CAPTURED && e->timer > 200)
            e->timer = 200;
        if (G->pstate == P_ALIVE && !G->dual && e->timer > 60 && e->timer < 220 && G->captor < 0 &&
            fabsf(aim_x() - e->x) < 18.0f) {
            G->pstate = P_CAPTURED;
            G->pt = 0;
            G->captor = i;
            G->pang = 0;
            // The aliens' shots are cleared so it isn't shot on the way up.
            for (int k = 0; k < MAX_ESHOTS; ++k)
                G->es[k].on = false;
        }
        if (e->timer >= 280) {
            beam_off(e);
            e->state = E_ATTACK;
            e->tx = e->x;
            e->timer = 0;
        }
        break;
    case E_CAPTIVE: {
        const enemy_t *b = &G->e[e->link];
        e->x = b->x;
        e->y = b->y - 15;
        e->ang = b->ang;
        break;
    }
    case E_RESCUE: {
        // Spins down to dock beside the fighter (or waits for the next one).
        e->ang = fmodf(e->ang + 12.0f, 256.0f);
        float tx = PF_W / 2.0f, ty = 150;
        bool dock = G->pstate == P_ALIVE && !G->dual;
        if (dock) {
            tx = G->px + 16 > PF_W - 8 ? G->px - 16 : G->px + 16;
            ty = PLAYER_Y;
        }
        float dx = tx - e->x, dy = ty - e->y, d = sqrtf(dx * dx + dy * dy);
        float step = 1.6f;
        if (d > step) {
            e->x += dx / d * step;
            e->y += dy / d * step;
        } else if (dock) {
            if (tx < G->px)
                G->px = tx;
            G->dual = true;
            e->state = E_NONE;
            snd_fx(FX_DOCK);
        } else {
            e->x = tx, e->y = ty;
        }
        break;
    }
    }
}

// ---- Stages -------------------------------------------------------------------------------------------

/// One wave: its path, whether it comes in two lines (mirrored), and its places.
typedef struct {
    const path_t *path;  ///< the path
    int8_t mirror;       ///< 1, -1, or 0 for two lines (the first four left, the rest right)
    uint8_t slots[8];    ///< formation places, row * 10 + column
} wave_t;

/// The five waves of a normal stage.
static const wave_t waves[5] = {
    {&path_top, 0, {14, 15, 24, 25, 34, 35, 44, 45}},
    {&path_side, 1, {3, 13, 4, 16, 5, 23, 6, 26}},
    {&path_side, -1, {11, 12, 17, 18, 21, 22, 27, 28}},
    {&path_corner, 1, {36, 37, 46, 47, 38, 39, 48, 49}},
    {&path_corner, -1, {33, 32, 43, 42, 31, 30, 41, 40}},
};

/// Sends wave `w` (for a challenge stage, a wave of flybys).
/// @param w 0-4
static void send_wave(int w) {
    if (G->challenge) {
        // Kinds and paths change from one challenge stage to the next.
        static const path_t *const paths[5] = {&chal_loop, &chal_arch, &chal_arch, &chal_weave, &chal_weave};
        static const int8_t mirrors[5] = {0, 1, -1, 1, -1};
        static const uint8_t kinds[4] = {K_DRONE, K_DART, K_DISC, K_MOTH};
        int c = (G->stage - 3) / 4;
        int kind = kinds[(c + w) % 4];
        for (int k = 0; k < 8; ++k) {
            int i = free_enemy();
            if (i < 0)
                return;
            enemy_t *e = &G->e[i];
            memset(e, 0, sizeof *e);
            int m = mirrors[w] ? mirrors[w] : (k < 4 ? 1 : -1);
            set_path(e, paths[w], c & 1 ? -m : m, false);
            e->kind = (uint8_t)kind;
            e->flyby = true;
            e->hits = 1;
            e->link = -1;
            e->state = E_WAIT;
            e->timer = (int16_t)(-(mirrors[w] ? k : k % 4) * 9);
        }
        return;
    }
    const wave_t *wv = &waves[w];
    for (int k = 0; k < 8; ++k) {
        int i = free_enemy();
        if (i < 0)
            return;
        enemy_t *e = &G->e[i];
        memset(e, 0, sizeof *e);
        int row = wv->slots[k] / 10, col = wv->slots[k] % 10;
        e->slot = (uint8_t)slot_of(row, col);
        e->kind = row == 0 ? K_BOSS : row < 3 ? K_MOTH : K_DRONE;
        e->hits = e->kind == K_BOSS ? 2 : 1;
        e->link = -1;
        int m = wv->mirror ? wv->mirror : (k < 4 ? 1 : -1);
        set_path(e, wv->path, m, false);
        e->state = E_WAIT;
        e->timer = (int16_t)(-(wv->mirror ? k : k % 4) * 9);
    }
}

/// Starts stage `n` (the stage text first).
/// @param n the stage
static void start_stage(int n) {
    G->stage = n;
    G->challenge = n % 4 == 3;
    G->phase = PH_INTRO;
    G->phase_t = 0;
    G->wave = 0;
    G->wave_t = 0;
    G->attack = false;
    G->dive_t = 120;
    G->chal_hits = 0;
    G->form_dx = 0;
    G->spread = 1;
    G->breathe = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state != E_RESCUE)
            G->e[i].state = E_NONE;
    for (int i = 0; i < MAX_ESHOTS; ++i)
        G->es[i].on = false;
    if (G->challenge)
        snd_tune(TUNE_CHALLENGE);
}

/// One frame of the stage: the waves, the formation, the dives, and the end.
static void update_stage(void) {
    ++G->phase_t;
    switch (G->phase) {
    case PH_INTRO:
        if (G->phase_t >= (G->stage == 1 && G->tick < 400 ? 260 : 110)) {
            G->phase = PH_WAVES;
            G->phase_t = 0;
        }
        return;
    case PH_WAVES:
        if (G->wave_t > -10000)
            --G->wave_t;
        if (G->wave < 5 && G->wave_t <= 0) {
            send_wave(G->wave++);
            G->wave_t = G->challenge ? 150 : 135;
        }
        if (!G->attack && !G->challenge && G->wave == 5 && G->wave_t <= 0) {
            bool settled = true;
            for (int i = 0; i < MAX_ENEMIES; ++i)
                if (G->e[i].state == E_PATH || G->e[i].state == E_WAIT || G->e[i].state == E_HOME)
                    settled = false;
            if (settled || G->wave_t < -240)
                G->attack = true;
        }
        if (G->wave == 5 && G->wave_t <= 0 && aliens_left() == 0) {
            G->phase = G->challenge ? PH_BONUS : PH_CLEAR;
            G->phase_t = 0;
            if (G->challenge) {
                G->score += G->chal_hits == 40 ? 10000 : G->chal_hits * 100;
                add_score(0);
                snd_tune(G->chal_hits == 40 ? TUNE_PERFECT : TUNE_EXTRA);
            } else {
                ++G->level;
            }
        }
        break;
    case PH_CLEAR:
        if (G->phase_t > 90)
            start_stage(G->stage + 1);
        return;
    case PH_BONUS:
        if (G->phase_t > 240)
            start_stage(G->stage + 1);
        return;
    }
    // The formation sways while the aliens arrive, then breathes.
    if (G->attack) {
        G->form_dx *= 0.97f;
        G->breathe += 6.2831853f / 200.0f;
        G->spread = 1.0f + 0.09f * (1.0f - cosf(G->breathe));
    } else {
        G->form_dx = 14.0f * sinf(G->phase_t * (6.2831853f / 300.0f));
    }
    launch_dives();
}

// ---- The player --------------------------------------------------------------------------------------

/// The fighter (or one of a twin) is hit.
/// @param right the right-hand one of a twin
static void player_hit(bool right) {
    if (G->dual) {
        float x = G->px + (right ? 16 : 0);
        boom(x, PLAYER_Y, 1, 0);
        snd_fx(FX_PLAYER_BOOM);
        if (!right)
            G->px += 16;
        G->dual = false;
        return;
    }
    boom(G->px, PLAYER_Y, 1, 0);
    snd_fx(FX_PLAYER_BOOM);
    G->pstate = P_DYING;
    G->pt = 0;
}

/// Shots and aliens against the fighter, and the fighter's shots against the aliens.
static void collisions(void) {
    // The player's shots.
    for (int s = 0; s < MAX_PSHOTS; ++s) {
        shot_t *sh = &G->ps[s];
        if (!sh->on)
            continue;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            enemy_t *e = &G->e[i];
            if (e->state == E_NONE || e->state == E_WAIT || e->state == E_RESCUE)
                continue;
            if (fabsf(sh->x - e->x) < 7.0f && fabsf(sh->y - e->y) < 8.0f) {
                sh->on = false;
                kill_enemy(i);
                break;
            }
        }
    }
    if (G->pstate != P_ALIVE)
        return;
    int ships = G->dual ? 2 : 1;
    for (int k = 0; k < ships; ++k) {
        float x = G->px + k * 16;
        for (int s = 0; s < MAX_ESHOTS; ++s) {
            shot_t *sh = &G->es[s];
            if (sh->on && fabsf(sh->x - x) < 5.0f && fabsf(sh->y - PLAYER_Y) < 7.0f) {
                sh->on = false;
                player_hit(k == 1);
                return;
            }
        }
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            enemy_t *e = &G->e[i];
            if (e->state == E_NONE || e->state == E_WAIT || e->state == E_RESCUE || e->state == E_FORM)
                continue;
            if (fabsf(e->x - x) < 11.0f && fabsf(e->y - PLAYER_Y) < 11.0f) {
                if (e->kind == K_BOSS)
                    e->hits = 1;
                kill_enemy(i);
                player_hit(k == 1);
                return;
            }
        }
    }
}

/// One frame of the fighter.
static void update_player(void) {
    ++G->pt;
    switch (G->pstate) {
    case P_ALIVE: {
        float right = PF_W - 8 - (G->dual ? 16 : 0);
        if (G->in & IN_LEFT)
            G->px -= 2.0f;
        if (G->in & IN_RIGHT)
            G->px += 2.0f;
        if (G->px < 8) G->px = 8;
        if (G->px > right) G->px = right;
        // Fire: each press, and every 9 frames while held. One shot (a pair
        // for a twin) at a time on the screen, two with a gap.
        bool fire = pressed(IN_FIRE) || ((G->in & IN_FIRE) && ++G->fire_hold % 9 == 0);
        if (!(G->in & IN_FIRE))
            G->fire_hold = 0;
        if (fire && G->phase != PH_INTRO) {
            int ships = G->dual ? 2 : 1, freed = 0;
            for (int s = 0; s < MAX_PSHOTS; ++s)
                freed += !G->ps[s].on;
            if (freed >= ships && freed > MAX_PSHOTS - 2 * ships) {
                for (int k = 0; k < ships; ++k)
                    for (int s = 0; s < MAX_PSHOTS; ++s)
                        if (!G->ps[s].on) {
                            G->ps[s] = (shot_t){G->px + k * 16, PLAYER_Y - 8, 0, -6.5f, true};
                            break;
                        }
                ++G->shots_fired;
                snd_fx(FX_SHOT);
            }
        }
        break;
    }
    case P_DYING:
        if (G->pt > 100) {
            G->pstate = P_WAIT;
            G->pt = 0;
        }
        break;
    case P_GONE:
        if (G->pt > 120) {
            G->pstate = P_WAIT;
            G->pt = 0;
        }
        break;
    case P_WAIT:
        if (G->lives <= 0) {
            if (G->pt > 60) {
                G->pstate = P_OVER;
                G->mode = M_GAMEOVER;
                G->mode_t = 0;
                snd_fx_stop(FX_BEAM);
                snd_tune(TUNE_GAME_OVER);
            }
            break;
        }
        if ((G->pt > 60 && divers() == 0 && G->phase != PH_INTRO) || G->pt > 600) {
            --G->lives;
            G->pstate = P_READY;
            G->pt = 0;
            G->px = PF_W / 2.0f;
            G->py = PLAYER_Y;
            G->pang = 0;
            G->dual = false;
        }
        break;
    case P_READY:
        if (G->pt > 90) {
            G->pstate = P_ALIVE;
            G->pt = 0;
        }
        break;
    case P_CAPTURED: {
        enemy_t *b = G->captor >= 0 ? &G->e[G->captor] : NULL;
        if (!b || b->state != E_BEAM) {
            release_player();
            break;
        }
        G->pang = fmodf(G->pang + 10.0f, 256.0f);
        G->px += (b->x - G->px) * 0.05f;
        G->py -= 0.8f;
        if (G->py <= b->y + 2) {
            // Carried off: the fighter now rides on the flagship.
            int c = free_enemy();
            if (c >= 0) {
                enemy_t *e = &G->e[c];
                memset(e, 0, sizeof *e);
                e->kind = K_CAPTIVE;
                e->state = E_CAPTIVE;
                e->link = (int8_t)G->captor;
                e->hits = 1;
                e->x = b->x;
                e->y = b->y - 15;
                b->link = (int8_t)c;
            }
            beam_off(b);
            b->state = E_HOME;
            b->timer = 0;
            G->captor = -1;
            G->pstate = P_GONE;
            G->pt = 0;
            message("FIGHTER CAPTURED", 200);
            snd_tune(TUNE_CAPTURED);
        }
        break;
    }
    case P_FREE:
        G->pang = turn_to(G->pang, 0, 8);
        G->py += 1.2f;
        if (G->py >= PLAYER_Y) {
            G->py = PLAYER_Y;
            G->pang = 0;
            G->pstate = P_ALIVE;
            G->pt = 0;
        }
        break;
    }
}

/// One frame of the shots, explosions and stars.
static void update_things(void) {
    for (int s = 0; s < MAX_PSHOTS; ++s) {
        shot_t *sh = &G->ps[s];
        if (sh->on && (sh->y += sh->dy) < -8)
            sh->on = false;
    }
    for (int s = 0; s < MAX_ESHOTS; ++s) {
        shot_t *sh = &G->es[s];
        if (!sh->on)
            continue;
        sh->x += sh->dx;
        sh->y += sh->dy;
        if (sh->y > SH + 4 || sh->x < -4 || sh->x > PF_W + 4)
            sh->on = false;
    }
    for (int i = 0; i < MAX_BOOMS; ++i) {
        boom_t *b = &G->boom[i];
        int life = b->big == 1 ? 90 : b->big == 2 ? 70 : 26;
        if (b->t >= 0 && ++b->t > life)
            b->t = -1;
    }
    float speed = G->mode == M_PLAY && G->pstate != P_DYING ? 1.0f : 0.35f;
    for (int i = 0; i < STARS; ++i) {
        star_t *s = &G->star[i];
        s->y += s->speed * speed;
        if (s->y >= SH) {
            s->y -= SH;
            s->x = (uint8_t)rnd_n(PF_W);
        }
    }
    if (G->msg_t)
        --G->msg_t;
}

// ---- Modes --------------------------------------------------------------------------------------------

/// A new game.
static void new_game(void) {
    memset(G->e, 0, sizeof G->e);
    memset(G->ps, 0, sizeof G->ps);
    memset(G->es, 0, sizeof G->es);
    G->score = 0;
    G->next_extra = 20000;
    G->lives = 2;
    G->level = 0;
    G->shots_fired = G->hits = 0;
    G->px = PF_W / 2.0f;
    G->py = PLAYER_Y;
    G->pang = 0;
    G->pstate = P_READY;
    G->pt = -170;
    G->dual = false;
    G->captor = -1;
    G->msg_t = 0;
    G->mode = M_PLAY;
    G->mode_t = 0;
    G->paused = false;
    G->tick = 0;
    start_stage(1);
    snd_tune(TUNE_START);
}

/// The high-score place a score earns (-1: none).
/// @param score the score
/// @return 0-4, or -1
static int hi_rank(uint32_t score) {
    for (int i = 0; i < 5; ++i)
        if (score > G->hi[i].score)
            return i;
    return -1;
}

/// One frame of whatever is showing.
static void update(void) {
    ++G->mode_t;
    if (pressed(IN_EXIT)) {
        G->quit = true;
        return;
    }
    if (pressed(IN_SOUND)) {
        G->sound = !G->sound;
        snd_enable(G->sound);
    }
    switch (G->mode) {
    case M_TITLE:
        if (pressed(IN_QUIT)) {
            G->quit = true;
            return;
        }
        if (G->mode_t % 420 == 0)
            G->title_page ^= 1;
        if (pressed(IN_START | IN_FIRE) && G->mode_t > 20) {
            snd_fx(FX_COIN);
            new_game();
        }
        break;
    case M_PLAY:
        if (pressed(IN_QUIT)) {
            snd_tune(-1);
            snd_fx_stop(FX_BEAM);
            G->mode = M_TITLE;
            G->mode_t = 0;
            return;
        }
        if (pressed(IN_PAUSE))
            G->paused = !G->paused;
        if (G->paused)
            return;
        ++G->tick;
        update_stage();
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].state != E_NONE)
                update_enemy(i);
        update_player();
        collisions();
        break;
    case M_GAMEOVER:
        ++G->tick;
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].state != E_NONE)
                update_enemy(i);
        if (G->mode_t > 200 || (G->mode_t > 60 && pressed(IN_START | IN_FIRE))) {
            G->mode = M_RESULTS;
            G->mode_t = 0;
        }
        break;
    case M_RESULTS:
        if (G->mode_t > 300 || (G->mode_t > 60 && pressed(IN_START | IN_FIRE))) {
            memset(G->e, 0, sizeof G->e);
            G->entry_rank = hi_rank(G->score);
            G->mode_t = 0;
            if (G->entry_rank >= 0) {
                G->mode = M_ENTRY;
                G->entry_pos = 0;
                memcpy(G->entry_name, "A  ", 4);
                snd_tune(TUNE_HIGH);
            } else {
                G->mode = M_TITLE;
                G->title_page = 1;
            }
        }
        break;
    case M_ENTRY: {
        char *c = &G->entry_name[G->entry_pos];
        static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ. ";
        const char *at = strchr(letters, *c);
        int k = at ? (int)(at - letters) : 0;
        // The arrows step through the letters (repeating when held).
        unsigned dir = G->in & (IN_LEFT | IN_RIGHT | IN_UP | IN_DOWN);
        G->fire_hold = dir ? G->fire_hold + 1 : 0;
        bool step = G->fire_hold == 1 || (G->fire_hold > 20 && G->fire_hold % 6 == 0);
        if (step && (dir & (IN_LEFT | IN_DOWN)))
            k = (k + 27) % 28;
        else if (step && (dir & (IN_RIGHT | IN_UP)))
            k = (k + 1) % 28;
        *c = letters[k];
        bool next = pressed(IN_FIRE | IN_START);
        if (G->typed) {
            *c = G->typed;
            next = true;
        }
        if (pressed(IN_BACK) && G->entry_pos > 0) {
            *c = ' ';
            --G->entry_pos;
            break;
        }
        if (next || G->mode_t > 60 * 40) {
            snd_fx(FX_COIN);
            if (++G->entry_pos >= 3 || G->mode_t > 60 * 40) {
                int r = G->entry_rank;
                memmove(&G->hi[r + 1], &G->hi[r], (4 - r) * sizeof G->hi[0]);
                G->hi[r].score = G->score;
                memcpy(G->hi[r].name, G->entry_name, 4);
                bool saved = hi_save();
                G->mode = M_TITLE;
                G->mode_t = 0;
                G->title_page = 1;
                message(saved ? "" : "NO DRIVE: NOT SAVED", 240);
            } else {
                G->entry_name[G->entry_pos] = 'A';
            }
        }
        break;
    }
    }
}

// ---- Drawing the frame ---------------------------------------------------------------------------------

/// The stars.
static void draw_stars(void) {
    for (int i = 0; i < STARS; ++i) {
        const star_t *s = &G->star[i];
        if (((G->mode_t + s->phase) & 63) < 48)
            pset(s->x, (int)s->y, s->colour);
    }
}

/// A flagship's tractor beam: a widening cone of bands that ripple downward.
/// @param e the flagship
static void draw_beam(const enemy_t *e) {
    int len = beam_len(e), top = (int)e->y + 8;
    static const uint8_t col[4] = {RGB332(2, 4, 3), RGB332(4, 6, 3), C_WHITE, RGB332(1, 3, 3)};
    for (int r = 0; r < len; ++r) {
        int half = 4 + r * 22 / 90;
        if (((r + (int)(G->tick * 2)) >> 2) % 3 == 2)
            continue;  // gaps between the bands
        uint8_t c = col[((r - (int)G->tick) >> 3) & 3];
        for (int x = -half; x <= half; x += (r & 1) + 1)
            if ((int)e->x + x >= 0 && (int)e->x + x < PF_W)
                pset((int)e->x + x, top + r, c);
    }
}

/// An explosion: sparks flying out, fading from white through yellow to red.
/// @param b the explosion
static void draw_boom(const boom_t *b) {
    if (b->big == 2) {
        char s[8];
        snprintf(s, sizeof s, "%u", b->points);
        text((int)b->x - 3 * (int)strlen(s), (int)b->y - 3, s, (b->t >> 2) & 1 ? C_CYAN : C_WHITE);
        return;
    }
    bool big = b->big == 1;
    int t = b->t, n = big ? 24 : 12;
    float r = big ? 2.0f + t * 0.35f : 1.5f + t * 0.5f;
    static const uint8_t fade[6] = {C_WHITE, C_YELLOW, C_ORANGE, C_RED, RGB332(4, 0, 0), RGB332(2, 0, 0)};
    int life = big ? 90 : 26;
    uint8_t c = fade[t * 6 / (life + 1)];
    for (int k = 0; k < n; ++k) {
        float a = k * (256.0f / n) + (k & 1) * 6.0f + (big ? t * 0.7f : 0);
        float rr = r * (k % 3 == 0 ? 1.0f : 0.7f);
        int x = (int)(b->x + sin_a(a) * rr), y = (int)(b->y - cos_a(a) * rr);
        pset(x, y, c);
        if (t < life / 2) {
            pset(x + 1, y, c);
            pset(x, y + 1, c);
        }
    }
    if (t < (big ? 12 : 5))
        fill((int)b->x - 2, (int)b->y - 2, 5, 5, t & 1 ? C_WHITE : C_YELLOW);
    if (big && t < 40) {
        // Debris: the fighter's own colours tumbling outward.
        for (int k = 0; k < 6; ++k) {
            float a = k * 42.0f + t * 3.0f;
            int x = (int)(b->x + sin_a(a) * t * 0.5f), y = (int)(b->y - cos_a(a) * t * 0.4f);
            fill(x, y, 2, 2, k & 1 ? C_BLUE : C_RED);
        }
    }
}

/// A stage badge: a pennant on a pole, coloured and sized by what it counts.
/// @param x left
/// @param y bottom
/// @param kind 0 for 1, 1 for 5, 2 for 10, 3 for 20, 4 for 30, 5 for 50
/// @return its width
static int draw_badge(int x, int y, int kind) {
    static const uint8_t flag[6] = {C_RED, C_YELLOW, C_CYAN, C_PURPLE, C_GREEN, C_ORANGE};
    static const uint8_t trim[6] = {C_WHITE, C_RED, C_WHITE, C_YELLOW, C_YELLOW, C_WHITE};
    int h = kind == 0 ? 10 : 14, w = kind == 0 ? 4 : 6;
    fill(x, y - h, 1, h, C_GREY);
    for (int r = 0; r < h / 2 + 1; ++r) {
        int len = w - (r > h / 4 ? r - h / 4 : h / 4 - r) * w / (h / 4 + 1);
        fill(x + 1, y - h + r, len, 1, r == h / 4 ? trim[kind] : flag[kind]);
    }
    return w + 2;
}

/// The panel to the right: scores, reserve fighters, stage badges.
static void draw_panel(void) {
    fill(PF_W, 0, SW - PF_W, SH, C_BLACK);
    fill(PF_W, 0, 1, SH, C_DKBLUE);
    char s[16];
    text(PANEL_X, 6, "HIGH SCORE", C_RED);
    uint32_t hi = G->hi[0].score > G->score ? G->hi[0].score : G->score;
    snprintf(s, sizeof s, "%7lu", (unsigned long)hi);
    text(PANEL_X + 12, 16, s, C_WHITE);
    if (G->mode != M_PLAY || (G->tick >> 5) & 1)
        text(PANEL_X, 36, "1UP", C_RED);
    snprintf(s, sizeof s, "%7lu", (unsigned long)G->score);
    text(PANEL_X + 12, 46, s, C_WHITE);
    if (G->mode == M_TITLE)
        return;
    text(PANEL_X, 70, "STAGE", C_CYAN);
    snprintf(s, sizeof s, "%d", G->stage);
    text(PANEL_X + 36, 70, s, C_WHITE);
    // Fighters in reserve.
    for (int k = 0; k < G->lives && k < 8; ++k)
        sprite_at(IMG_SHIP, PANEL_X + 8 + (k % 4) * 18, 96 + (k / 4) * 18, 0, true);
    // Badges for the stages: 50s, 30s, 20s, 10s, 5s, then 1s.
    static const int value[6] = {50, 30, 20, 10, 5, 1}, kind[6] = {5, 4, 3, 2, 1, 0};
    int n = G->stage, x = PANEL_X, y = 150;
    for (int v = 0; v < 6; ++v)
        while (n >= value[v]) {
            n -= value[v];
            if (x > SW - 8) {
                x = PANEL_X;
                y += 16;
            }
            if (y < SH)
                x += draw_badge(x, y, kind[v]);
        }
    if (!G->sound)
        text(PANEL_X, SH - 12, "SOUND OFF", C_GREY);
}

/// The title screen's pages: what each alien is worth, or the high scores.
static void draw_title(void) {
    text_s((PF_W - 5 * 24) / 2 + 2, 20, "SWARM", C_ORANGE, 4);
    text_s((PF_W - 5 * 24) / 2, 18, "SWARM", C_YELLOW, 4);
    if (G->title_page == 0) {
        text_mid(66, "- SCORE ADVANCE TABLE -", C_CYAN);
        text(70, 80, "FORMATION  DIVING", C_GREY);
        static const struct { int img; const char *a, *b; } rows[] = {
            {IMG_BOSS, "150", "400-1600"}, {IMG_MOTH, " 80", "160"}, {IMG_DRONE, " 50", "100"},
            {IMG_CAPTIVE, "500", "1000"},
        };
        for (int i = 0; i < 4; ++i) {
            int y = 96 + i * 20;
            sprite_at(rows[i].img + ((G->mode_t >> 4) & (i < 3)), 48, y + 3, 0, false);
            text(76, y, rows[i].a, C_WHITE);
            text(136, y, rows[i].b, C_WHITE);
        }
    } else {
        text_mid(66, "- THE TOP 5 -", C_CYAN);
        text(46, 80, "RANK  SCORE   NAME", C_GREY);
        for (int i = 0; i < 5; ++i) {
            char s[40];
            snprintf(s, sizeof s, "%d%s  %7lu   %.3s", i + 1, i == 0 ? "ST" : i == 1 ? "ND" : i == 2 ? "RD" : "TH",
                     (unsigned long)G->hi[i].score, G->hi[i].name);
            text(46, 96 + i * 14, s, i == 0 ? C_YELLOW : C_WHITE);
        }
    }
    if ((G->mode_t >> 5) & 1)
        text_mid(186, "PUSH ENTER OR FIRE", C_RED);
    text_mid(204, "ARROWS MOVE  SPACE FIRES", C_GREY);
    text_mid(214, "P PAUSE  M SOUND  ESC QUITS", C_GREY);
    text_mid(228, "ORIGINAL GAME FOR PICO 2 OS", RGB332(2, 2, 2));
}

/// Draws the whole frame in the back buffer.
static void draw(void) {
    memset(BACK, 0, SW * SH);
    draw_stars();
    if (G->mode == M_TITLE) {
        draw_title();
        if (G->msg_t && G->msg[0])
            text_mid(170, G->msg, C_RED);
        draw_panel();
        return;
    }
    // Beams under the aliens, then the aliens.
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (G->e[i].state == E_BEAM)
            draw_beam(&G->e[i]);
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        if (e->state == E_NONE || e->state == E_WAIT)
            continue;
        sprite_at(enemy_img(e), e->x, e->y, e->ang, false);
    }
    for (int s = 0; s < MAX_ESHOTS; ++s)
        if (G->es[s].on) {
            int x = (int)G->es[s].x, y = (int)G->es[s].y;
            fill(x - 1, y - 2, 3, 4, C_RED);
            fill(x, y - 3, 1, 6, C_WHITE);
        }
    for (int s = 0; s < MAX_PSHOTS; ++s)
        if (G->ps[s].on) {
            int x = (int)G->ps[s].x, y = (int)G->ps[s].y;
            fill(x, y - 4, 1, 8, C_YELLOW);
            fill(x - 1, y - 1, 3, 4, C_WHITE);
        }
    // The fighter.
    int ps = G->pstate;
    if (ps == P_ALIVE || ps == P_FREE || ps == P_CAPTURED || (ps == P_READY && G->pt >= 0)) {
        uint8_t img = ps == P_CAPTURED && G->py < PLAYER_Y - 40 ? IMG_CAPTIVE : IMG_SHIP;
        sprite_at(img, G->px, G->py, G->pang, false);
        if (G->dual)
            sprite_at(IMG_SHIP, G->px + 16, G->py, 0, false);
    }
    for (int i = 0; i < MAX_BOOMS; ++i)
        if (G->boom[i].t >= 0)
            draw_boom(&G->boom[i]);
    // Words.
    char s[40];
    if (G->mode == M_PLAY) {
        if (G->phase == PH_INTRO) {
            if (G->stage == 1 && G->tick < 400 && G->phase_t < 150) {
                text_mid(110, "PLAYER 1", C_CYAN);
            } else {
                if (G->challenge)
                    snprintf(s, sizeof s, "CHALLENGING STAGE");
                else
                    snprintf(s, sizeof s, "STAGE %d", G->stage);
                text_mid(110, s, C_CYAN);
            }
        }
        if (G->phase == PH_BONUS) {
            snprintf(s, sizeof s, "NUMBER OF HITS %d", G->chal_hits);
            if (G->phase_t > 40)
                text_mid(100, s, C_CYAN);
            if (G->phase_t > 100) {
                if (G->chal_hits == 40) {
                    text_mid(116, "PERFECT !", (G->tick >> 3) & 1 ? C_RED : C_YELLOW);
                    text_mid(130, "SPECIAL BONUS 10000", C_YELLOW);
                } else {
                    snprintf(s, sizeof s, "BONUS %d", G->chal_hits * 100);
                    text_mid(120, s, C_YELLOW);
                }
            }
        }
        if (G->pstate == P_READY && G->pt >= 0 && G->phase != PH_INTRO)
            text_mid(120, "READY", C_RED);
        if (G->msg_t && G->msg[0])
            text_mid(140, G->msg, C_RED);
        if (G->paused)
            text_mid(120, "PAUSED", (G->mode_t >> 4) & 1 ? C_WHITE : C_YELLOW);
    } else if (G->mode == M_GAMEOVER) {
        text_mid(116, "GAME OVER", C_RED);
    } else if (G->mode == M_RESULTS) {
        text_mid(70, "- RESULTS -", C_RED);
        snprintf(s, sizeof s, "SHOTS FIRED     %6lu", (unsigned long)G->shots_fired);
        text_mid(96, s, C_YELLOW);
        snprintf(s, sizeof s, "NUMBER OF HITS  %6lu", (unsigned long)G->hits);
        text_mid(112, s, C_YELLOW);
        unsigned pct = G->shots_fired ? (unsigned)(G->hits * 1000ull / G->shots_fired) : 0;
        if (pct > 1000)
            pct = 1000;
        snprintf(s, sizeof s, "HIT-MISS RATIO %3u.%u %%", pct / 10, pct % 10);
        text_mid(128, s, C_WHITE);
    } else if (G->mode == M_ENTRY) {
        text_mid(60, "ENTER YOUR INITIALS !", C_RED);
        snprintf(s, sizeof s, "SCORE %lu", (unsigned long)G->score);
        text_mid(80, s, C_YELLOW);
        for (int k = 0; k < 3; ++k) {
            char ch[2] = {G->entry_name[k], 0};
            int x = PF_W / 2 - 30 + k * 24;
            if (k <= G->entry_pos)
                text_s(x, 104, ch, k == G->entry_pos && (G->mode_t >> 3) & 1 ? C_WHITE : C_CYAN, 3);
            fill(x - 1, 128, 17, 1, k == G->entry_pos ? C_RED : C_GREY);
        }
        text_mid(150, "TYPE THEM, OR ARROWS AND FIRE", C_GREY);
    }
    draw_panel();
}

// ---- Input -----------------------------------------------------------------------------------------------

/// Reads the keyboard and the first gamepad into G->in (and a typed letter
/// into G->typed); throws away the characters the console would have got.
static void read_input(void) {
    G->in_prev = G->in;
    unsigned in = 0, letter_keys = 0;  // letter_keys: WASD and Z X, which also type initials
    uint8_t keys[6], mods = 0;
    char typed = 0;
    static uint8_t last[6];  // last frame's keys, for letters newly typed
    if (usb_keyboard_held_keys(keys, &mods)) {
        for (int i = 0; i < 6; ++i) {
            uint8_t k = keys[i];
            switch (k) {
            case HID_KEY_ARROW_LEFT: in |= IN_LEFT; break;
            case HID_KEY_ARROW_RIGHT: in |= IN_RIGHT; break;
            case HID_KEY_ARROW_UP: in |= IN_UP; break;
            case HID_KEY_ARROW_DOWN: in |= IN_DOWN; break;
            case HID_KEY_A: letter_keys |= IN_LEFT; break;
            case HID_KEY_D: letter_keys |= IN_RIGHT; break;
            case HID_KEY_W: letter_keys |= IN_UP; break;
            case HID_KEY_S: letter_keys |= IN_DOWN; break;
            case HID_KEY_Z: case HID_KEY_X: letter_keys |= IN_FIRE; break;
            case HID_KEY_SPACE: in |= IN_FIRE; break;
            case HID_KEY_ENTER: case HID_KEY_KEYPAD_ENTER: case HID_KEY_1: in |= IN_START; break;
            case HID_KEY_P: in |= IN_PAUSE; break;
            case HID_KEY_M: in |= IN_SOUND; break;
            case HID_KEY_ESCAPE: in |= IN_QUIT; break;
            case HID_KEY_F10: in |= IN_EXIT; break;
            case HID_KEY_BACKSPACE: in |= IN_BACK; break;
            }
            if (k >= HID_KEY_A && k <= HID_KEY_Z && !memchr(last, k, 6))
                typed = (char)('A' + k - HID_KEY_A);
        }
        memcpy(last, keys, 6);
        if (mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
            in |= IN_FIRE;
    }
    uint8_t x, y;
    uint32_t b;
    if (usb_gamepad_read_n(0, &x, &y, &b)) {
        if (x < 64) in |= IN_LEFT;
        if (x > 191) in |= IN_RIGHT;
        if (y < 64) in |= IN_UP;
        if (y > 191) in |= IN_DOWN;
        if (b & 0x0f) in |= IN_FIRE;
        if (b & 0xf80) in |= G->mode == M_PLAY ? IN_PAUSE : IN_START;
    }
    // While initials are entered, letters are letters.
    G->typed = G->mode == M_ENTRY ? typed : 0;
    if (G->mode != M_ENTRY)
        in |= letter_keys;
    G->in = in;
    while (getchar_timeout_us(0) >= 0) {
    }
}

// ---- Running ---------------------------------------------------------------------------------------------

/// Sets the world up in the framebuffer's free part (sprites, stars,
/// scores, sound).
/// @return false if it doesn't fit (a build mistake)
static bool world_start(void) {
    _Static_assert(sizeof(world_t) <= sizeof dvi_framebuf - 2 * SW * SH - 8, "world_t doesn't fit");
    _Static_assert(sizeof(((world_t *)0)->snd) >= 900, "room for the sound");
    G = (world_t *)(((uintptr_t)(dvi_framebuf + 2 * SW * SH) + 7) & ~(uintptr_t)7);
    memset(G, 0, sizeof *G);
    if (snd_state_size() > sizeof G->snd)
        return false;
    memcpy(G->font, font_rows, sizeof G->font);
    for (int i = 0; i < 256; ++i)
        G->sin_t[i] = sinf(i * (6.2831853f / 256.0f));
    G->rng = 0x2545f491u ^ time_us_32();
    if (!G->rng)
        G->rng = 1;
    // The sprites are worked out in the second buffer, free until the first frame.
    make_pictures(BACK);
    for (int i = 0; i < STARS; ++i) {
        static const uint8_t cols[] = {C_WHITE, C_YELLOW, C_CYAN, C_RED, C_PINK, RGB332(3, 3, 3), C_BLUE, C_GREEN};
        G->star[i] = (star_t){(float)rnd_n(SH), 0.3f + rnd_n(100) / 70.0f, (uint8_t)rnd_n(PF_W),
                              cols[rnd_n(sizeof cols)], (uint8_t)rnd_n(64)};
    }
    for (int i = 0; i < MAX_BOOMS; ++i)
        G->boom[i].t = -1;
    G->captor = -1;
    G->sound = true;
    hi_load();
    snd_start(G->snd);
    G->mode = M_TITLE;
    return true;
}

/// Waits for the vertical sync, then shows the frame just drawn.
static void present(void) {
    uint32_t f = dvi_frames;
    while (dvi_frames == f)
        tight_loop_contents();
    memcpy(FRONT, BACK, SW * SH);
}

/// The game, until Esc on the title screen (or F10).
static void play(void) {
    scr_begin();
    stdio_set_driver_enabled(&console_stdio_driver, false);
    console_freeze(true);
    mouse_pointer(MOUSE_POINTER_OFF);
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    dvi_double_top = 0;
    dvi_double_rows = SH;
    dvi_pixel_double = true;
    dvi_line_double = true;
    if (world_start()) {
        audio_set_source(snd_source);
        while (!G->quit) {
            read_input();
            update();
            update_things();
            draw();
            present();
        }
        audio_set_source(NULL);
        sleep_ms(3);  // core 1 may still be in snd_source
    }
    G = NULL;
    dvi_line_double = false;
    dvi_pixel_double = false;
    memset(dvi_framebuf, 0, sizeof dvi_framebuf);
    console_freeze(false);
    stdio_set_driver_enabled(&console_stdio_driver, true);
    scr_end();
    while (getchar_timeout_us(0) >= 0) {
    }
}

/// Hands the built-in sprites to spr_save.
/// @param ctx unused
/// @param frame which
/// @param px set to its colours
/// @param on set to which are drawn
static void spr_get(void *ctx, int frame, uint8_t *px, uint8_t *on) {
    (void)ctx;
    art_get(frame, px);
    for (int i = 0; i < SPR * SPR; ++i)
        on[i] = px[i] != 0;
}

void swarm_command(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "sprites")) {
        const char *path = argc > 2 ? argv[2] : "SWARM.SPR";
        spr_info_t info = {SPR, SPR, ART_COUNT};
        const char *err = NULL;
        if (spr_save(path, &info, spr_get, NULL, &err))
            printf("%s: the game's %d sprites (16x16). Edit them with: sprite %s\n"
                   "Saved as %s at the top of the drive, the game uses them.\n",
                   path, ART_COUNT, path, SPR_FILE + 1);
        else
            printf("swarm: %s: %s\n", path, err ? err : "can't write it");
        return;
    }
    if (argc > 1) {
        printf("usage: swarm              play (Esc on the title screen to leave)\n"
               "       swarm sprites [file.spr]  write the game's sprites for the sprite editor\n");
        return;
    }
    play();
}
