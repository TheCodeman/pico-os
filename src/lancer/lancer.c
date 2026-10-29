/**
 * @file lancer.c
 * @brief Lancer, a side-scrolling space shooter (see lancer.h): the game, its stages, its drawing, and the command.
 *
 * The screen: while the game runs the scanout doubles pixels both ways, so
 * the picture is 320x240 (dvi_framebuf's first quarter). Each frame is
 * drawn in a second 320x240 buffer straight after it, also in SRAM, and
 * copied over when the vertical sync starts (dvi_frames), so nothing
 * flickers or tears. The rest of the framebuffer holds everything else the
 * game needs (world_t: the pictures, the stage's map, the enemies, the
 * sound's state), so the game keeps no SRAM of its own once it ends.
 *
 * The play area is the top 224 rows; the 16 below show the beam's charge,
 * the ships in reserve, the power-ups and the scores.
 *
 * How it plays: the view scrolls right through a stage of rock, metal or
 * living walls (a map of 8x8 tiles, 28 rows high, built from a list of
 * stretches of ceiling and floor heights when the stage starts). Touching a
 * wall, an enemy or a shot loses the ship. Fire tapped shoots; held, it
 * charges the lance, a beam that goes through what it kills, in three
 * sizes. A hauler shot down leaves a capsule: the first coloured one brings
 * the pod, which locks on to the front or back of the ship, takes enemy
 * shots, hurts what it touches, and fires the colour's weapon (red spread,
 * blue lasers that bounce off walls, yellow shots that crawl along them).
 * The pod key launches it and calls it back. S capsules speed the ship up
 * and M ones add homing missiles. Each stage ends with a boss; after the
 * third, the stages start over, harder.
 *
 * Everything is kept in world coordinates (x along the stage from its
 * start); the ship, the pod and the shots are moved on with the view each
 * frame so they keep their place on the screen. Headings are 0-256 a turn,
 * 0 right and 64 down.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "lancer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "ff.h"
#include "lancer_art.h"
#include "lancer_snd.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "spr.h"
#include "tusb.h"
#include "usb_keyboard.h"

// ---- Sizes and places ---------------------------------------------------------------------------------

#define SW 320          ///< the picture's width
#define SH 240          ///< and height
#define PH 224          ///< the play area's height (the panel is below it)
#define TILE 8          ///< map tiles are TILE x TILE
#define MAP_H 28        ///< map rows (PH / TILE)
#define MAP_W 400       ///< the most columns a stage has
#define SPR 16          ///< sprites are SPR x SPR
#define SHIP_X 56       ///< where a ship starts across the screen

#define MAX_ENEMIES 64  ///< enemies (and a boss's parts) at once
#define MAX_PSHOTS 40   ///< the ship's and the pod's shots
#define MAX_ESHOTS 96   ///< the enemies' shots
#define MAX_BOOMS 40    ///< explosions and score pop-ups
#define MAX_CAPS 4      ///< capsules
#define STARS 70        ///< the starfield
#define LOOKS 41        ///< tile pictures (0: none)
#define CHARGE_MAX 96   ///< a full charge

#define HI_FILE "/LANCER.HI"    ///< the high scores
#define SPR_FILE "/LANCER.SPR"  ///< sprites used in place of the built-in ones

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
    C_GREEN = RGB332(0, 6, 0),
    C_ORANGE = RGB332(7, 4, 0),
    C_PURPLE = RGB332(5, 1, 3),
    C_GREY = RGB332(4, 4, 2),
    C_DKGREY = RGB332(2, 2, 1),
    C_PINK = RGB332(7, 3, 2),
    C_HUD = RGB332(0, 0, 1),
};

// ---- Pictures -----------------------------------------------------------------------------------------

/// The pictures, made from the art when the game starts.
enum {
    IMG_SHIP, IMG_SHIP_B,
    IMG_ORB, IMG_ORB_B, IMG_ORB_BLUE, IMG_ORB_BLUE_B, IMG_ORB_YEL, IMG_ORB_YEL_B,  ///< by colour, 2 frames
    IMG_DART, IMG_DART_B, IMG_SWOOP, IMG_SWOOP_B,
    IMG_TURRET, IMG_TURRET_C,                                   ///< on the floor, on the ceiling
    IMG_HOPPER, IMG_HOPPER_B, IMG_HOPPER_C, IMG_HOPPER_CB,
    IMG_HAULER, IMG_HAULER_B, IMG_MINE, IMG_MINE_B, IMG_COIL, IMG_COIL_HEAD,
    IMG_HATCH, IMG_HATCH_OPEN, IMG_HATCH_C, IMG_HATCH_OPEN_C,
    IMG_CAP, IMG_CAP_BLUE, IMG_CAP_YEL, IMG_CAP_S, IMG_CAP_M,   ///< capsules: red, blue, yellow, speed, missile
    IMG_PLATE,
    IMG_COUNT
};

/// Where a picture comes from: its art, the colour for 'X' and 'x', and
/// whether it is turned upside down.
typedef struct {
    uint8_t art;   ///< ART_ value
    uint8_t tint;  ///< 0 red, 1 blue, 2 yellow, 3 green, 4 purple
    bool flip;     ///< upside down (for the ceiling)
} img_src_t;

/// Each picture's source.
static const img_src_t img_src[IMG_COUNT] = {
    [IMG_SHIP] = {ART_SHIP, 0, false},          [IMG_SHIP_B] = {ART_SHIP_B, 0, false},
    [IMG_ORB] = {ART_ORB_A, 0, false},          [IMG_ORB_B] = {ART_ORB_B, 0, false},
    [IMG_ORB_BLUE] = {ART_ORB_A, 1, false},     [IMG_ORB_BLUE_B] = {ART_ORB_B, 1, false},
    [IMG_ORB_YEL] = {ART_ORB_A, 2, false},      [IMG_ORB_YEL_B] = {ART_ORB_B, 2, false},
    [IMG_DART] = {ART_DART_A, 0, false},        [IMG_DART_B] = {ART_DART_B, 0, false},
    [IMG_SWOOP] = {ART_SWOOP_A, 0, false},      [IMG_SWOOP_B] = {ART_SWOOP_B, 0, false},
    [IMG_TURRET] = {ART_TURRET, 0, false},      [IMG_TURRET_C] = {ART_TURRET, 0, true},
    [IMG_HOPPER] = {ART_HOPPER_A, 0, false},    [IMG_HOPPER_B] = {ART_HOPPER_B, 0, false},
    [IMG_HOPPER_C] = {ART_HOPPER_A, 0, true},   [IMG_HOPPER_CB] = {ART_HOPPER_B, 0, true},
    [IMG_HAULER] = {ART_HAULER_A, 0, false},    [IMG_HAULER_B] = {ART_HAULER_B, 0, false},
    [IMG_MINE] = {ART_MINE_A, 0, false},        [IMG_MINE_B] = {ART_MINE_B, 0, false},
    [IMG_COIL] = {ART_COIL, 0, false},          [IMG_COIL_HEAD] = {ART_COIL_HEAD, 0, false},
    [IMG_HATCH] = {ART_HATCH, 0, false},        [IMG_HATCH_OPEN] = {ART_HATCH_OPEN, 0, false},
    [IMG_HATCH_C] = {ART_HATCH, 0, true},       [IMG_HATCH_OPEN_C] = {ART_HATCH_OPEN, 0, true},
    [IMG_CAP] = {ART_CAPSULE, 0, false},        [IMG_CAP_BLUE] = {ART_CAPSULE, 1, false},
    [IMG_CAP_YEL] = {ART_CAPSULE, 2, false},    [IMG_CAP_S] = {ART_CAPSULE, 3, false},
    [IMG_CAP_M] = {ART_CAPSULE, 4, false},      [IMG_PLATE] = {ART_PLATE, 0, false},
};

// ---- The map --------------------------------------------------------------------------------------------

/// Tile shapes: full, or half a tile cut along a diagonal (floor slopes
/// going up or down to the right, and the ceiling's).
enum { S_EMPTY, S_FULL, S_UP, S_DOWN, S_CUP, S_CDOWN };

/// Which sides of a full tile are open (and drawn lit).
enum { X_UP = 1, X_DOWN = 2, X_LEFT = 4, X_RIGHT = 8 };

/// One stretch of a stage's walls: for `cols` columns the ceiling and the
/// floor head for these heights (in tiles), a tile a column, or at once.
typedef struct {
    uint8_t cols;   ///< how long (0 ends the list)
    uint8_t ceil;   ///< the ceiling's height, tiles down from the top
    uint8_t floor;  ///< the floor's, tiles up from the bottom
    uint8_t flags;  ///< TF_ bits
} terr_t;

/// Stretch flags.
enum {
    TF_STEP = 1,   ///< the heights change at once (walls), not in slopes
    TF_ROUGH = 2,  ///< a tile up or down here and there
};

/// A solid block in the open (a pillar, a boulder).
typedef struct {
    uint16_t col;     ///< left column (0 ends the list)
    uint8_t row;      ///< top row
    uint8_t w, h;     ///< size in tiles
} block_t;

// ---- The world ----------------------------------------------------------------------------------------

/// Kinds of enemy (and of boss part).
enum {
    K_DART,     ///< flies in waves along a curve
    K_SWOOP,    ///< flies in, then turns at the ship (or comes from behind)
    K_TURRET,   ///< a gun dome on the floor or the ceiling
    K_HOPPER,   ///< walks along the floor or the ceiling and hops
    K_HAULER,   ///< slow; leaves a capsule
    K_MINE,     ///< drifts at the ship; bursts into shots
    K_COIL,     ///< a chain of balls following its head along a wave
    K_HATCH,    ///< opens now and then to let out spores
    K_SPORE,    ///< a small seeker from a hatch
    K_PLATE,    ///< an armour plate going round the first boss's core
    K_CORE,     ///< a boss's heart: shooting it to nothing ends the stage
    K_BTURRET,  ///< a gun on the second boss
    K_SEG,      ///< a part of the third boss's body (armoured)
    K_TAIL,     ///< the third boss's tail
    K_COUNT
};

/// What an enemy kind is like.
typedef struct {
    int16_t hp;      ///< hits it takes
    uint16_t score;  ///< points for it
    uint8_t hw, hh;  ///< half its size, for hits
} kind_t;

/// The kinds.
static const kind_t kinds[K_COUNT] = {
    [K_DART] = {1, 100, 6, 4},     [K_SWOOP] = {1, 150, 6, 5},   [K_TURRET] = {3, 300, 6, 5},
    [K_HOPPER] = {4, 400, 6, 5},   [K_HAULER] = {6, 500, 7, 6},  [K_MINE] = {2, 200, 5, 5},
    [K_COIL] = {2, 200, 6, 6},     [K_HATCH] = {10, 800, 7, 5},  [K_SPORE] = {1, 50, 3, 3},
    [K_PLATE] = {20, 300, 6, 6},    [K_CORE] = {80, 20000, 11, 11}, [K_BTURRET] = {16, 1000, 7, 6},
    [K_SEG] = {1, 0, 6, 6},        [K_TAIL] = {30, 3000, 6, 6},
};

/// An enemy.
typedef struct {
    float x, y;        ///< centre (world)
    float dx, dy;      ///< speed (pixels a frame)
    float bx, by;      ///< where its path starts
    int16_t t;         ///< frames since it appeared (below 0: still to come)
    int16_t hp;        ///< hits left
    int16_t fire_t;    ///< frames to its next shot
    uint16_t lance;    ///< the last lance that hit it (a lance hits once)
    uint8_t kind;      ///< K_ value
    bool on;           ///< in use
    bool armour;       ///< shots bounce off it (for now)
    bool hidden;       ///< not on its path yet (a coil's ball)
    int8_t side;       ///< 1 on the floor, -1 on the ceiling
    uint8_t flash;     ///< frames to draw it white (just hit)
    uint8_t p;         ///< what it was made with (path, capsule)
    uint8_t k;         ///< which in its group
    uint8_t orb_cool;  ///< frames before the pod hurts it again
} enemy_t;

/// The ship's shots.
enum {
    PS_PELLET,   ///< the plain shot
    PS_LANCE,    ///< the charged beam (lvl 1-3)
    PS_SPREAD,   ///< the red pod's
    PS_PRISM,    ///< the blue pod's: bounces off walls
    PS_CRAWL,    ///< the yellow pod's: runs along walls
    PS_MISSILE,  ///< homes in
    PS_ORBSHOT,  ///< a pod's on its own
};

/// A shot of the ship's.
typedef struct {
    float x, y, dx, dy;  ///< where and how fast
    int16_t t;           ///< frames in flight
    uint16_t serial;     ///< a lance's number
    uint8_t kind;        ///< PS_ value
    bool on;             ///< in flight
    uint8_t lvl;         ///< a lance's size
    int8_t side;         ///< a crawler's wall: 1 below, -1 above, 0 not on one yet
    int8_t dir;          ///< a crawler's way along it
    uint8_t bounces;     ///< a laser's bounces so far
} pshot_t;

/// An enemy shot.
typedef struct {
    float x, y, dx, dy;  ///< where and how fast
    bool on;             ///< in flight
} eshot_t;

/// An explosion, or points shown where something was hit.
typedef struct {
    float x, y;        ///< where (world)
    int16_t t;         ///< frames so far (-1: free)
    uint8_t big;       ///< 0 small, 1 big, 2 points
    uint16_t points;   ///< points shown
} boom_t;

/// Capsule kinds.
enum { CAP_ORB, CAP_SPEED, CAP_MISSILE };

/// A capsule.
typedef struct {
    float x, y;  ///< where (world)
    int16_t t;   ///< frames since it was left
    uint8_t kind;  ///< CAP_ value
    bool on;       ///< there
} cap_t;

/// A star.
typedef struct {
    float x, speed;   ///< column and how fast it goes by
    uint8_t y;        ///< row
    uint8_t colour;   ///< RGB332
} star_t;

/// A high score.
typedef struct {
    uint32_t score;  ///< the score
    char name[4];    ///< three letters
} hiscore_t;

/// The bosses.
enum { BOSS_NONE, BOSS_ORRERY, BOSS_BASTION, BOSS_WYRM };

/// A boss.
typedef struct {
    int type;        ///< BOSS_ value
    int t;           ///< frames since it came
    int core;        ///< its core's enemy (-1: none)
    int dying;       ///< frames of its end left (0: alive)
    float cx, cy;    ///< the core's place (world)
    float phase;     ///< the third boss: how far along its path
    float fort_x;    ///< the second boss: its body's left edge (world)
    float fort_y;    ///< and its middle row
    bool beam_warn;  ///< the second boss's laser is about to fire
    bool beam;       ///< and firing
} boss_t;

/// A stage's event: something to bring on when the view reaches it.
typedef struct {
    uint16_t x;     ///< where along the stage (0 ends the list)
    uint8_t kind;   ///< K_ value, or EV_CHECK
    uint8_t n;      ///< how many
    uint8_t y;      ///< the row it flies at
    uint8_t p;      ///< its path, its side (0 floor, 1 ceiling) or a hauler's capsule
} event_t;

/// An event that marks where a lost ship starts again.
#define EV_CHECK 200

/// A stage.
typedef struct {
    const char *name;      ///< shown at its start
    uint8_t theme;         ///< 0 rock, 1 metal, 2 living
    float scroll;          ///< pixels a frame the view moves
    int tune;              ///< TUNE_ value
    int boss;              ///< BOSS_ value
    const terr_t *terr;    ///< its walls
    const block_t *blocks; ///< the blocks in it
    const event_t *ev;     ///< its events
} stage_t;

/// The pod.
enum {
    O_NONE,     ///< not had yet
    O_COMING,   ///< flying in from the left
    O_FRONT,    ///< on the ship's nose
    O_BACK,     ///< on its tail
    O_OUT,      ///< launched
    O_FREE,     ///< on its own: comes level with the ship, locks on if touched
    O_RECALL,   ///< called back
};

/// The ship.
enum {
    P_ENTER,  ///< flying in from the left
    P_ALIVE,  ///< flying
    P_DYING,  ///< blowing up
    P_OVER,   ///< none left
};

/// What the game is showing.
enum { M_TITLE, M_PLAY, M_GAMEOVER, M_ENTRY };

/// The parts of a stage.
enum { PH_INTRO, PH_RUN, PH_WARN, PH_BOSS, PH_CLEAR };

/// Everything (placed in the framebuffer's free part while the game runs).
typedef struct {
    uint8_t img[IMG_COUNT][SPR * SPR];       ///< the pictures (0: see-through)
    uint8_t tile[LOOKS][TILE * TILE];        ///< the stage's tile pictures
    uint8_t map[MAP_H][MAP_W];               ///< the stage (tile looks, 0: open)
    uint8_t font[sizeof font_chars - 1][7];  ///< the font (copied from flash)
    float sin_t[256];                        ///< sine of a heading
    enemy_t e[MAX_ENEMIES];                  ///< the enemies
    pshot_t ps[MAX_PSHOTS];                  ///< the ship's shots
    eshot_t es[MAX_ESHOTS];                  ///< the enemies' shots
    boom_t boom[MAX_BOOMS];                  ///< explosions
    cap_t cap[MAX_CAPS];                     ///< capsules
    star_t star[STARS];                      ///< the starfield
    hiscore_t hi[5];                         ///< the high scores
    boss_t boss;                             ///< the stage's boss
    uint8_t snd[1024];                       ///< the synthesizer's state

    uint32_t tick;        ///< frames since the game started
    uint32_t rng;         ///< random numbers
    int mode;             ///< M_ value
    int mode_t;           ///< frames in the mode
    bool paused;          ///< P
    bool quit;            ///< back to the shell
    bool sound;           ///< sound on
    bool music;           ///< music on

    // Input
    unsigned in, in_prev;  ///< IN_ bits held now and last frame
    char typed;            ///< a letter pressed this frame (initials), or 0
    int key_hold;          ///< frames an arrow has been held (initials)

    // Ship
    float px, py;          ///< its centre (world)
    int pstate, pt;        ///< P_ value and frames in it
    int invuln;            ///< frames it can't be hit (after starting again)
    int speed_lvl;         ///< S capsules taken (0-3)
    bool missiles;         ///< has missiles
    int missile_cool;      ///< frames before the next pair
    float charge;          ///< the beam's charge (0-CHARGE_MAX)
    int fire_held;         ///< frames fire has been held
    uint16_t lance_serial; ///< numbers the lances
    int lives;             ///< ships in reserve
    uint32_t score;        ///< the score
    uint32_t next_extra;   ///< the score that gives the next extra ship
    int lances;            ///< lances fired (for the test)

    // Pod
    int orb;               ///< O_ value
    float ox, oy;          ///< its centre (world)
    float odx;             ///< its speed when launched
    int ocol;              ///< its colour: 0 red, 1 blue, 2 yellow
    int olvl;              ///< its weapon's level (1-2)

    // Stage
    int stage;             ///< 1-3
    int loop;              ///< times round all three (harder each time)
    const stage_t *st;     ///< the stage
    int map_w;             ///< its columns
    float cam;             ///< the view's left edge (world)
    float cam_end;         ///< where the view stops (the boss's arena)
    float scroll_now;      ///< how far the view moved this frame
    float check_cam;       ///< where a lost ship starts again
    int ev;                ///< the next event
    int phase, phase_t;    ///< PH_ value and frames in it
    char msg[40];          ///< a message in the middle
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
    IN_ORB = 2048, IN_MUSIC = 4096,
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
    return G->sin_t[(int)(a + 256.0f * 64 + 0.5f) & 255];
}

/// The cosine of a heading.
/// @param a the heading
/// @return its cosine
static float cos_a(float a) {
    return sin_a(a + 64);
}

/// The heading from (0,0) towards (dx, dy) (rows grow downwards).
/// @param dx across
/// @param dy down
/// @return 0-256, 0 right, 64 down
static float heading(float dx, float dy) {
    float a = atan2f(dy, dx) * (128.0f / 3.14159265f);
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

/// How much harder it is now: 1 on the first round, more after.
/// @return the factor
static float hard(void) {
    return 1.0f + 0.2f * (float)G->loop + 0.05f * (float)(G->stage - 1);
}

/// A column on the screen for a world one.
/// @param x world column
/// @return screen column
static int scr_x(float x) {
    return (int)floorf(x) - (int)floorf(G->cam);  // as the walls are drawn
}

// ---- The map --------------------------------------------------------------------------------------------

/// The shape of a tile look.
/// @param l look (1..LOOKS-1)
/// @return S_ value
static int look_shape(int l) {
    return l == 0 ? S_EMPTY : l <= 32 ? S_FULL : S_UP + (l - 33) / 2;
}

/// True if a pixel of a shape is solid.
/// @param shape S_ value
/// @param x 0-7 across the tile
/// @param y 0-7 down it
/// @return true if solid
static bool shape_px(int shape, int x, int y) {
    switch (shape) {
    case S_FULL: return true;
    case S_UP: return y >= 7 - x;
    case S_DOWN: return y >= x;
    case S_CUP: return y <= 7 - x;
    case S_CDOWN: return y <= x;
    default: return false;
    }
}

/// True if the wall is at a world pixel (outside the stage there is none).
/// @param fx column
/// @param fy row
/// @return true if solid
static bool solid(float fx, float fy) {
    int x = (int)floorf(fx), y = (int)floorf(fy);
    if (y < 0 || y >= PH || x < 0 || x >= G->map_w * TILE)
        return false;
    int l = G->map[y >> 3][x >> 3];
    return l && shape_px(look_shape(l), x & 7, y & 7);
}

/// The top of the floor below the middle of a column: the first open row
/// up from the bottom.
/// @param x world column
/// @return the row of the floor's top (PH if there is none)
static float floor_y(float x) {
    int y = PH - 1;
    while (y >= 0 && solid(x, (float)y))
        --y;
    return (float)(y + 1);
}

/// The bottom of the ceiling: the first open row down from the top.
/// @param x world column
/// @return the first open row (0 if there is no ceiling)
static float ceil_y(float x) {
    int y = 0;
    while (y < PH && solid(x, (float)y))
        ++y;
    return (float)y;
}

/// True if a box is clear of the walls (tested at its corners, edges and middle).
/// @param x centre column
/// @param y centre row
/// @param hw half its width
/// @param hh half its height
/// @return true if clear
static bool clear_box(float x, float y, float hw, float hh) {
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            if (solid(x + i * hw, y + j * hh))
                return false;
    return true;
}

/// Where a thing on the ground goes: the nearest column to `x` where the
/// floor (or the ceiling) is level under its whole width, and its row.
/// @param x the column asked for (world)
/// @param side 1 the floor, -1 the ceiling
/// @param y set to its centre row
/// @return its column
static float ground_spot(float x, int side, float *y) {
    for (int d = 0; d <= 64; d += 4)
        for (int s = -1; s <= 1; s += 2) {
            float cx = x + d * s;
            float g = side > 0 ? floor_y(cx) : ceil_y(cx);
            if (g > 8 && g < PH - 8 && (side > 0 ? floor_y(cx - 7) : ceil_y(cx - 7)) == g &&
                (side > 0 ? floor_y(cx + 7) : ceil_y(cx + 7)) == g) {
                *y = g - 8 * side;
                return cx;
            }
        }
    *y = side > 0 ? floor_y(x) - 8 : ceil_y(x) + 8;
    return x;
}

/// An open row near the middle of a column, for a ship starting there.
/// @param x world column
/// @return the row
static float open_row(float x) {
    for (int d = 0; d < 100; d += 4) {
        if (clear_box(x, PH / 2.0f + d, 12, 10))
            return PH / 2.0f + d;
        if (clear_box(x, PH / 2.0f - d, 12, 10))
            return PH / 2.0f - d;
    }
    return PH / 2.0f;
}

/// A pseudo-random byte for a tile pixel.
/// @param x across
/// @param y down
/// @param v the tile's variant
/// @return 0-255
static unsigned hash3(int x, int y, int v) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)v * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 255;
}

/// A pixel of a stage's wall texture (never 0, which is see-through).
/// @param theme 0 rock, 1 metal, 2 living
/// @param v the variant (0-1)
/// @param x 0-7
/// @param y 0-7
/// @return RGB332
static uint8_t texture(int theme, int v, int x, int y) {
    unsigned h = hash3(x, y, v + theme * 2);
    switch (theme) {
    case 0: {  // rock: browns, speckled, darker cracks
        static const uint8_t rock[8] = {RGB332(3, 2, 0), RGB332(3, 2, 0), RGB332(4, 2, 0), RGB332(3, 1, 0),
                                        RGB332(4, 3, 1), RGB332(2, 1, 0), RGB332(3, 2, 0), RGB332(5, 3, 1)};
        if ((x + 2 * y + v * 3) % 7 == 0 && h < 120)
            return RGB332(1, 1, 0);
        return rock[h & 7];
    }
    case 1: {  // metal: panels with bevelled edges and rivets
        if (x == 0 || y == 0)
            return RGB332(4, 4, 2);
        if (x == 7 || y == 7)
            return RGB332(1, 1, 1);
        if (v == 0 && (x == 2 || x == 5) && (y == 2 || y == 5))
            return RGB332(5, 5, 2);
        if (v == 1 && (y == 3 || y == 4))
            return (x & 1) ? RGB332(1, 2, 1) : RGB332(2, 3, 1);
        return h < 40 ? RGB332(3, 3, 1) : RGB332(2, 2, 1);
    }
    default: {  // living: cells with dark membranes
        float cx = v ? 4.5f : 3.0f, cy = v ? 3.0f : 4.0f;
        float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (d < 3)
            return RGB332(5, 1, 2);
        if (d < 8)
            return h < 128 ? RGB332(4, 0, 2) : RGB332(3, 0, 2);
        if (d < 13)
            return RGB332(2, 0, 1);
        return h < 30 ? RGB332(1, 4, 1) : RGB332(1, 0, 1);
    }
    }
}

/// Makes the stage's tile pictures: 32 full tiles (16 ways their sides can
/// be open, 2 textures) and 2 of each slope; open sides and slopes' faces lit.
/// @param theme 0 rock, 1 metal, 2 living
static void make_tiles(int theme) {
    static const uint8_t lit[3][2] = {
        {RGB332(7, 5, 1), RGB332(5, 3, 1)}, {RGB332(3, 7, 3), RGB332(2, 5, 2)}, {RGB332(3, 7, 1), RGB332(2, 4, 1)}};
    memset(G->tile, 0, sizeof G->tile);
    for (int l = 1; l < LOOKS; ++l) {
        int shape = look_shape(l);
        int v = shape == S_FULL ? (l - 1) & 1 : (l - 33) & 1;
        int open = shape == S_FULL ? (l - 1) >> 1 : 0;
        for (int y = 0; y < TILE; ++y)
            for (int x = 0; x < TILE; ++x) {
                if (!shape_px(shape, x, y))
                    continue;
                uint8_t c = texture(theme, v, x, y);
                int edge = 9;  // how far in from an open side
                if (open & X_UP) edge = y < edge ? y : edge;
                if (open & X_DOWN) edge = 7 - y < edge ? 7 - y : edge;
                if (open & X_LEFT) edge = x < edge ? x : edge;
                if (open & X_RIGHT) edge = 7 - x < edge ? 7 - x : edge;
                switch (shape) {
                case S_UP: edge = y - (7 - x); break;
                case S_DOWN: edge = y - x; break;
                case S_CUP: edge = (7 - x) - y; break;
                case S_CDOWN: edge = x - y; break;
                }
                if (edge == 0)
                    c = lit[theme][0];
                else if (edge == 1)
                    c = lit[theme][1];
                G->tile[l][y * TILE + x] = c;
            }
    }
}

/// Builds the stage's map from its stretches and blocks, then picks each
/// tile's look from its shape and its open sides.
/// @param st the stage
static void build_map(const stage_t *st) {
    memset(G->map, 0, sizeof G->map);
    int col = 0, ch = st->terr[0].ceil, fh = st->terr[0].floor;
    uint32_t seed = 0x9e3779b9u * (uint32_t)(st->theme + 1);
    for (const terr_t *s = st->terr; s->cols && col < MAP_W; ++s) {
        int jc = 0, jf = 0;
        for (int i = 0; i < s->cols && col < MAP_W; ++i, ++col) {
            if ((s->flags & TF_ROUGH) && i % 4 == 0) {
                seed = seed * 1664525u + 1013904223u;
                jc = (int)((seed >> 24) % 3) - 1;
                jf = (int)((seed >> 16) % 3) - 1;
            }
            int wc = s->ceil + jc, wf = s->floor + jf;
            if (wc < 1) wc = 1;
            if (wf < 1) wf = 1;
            if (wc > MAP_H / 2) wc = MAP_H / 2;
            if (wf > MAP_H / 2) wf = MAP_H / 2;
            int pc = ch, pf = fh;
            if (s->flags & TF_STEP) {
                ch = wc, fh = wf;
            } else {
                ch += wc > ch ? 1 : wc < ch ? -1 : 0;
                fh += wf > fh ? 1 : wf < fh ? -1 : 0;
            }
            for (int r = 0; r < ch && r < MAP_H; ++r)
                G->map[r][col] = S_FULL;
            for (int r = MAP_H - fh; r < MAP_H; ++r)
                if (r >= 0)
                    G->map[r][col] = S_FULL;
            if (!(s->flags & TF_STEP)) {
                // The slope tile where a height changed.
                if (ch > pc && ch >= 1 && ch - 1 < MAP_H)
                    G->map[ch - 1][col] = S_CDOWN;
                if (ch < pc && ch < MAP_H)
                    G->map[ch][col] = S_CUP;
                if (fh > pf && MAP_H - fh >= 0)
                    G->map[MAP_H - fh][col] = S_UP;
                if (fh < pf && MAP_H - pf >= 0)
                    G->map[MAP_H - pf][col] = S_DOWN;
            }
        }
    }
    G->map_w = col;
    for (const block_t *b = st->blocks; b && b->col; ++b)
        for (int r = b->row; r < b->row + b->h && r < MAP_H; ++r)
            for (int c = b->col; c < b->col + b->w && c < G->map_w; ++c)
                G->map[r][c] = S_FULL;
    // Looks (in place: 0 stays 0, so the open sides read the same after).
    for (int r = 0; r < MAP_H; ++r)
        for (int c = 0; c < G->map_w; ++c) {
            int s = G->map[r][c];
            if (!s)
                continue;
            int v = (int)(hash3(c, r, 7) & 1);
            if (s != S_FULL) {
                G->map[r][c] = (uint8_t)(33 + (s - S_UP) * 2 + v);
                continue;
            }
            int open = 0;
            if (r > 0 && !G->map[r - 1][c]) open |= X_UP;
            if (r < MAP_H - 1 && !G->map[r + 1][c]) open |= X_DOWN;
            if (c > 0 && !G->map[r][c - 1]) open |= X_LEFT;
            if (c < G->map_w - 1 && !G->map[r][c + 1]) open |= X_RIGHT;
            G->map[r][c] = (uint8_t)(1 + open * 2 + v);
        }
}

// ---- The stages ---------------------------------------------------------------------------------------

/// Stage 1, the rubble belt: rock, slopes and boulders.
static const terr_t terr1[] = {
    {24, 2, 3, TF_ROUGH}, {30, 5, 5, TF_ROUGH}, {30, 8, 8, TF_ROUGH}, {30, 3, 12, 0}, {30, 12, 3, 0},
    {44, 2, 2, TF_ROUGH}, {40, 7, 7, TF_ROUGH}, {40, 4, 10, TF_ROUGH}, {40, 10, 4, TF_ROUGH},
    {32, 5, 5, TF_ROUGH}, {60, 2, 2, 0}, {0, 0, 0, 0},
};
static const block_t blocks1[] = {{152, 8, 3, 3}, {164, 16, 3, 3}, {174, 6, 4, 2}, {182, 18, 3, 2}, {0, 0, 0, 0}};
static const event_t ev1[] = {
    {360, K_DART, 5, 90, 0},     {520, K_DART, 5, 150, 0},     {640, K_HAULER, 1, 112, CAP_ORB},
    {760, K_SWOOP, 3, 70, 0},    {780, K_TURRET, 1, 0, 0},     {860, K_TURRET, 1, 0, 0},
    {900, K_DART, 4, 60, 2},     {960, K_TURRET, 1, 0, 1},     {1020, K_HOPPER, 1, 0, 1},
    {1080, K_HOPPER, 1, 0, 0},   {1120, K_HAULER, 1, 160, CAP_SPEED}, {1180, K_SWOOP, 3, 130, 1},
    {1200, EV_CHECK, 0, 0, 0},   {1300, K_MINE, 3, 112, 0},    {1380, K_DART, 6, 80, 1},
    {1460, K_COIL, 7, 112, 0},   {1560, K_DART, 5, 150, 0},    {1600, K_TURRET, 1, 0, 0},
    {1640, K_TURRET, 1, 0, 1},   {1700, K_HAULER, 1, 112, CAP_ORB}, {1760, K_SWOOP, 3, 90, 0},
    {1840, K_HOPPER, 1, 0, 0},   {1880, K_HOPPER, 1, 0, 0},    {1940, K_DART, 5, 60, 2},
    {2000, K_TURRET, 1, 0, 0},   {2040, EV_CHECK, 0, 0, 0},    {2100, K_HATCH, 1, 0, 0},
    {2160, K_MINE, 4, 100, 0},   {2200, K_COIL, 8, 136, 0},    {2280, K_TURRET, 1, 0, 1},
    {2320, K_HAULER, 1, 150, CAP_MISSILE}, {2400, K_SWOOP, 3, 160, 1}, {2460, K_DART, 6, 100, 0},
    {2560, K_HOPPER, 1, 0, 0},   {2600, K_HOPPER, 1, 0, 1},    {2660, K_DART, 5, 70, 1},
    {2760, K_SWOOP, 4, 112, 0},  {0, 0, 0, 0, 0},
};

/// Stage 2, the iron citadel: metal, steps and pillars.
static const terr_t terr2[] = {
    {30, 3, 3, TF_STEP}, {16, 6, 3, TF_STEP}, {16, 3, 6, TF_STEP}, {16, 8, 3, TF_STEP}, {16, 3, 8, TF_STEP},
    {40, 4, 4, TF_STEP}, {24, 10, 4, TF_STEP}, {24, 4, 10, TF_STEP}, {24, 9, 9, TF_STEP},
    {40, 3, 3, TF_STEP}, {30, 7, 7, TF_STEP}, {24, 11, 4, TF_STEP}, {24, 4, 11, TF_STEP},
    {60, 1, 1, TF_STEP}, {0, 0, 0, 0},
};
static const block_t blocks2[] = {
    {100, 4, 2, 9}, {112, 14, 2, 10}, {124, 4, 2, 9}, {212, 14, 2, 11}, {224, 3, 2, 11}, {236, 14, 2, 11},
    {0, 0, 0, 0},
};
static const event_t ev2[] = {
    {360, K_DART, 5, 112, 2},    {440, K_TURRET, 1, 0, 0},     {470, K_TURRET, 1, 0, 1},
    {560, K_HAULER, 1, 112, CAP_ORB}, {600, K_TURRET, 1, 0, 1}, {640, K_TURRET, 1, 0, 0},
    {700, K_SWOOP, 3, 80, 0},    {780, K_DART, 5, 140, 0},     {880, K_SWOOP, 3, 60, 1},
    {960, K_HOPPER, 1, 0, 0},    {1000, K_TURRET, 1, 0, 1},    {1080, K_HAULER, 1, 112, CAP_SPEED},
    {1100, EV_CHECK, 0, 0, 0},   {1160, K_TURRET, 1, 0, 1},    {1220, K_DART, 6, 140, 0},
    {1300, K_MINE, 3, 90, 0},    {1360, K_TURRET, 1, 0, 0},    {1420, K_COIL, 7, 100, 0},
    {1500, K_HOPPER, 1, 0, 1},   {1560, K_HOPPER, 1, 0, 0},    {1620, K_HAULER, 1, 112, CAP_ORB},
    {1700, K_SWOOP, 3, 112, 0},  {1740, EV_CHECK, 0, 0, 0},    {1800, K_TURRET, 1, 0, 1},
    {1820, K_HATCH, 1, 0, 0},    {1900, K_DART, 5, 60, 0},     {1980, K_SWOOP, 4, 150, 1},
    {2060, K_TURRET, 1, 0, 0},   {2100, K_TURRET, 1, 0, 1},    {2160, K_MINE, 4, 112, 0},
    {2240, K_HAULER, 1, 140, CAP_MISSILE}, {2300, K_COIL, 8, 140, 0}, {2400, K_HOPPER, 1, 0, 1},
    {2440, K_TURRET, 1, 0, 0},   {2520, K_DART, 5, 80, 2},     {2620, K_SWOOP, 4, 112, 0},
    {0, 0, 0, 0, 0},
};

/// Stage 3, the hive: living walls, winding and narrow.
static const terr_t terr3[] = {
    {24, 3, 3, TF_ROUGH}, {24, 7, 4, TF_ROUGH}, {24, 4, 8, TF_ROUGH}, {24, 9, 6, TF_ROUGH},
    {24, 5, 10, TF_ROUGH}, {24, 10, 5, TF_ROUGH}, {30, 3, 3, TF_ROUGH}, {24, 8, 8, TF_ROUGH},
    {24, 6, 11, TF_ROUGH}, {24, 11, 6, TF_ROUGH}, {24, 7, 7, TF_ROUGH}, {24, 4, 4, TF_ROUGH},
    {60, 2, 2, 0}, {0, 0, 0, 0},
};
static const block_t blocks3[] = {{152, 10, 3, 4}, {162, 6, 3, 3}, {164, 17, 3, 3}, {0, 0, 0, 0}};
static const event_t ev3[] = {
    {340, K_SWOOP, 3, 112, 0},   {420, K_HATCH, 1, 0, 0},      {480, K_DART, 5, 80, 0},
    {560, K_HAULER, 1, 100, CAP_ORB}, {620, K_HATCH, 1, 0, 1}, {680, K_COIL, 7, 120, 0},
    {780, K_MINE, 3, 100, 0},    {840, K_HOPPER, 1, 0, 0},     {900, K_DART, 5, 90, 0},
    {960, K_HOPPER, 1, 0, 1},    {1000, EV_CHECK, 0, 0, 0},    {1020, K_HAULER, 1, 140, CAP_SPEED},
    {1080, K_HATCH, 1, 0, 0},    {1140, K_SWOOP, 4, 112, 1},   {1220, K_COIL, 8, 112, 1},
    {1300, K_MINE, 5, 112, 0},   {1380, K_HATCH, 1, 0, 1},     {1420, K_DART, 6, 130, 0},
    {1480, K_HAULER, 1, 112, CAP_ORB}, {1540, K_HOPPER, 1, 0, 0}, {1600, K_HATCH, 1, 0, 0},
    {1640, EV_CHECK, 0, 0, 0},   {1700, K_SWOOP, 3, 90, 0},    {1760, K_COIL, 8, 92, 0},
    {1840, K_HATCH, 1, 0, 1},    {1900, K_MINE, 4, 140, 0},    {1960, K_HAULER, 1, 130, CAP_MISSILE},
    {2020, K_DART, 6, 112, 1},   {2100, K_HOPPER, 1, 0, 0},    {2140, K_HOPPER, 1, 0, 1},
    {2200, K_HATCH, 1, 0, 0},    {2260, K_SWOOP, 4, 112, 0},   {2340, K_COIL, 9, 112, 1},
    {0, 0, 0, 0, 0},
};

/// The three stages.
static const stage_t stages[3] = {
    {"THE RUBBLE BELT", 0, 0.5f, TUNE_STAGE1, BOSS_ORRERY, terr1, blocks1, ev1},
    {"THE IRON CITADEL", 1, 0.5f, TUNE_STAGE2, BOSS_BASTION, terr2, blocks2, ev2},
    {"THE HIVE", 2, 0.45f, TUNE_STAGE3, BOSS_WYRM, terr3, blocks3, ev3},
};

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

/// A filled circle, clipped to the play area.
/// @param cx centre column
/// @param cy centre row
/// @param r radius
/// @param c colour
static void disc(int cx, int cy, int r, uint8_t c) {
    for (int y = -r; y <= r; ++y) {
        int w = (int)sqrtf((float)(r * r - y * y) + 0.5f);
        if (cy + y >= 0 && cy + y < PH)
            fill(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/// A line, clipped to the play area.
/// @param x0 start column
/// @param y0 start row
/// @param x1 end column
/// @param y1 end row
/// @param c colour
static void line(int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (y0 < PH)
            pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

/// A picture centred on a world point, see-through pixels left alone,
/// clipped to the play area, all white if `white` (just hit), at `scale`.
/// @param img IMG_ value
/// @param wx centre column (world)
/// @param wy centre row
/// @param white draw every pixel white
/// @param scale 1 or 2
static void sprite_at(int img, float wx, float wy, bool white, int scale) {
    const uint8_t *p = G->img[img];
    int x0 = scr_x(wx) - SPR * scale / 2, y0 = (int)floorf(wy) - SPR * scale / 2;
    for (int r = 0; r < SPR * scale; ++r) {
        int y = y0 + r;
        if ((unsigned)y >= PH)
            continue;
        uint8_t *row = BACK + y * SW;
        const uint8_t *src = p + (r / scale) * SPR;
        for (int c = 0; c < SPR * scale; ++c) {
            int x = x0 + c;
            uint8_t v = src[c / scale];
            if (v && (unsigned)x < SW)
                row[x] = white ? C_WHITE : v;
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

/// Text centred across the screen.
/// @param y top
/// @param s the text
/// @param c colour
static void text_mid(int y, const char *s, uint8_t c) {
    text((SW - 6 * (int)strlen(s) + 1) / 2, y, s, c);
}

/// Text centred, with a dark box behind it so it reads over the walls.
/// @param y top
/// @param s the text
/// @param c colour
static void text_box(int y, const char *s, uint8_t c) {
    int w = 6 * (int)strlen(s) + 7;
    fill((SW - w) / 2, y - 3, w, 13, C_BLACK);
    text_mid(y, s, c);
}

// ---- Making the pictures -----------------------------------------------------------------------------

/// The colour of an art letter (lancer_art.h); 0 for see-through.
/// @param ch the letter
/// @param tint the colour for 'X' and 'x' (0 red, 1 blue, 2 yellow, 3 green, 4 purple)
/// @return RGB332
static uint8_t art_colour(char ch, int tint) {
    static const uint8_t xs[5][2] = {
        {RGB332(7, 0, 0), RGB332(4, 0, 0)}, {RGB332(1, 4, 3), RGB332(0, 1, 2)}, {RGB332(7, 6, 0), RGB332(5, 3, 0)},
        {RGB332(1, 6, 0), RGB332(0, 3, 0)}, {RGB332(6, 1, 3), RGB332(3, 0, 2)}};
    switch (ch) {
    case 'W': return C_WHITE;
    case 'w': return RGB332(5, 5, 2);
    case 'K': return C_GREY;
    case 'k': return C_DKGREY;
    case 'R': return C_RED;
    case 'r': return RGB332(4, 0, 0);
    case 'O': return C_ORANGE;
    case 'Y': return C_YELLOW;
    case 'G': return C_GREEN;
    case 'g': return RGB332(0, 3, 1);
    case 'L': return RGB332(3, 7, 1);
    case 'B': return C_BLUE;
    case 'b': return RGB332(0, 1, 2);
    case 'C': return RGB332(0, 6, 3);
    case 'c': return RGB332(0, 3, 2);
    case 'P': return C_PURPLE;
    case 'p': return RGB332(2, 0, 2);
    case 'M': return RGB332(7, 2, 3);
    case 'T': return RGB332(5, 4, 1);
    case 'n': return RGB332(3, 2, 0);
    case 'X': return xs[tint][0];
    case 'x': return xs[tint][1];
    default: return 0;
    }
}

/// A built-in sprite (0: see-through; a drawn black pixel is made the
/// darkest blue so it isn't).
/// @param n ART_ value
/// @param tint the colour for 'X' and 'x'
/// @param out SPR * SPR bytes
static void art_get(int n, int tint, uint8_t *out) {
    for (int r = 0; r < SPR; ++r)
        for (int c = 0; c < SPR; ++c)
            out[r * SPR + c] = art_colour(art_rows[n][r][c], tint);
}

/// Recolours the red of a sprite read from a file to another tint (for
/// the pod's and the capsules' colours): reds become that colour.
/// @param p SPR * SPR bytes, changed in place
/// @param tint 1 blue, 2 yellow, 3 green, 4 purple
static void retint(uint8_t *p, int tint) {
    for (int i = 0; i < SPR * SPR; ++i) {
        uint8_t v = p[i];
        int r = v >> 5, g = (v >> 2) & 7, b = v & 3;
        if (!v || r < 4 || g > 2 || b > 1)
            continue;
        uint8_t bright = art_colour('X', tint), dark = art_colour('x', tint);
        p[i] = r >= 6 ? bright : dark;
    }
}

/// Frames read from /LANCER.SPR (spr_load's callback).
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

/// Makes every picture: from /LANCER.SPR if there is a usable one (16x16,
/// at least ART_COUNT frames), else the built-in art.
/// @param scratch ART_COUNT * SPR * SPR bytes to work in
/// @return true if /LANCER.SPR was used
static bool make_pictures(uint8_t *scratch) {
    bool from_file = false;
    FILINFO fi;
    if (f_stat(SPR_FILE, &fi) == FR_OK) {
        spr_info_t info;
        const char *err;
        from_file = spr_load(SPR_FILE, &info, spr_put, scratch, &err) && info.w == SPR && info.h == SPR &&
                    info.frames >= ART_COUNT;
    }
    for (int i = 0; i < IMG_COUNT; ++i) {
        const img_src_t *s = &img_src[i];
        uint8_t up[SPR * SPR];
        if (from_file) {
            memcpy(up, scratch + s->art * SPR * SPR, sizeof up);
            if (s->tint)
                retint(up, s->tint);
        } else {
            art_get(s->art, s->tint, up);
        }
        for (int r = 0; r < SPR; ++r)
            memcpy(G->img[i] + r * SPR, up + (s->flip ? SPR - 1 - r : r) * SPR, SPR);
    }
    return from_file;
}

// ---- High scores -----------------------------------------------------------------------------------

/// Reads /LANCER.HI ("score name" lines), or sets the starting table.
static void hi_load(void) {
    static const hiscore_t start[5] = {
        {50000, "ACE"}, {40000, "POD"}, {30000, "PIX"}, {20000, "USB"}, {10000, "OS2"},
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

/// Writes the table to /LANCER.HI.
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

// ---- Things in the world -----------------------------------------------------------------------------

/// An explosion (or points) at a world point.
/// @param x column
/// @param y row
/// @param big 0 small, 1 big, 2 points
/// @param points the points shown (big 2)
static void boom(float x, float y, int big, unsigned points) {
    for (int i = 0; i < MAX_BOOMS; ++i)
        if (G->boom[i].t < 0) {
            G->boom[i] = (boom_t){x, y, 0, (uint8_t)big, (uint16_t)points};
            return;
        }
}

/// Adds points, with an extra ship at 50000 and every 100000 after.
/// @param n the points
static void add_score(unsigned n) {
    G->score += n;
    if (G->score >= G->next_extra) {
        G->next_extra = G->next_extra == 50000 ? 100000 : G->next_extra + 100000;
        if (G->lives < 9)
            ++G->lives;
        lsnd_fx(FX_EXTRA);
    }
}

/// A message in the middle of the screen for a while.
/// @param s the text
/// @param frames how long
static void message(const char *s, int frames) {
    snprintf(G->msg, sizeof G->msg, "%s", s);
    G->msg_t = frames;
}

/// A free enemy entry, or -1.
/// @return its index
static int free_enemy(void) {
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (!G->e[i].on)
            return i;
    return -1;
}

/// Makes an enemy.
/// @param kind K_ value
/// @param x centre column (world)
/// @param y centre row
/// @return its index, or -1 if there is no room
static int spawn(int kind, float x, float y) {
    int i = free_enemy();
    if (i < 0)
        return -1;
    enemy_t *e = &G->e[i];
    memset(e, 0, sizeof *e);
    e->on = true;
    e->kind = (uint8_t)kind;
    e->x = e->bx = x;
    e->y = e->by = y;
    e->hp = kinds[kind].hp;
    e->side = 1;
    e->fire_t = (int16_t)(60 + rnd_n(120));
    return i;
}

/// True for a boss's part (not taken away when off the screen).
/// @param e the enemy
/// @return true for a boss part
static bool boss_part(const enemy_t *e) {
    return e->kind >= K_PLATE;
}

/// Fires an enemy shot.
/// @param x from column (world)
/// @param y from row
/// @param ang heading
/// @param speed pixels a frame
static void eshot(float x, float y, float ang, float speed) {
    for (int s = 0; s < MAX_ESHOTS; ++s)
        if (!G->es[s].on) {
            G->es[s] = (eshot_t){x, y, cos_a(ang) * speed, sin_a(ang) * speed, true};
            return;
        }
}

/// The heading from a point to the ship.
/// @param x column (world)
/// @param y row
/// @return the heading
static float aim(float x, float y) {
    return heading(G->px - x, G->py - y);
}

/// Fires `n` shots at the ship, `spread` apart.
/// @param x from column (world)
/// @param y from row
/// @param n how many
/// @param spread heading between them
/// @param speed base speed (made harder)
static void fire_at(float x, float y, int n, float spread, float speed) {
    if (G->pstate != P_ALIVE)
        return;
    float a = aim(x, y);
    for (int k = 0; k < n; ++k)
        eshot(x, y, a + (k - (n - 1) / 2.0f) * spread, speed * hard());
}

/// Fires a ring of shots.
/// @param x from column (world)
/// @param y from row
/// @param n how many
/// @param offset the first one's heading
/// @param speed base speed (made harder)
static void fire_ring(float x, float y, int n, float offset, float speed) {
    for (int k = 0; k < n; ++k)
        eshot(x, y, offset + k * 256.0f / n, speed * hard());
}

/// True if a world point is on the screen (with a margin).
/// @param x column
/// @param y row
/// @param m the margin
/// @return true if it is
static bool on_screen(float x, float y, float m) {
    float sx = x - G->cam;
    return sx > -m && sx < SW + m && y > -m && y < PH + m;
}

/// Leaves a capsule.
/// @param x column (world)
/// @param y row
/// @param kind CAP_ value
static void drop_capsule(float x, float y, int kind) {
    for (int i = 0; i < MAX_CAPS; ++i)
        if (!G->cap[i].on) {
            G->cap[i] = (cap_t){x, y, 0, (uint8_t)kind, true};
            return;
        }
}

// ---- Bosses -------------------------------------------------------------------------------------------

/// The third boss's body, along its path: where a point `back` behind the
/// head is.
/// @param back how far back along the path
/// @param x set to its column (world)
/// @param y set to its row
static void wyrm_pos(float back, float *x, float *y) {
    float p = G->boss.phase - back;
    float in = 220.0f - G->boss.t * 1.6f;  // it comes in from the right
    *x = G->cam + 196 + 96 * sin_a(p * 0.55f) + (in > 0 ? in : 0);
    *y = PH / 2.0f + 78 * sin_a(p * 0.83f + 40);
}

/// The hits a boss's core takes when it comes: more for each boss, and
/// more each round.
/// @param type BOSS_ value
/// @return its hits
static float core_hp(int type) {
    static const float times[4] = {1, 2.0f, 2.4f, 3.0f};
    return kinds[K_CORE].hp * times[type] * (1.0f + 0.3f * (float)G->loop);
}

/// Brings a stage's boss on.
static void boss_start(void) {
    boss_t *b = &G->boss;
    memset(b, 0, sizeof *b);
    b->type = G->st->boss;
    b->core = -1;
    float scale = 1.0f + 0.3f * (float)G->loop;
    switch (b->type) {
    case BOSS_ORRERY:
        b->cx = G->cam + SW + 40, b->cy = PH / 2.0f;
        b->core = spawn(K_CORE, b->cx, b->cy);
        for (int k = 0; k < 8; ++k) {
            int i = spawn(K_PLATE, b->cx, b->cy);
            if (i >= 0)
                G->e[i].k = (uint8_t)k;
        }
        break;
    case BOSS_BASTION:
        b->fort_x = G->cam + SW + 8, b->fort_y = PH / 2.0f;
        b->core = spawn(K_CORE, b->fort_x + 4, b->fort_y);
        for (int k = 0; k < 4; ++k) {
            int i = spawn(K_BTURRET, b->fort_x, b->fort_y);
            if (i >= 0) {
                G->e[i].k = (uint8_t)k;
                G->e[i].fire_t = (int16_t)(100 + k * 23);
            }
        }
        break;
    case BOSS_WYRM:
        b->core = spawn(K_CORE, G->cam + SW + 40, PH / 2.0f);
        for (int k = 0; k < 12; ++k) {
            int i = spawn(K_SEG, G->cam + SW + 40, PH / 2.0f);
            if (i >= 0) {
                G->e[i].k = (uint8_t)k;
                G->e[i].armour = true;
            }
        }
        {
            int i = spawn(K_TAIL, G->cam + SW + 40, PH / 2.0f);
            if (i >= 0)
                G->e[i].hp = (int16_t)(kinds[K_TAIL].hp * scale);
        }
        break;
    }
    if (b->core >= 0) {
        enemy_t *c = &G->e[b->core];
        c->hp = (int16_t)core_hp(b->type);
        c->armour = b->type == BOSS_BASTION;
    }
}

/// The boss's core's hits left as a fraction (for the faster second half).
/// @return 0-1
static float boss_health(void) {
    const boss_t *b = &G->boss;
    if (b->core < 0 || !G->e[b->core].on)
        return 0;
    return G->e[b->core].hp / core_hp(b->type);
}

/// The second boss's body (world): left, top, width, height.
/// @param x set to its left
/// @param y set to its top
/// @param w set to its width
/// @param h set to its height
static void fort_box(float *x, float *y, float *w, float *h) {
    *w = 96, *h = 150;
    *x = G->boss.fort_x;
    *y = G->boss.fort_y - *h / 2;
}

/// One frame of the boss.
static void boss_update(void) {
    boss_t *b = &G->boss;
    if (b->type == BOSS_NONE)
        return;
    ++b->t;
    if (b->dying) {
        if (--b->dying % 5 == 0) {
            float x = G->cam + 150 + rnd_n(150), y = 30 + rnd_n(PH - 60);
            if (b->type == BOSS_BASTION)
                x = b->fort_x + rnd_n(96), y = b->fort_y - 70 + rnd_n(140);
            boom(x, y, 1, 0);
            lsnd_fx(FX_BOOM);
        }
        if (!b->dying) {
            for (int i = 0; i < MAX_ENEMIES; ++i)
                if (G->e[i].on && boss_part(&G->e[i]))
                    G->e[i].on = false;
            b->type = BOSS_NONE;
            G->phase = PH_CLEAR;
            G->phase_t = 0;
            unsigned bonus = 10000u * (unsigned)G->stage * (unsigned)(G->loop + 1);
            add_score(bonus);
            char s[32];
            snprintf(s, sizeof s, "BONUS %u", bonus);
            message(s, 300);
            lsnd_tune(TUNE_CLEAR);
        }
        return;
    }
    float h = boss_health();
    bool angry = h < 0.5f;
    enemy_t *core = b->core >= 0 && G->e[b->core].on ? &G->e[b->core] : NULL;
    switch (b->type) {
    case BOSS_ORRERY: {
        // Glides in, then weaves; its plates go round, faster when it is hurt.
        float home = G->cam + 232 + 14 * sin_a(b->t * 0.31f);
        b->cx += (home - b->cx) * (b->t < 120 ? 0.03f : 0.2f);
        b->cy = PH / 2.0f + 58 * sin_a(b->t * 0.5f);
        if (core)
            core->x = b->cx, core->y = b->cy;
        float rot = angry ? 1.6f : 0.8f;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            enemy_t *e = &G->e[i];
            if (e->on && e->kind == K_PLATE) {
                float a = e->k * 32.0f + b->t * rot;
                e->x = b->cx + 31 * cos_a(a);
                e->y = b->cy + 31 * sin_a(a);
            }
        }
        if (b->t > 120) {
            int every = (int)(70 / hard());
            if (b->t % every == 0)
                fire_at(b->cx, b->cy, 3, 10, 1.8f);
            if (b->t % (angry ? 150 : 210) == 0)
                fire_ring(b->cx, b->cy, 16, (float)(b->t & 255), 1.2f);
            if (angry && b->t % 280 == 0)
                for (int k = 0; k < 3; ++k) {
                    int i = spawn(K_DART, G->cam + SW + 12, b->cy);
                    if (i >= 0)
                        G->e[i].t = (int16_t)(-k * 14), G->e[i].p = 2;
                }
        }
        break;
    }
    case BOSS_BASTION: {
        // Slides in, then rides up and down; its hatch opens to fire a laser.
        float target = G->cam + 222;
        if (b->fort_x > target)
            b->fort_x -= 1.0f;
        b->fort_y = PH / 2.0f + 36 * sin_a(b->t * 0.42f);
        static const float offs[4] = {-60, -30, 30, 60};
        int guns = 0;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            enemy_t *e = &G->e[i];
            if (!e->on || e->kind != K_BTURRET)
                continue;
            ++guns;
            e->x = b->fort_x - 4;
            e->y = b->fort_y + offs[e->k & 3];
            if (b->fort_x <= target && --e->fire_t <= 0) {
                fire_at(e->x - 6, e->y, 1, 0, 1.9f);
                e->fire_t = (int16_t)(90 / hard() + rnd_n(30));
            }
        }
        if (core) {
            core->x = b->fort_x + 4, core->y = b->fort_y;
            int cyc = b->t % 300;
            bool open = b->fort_x <= target && cyc >= (guns ? 190 : 120);
            core->armour = !open;
            b->beam_warn = open && cyc >= 205 && cyc < 230;
            bool was = b->beam;
            b->beam = open && cyc >= 230 && cyc < 272;
            if (b->beam && !was)
                lsnd_fx(FX_BEAM);
            if (!guns && b->t % 110 == 0 && b->fort_x <= target)
                fire_ring(core->x - 8, core->y, 12, (float)(b->t & 255), 1.3f);
        }
        break;
    }
    case BOSS_WYRM: {
        // Winds round the arena; its body is armour, the head and the tail aren't.
        b->phase += angry ? 1.5f : 1.0f;
        if (core) {
            wyrm_pos(0, &core->x, &core->y);
            if (b->t > 150 && b->t % (int)(100 / hard()) == 0)
                fire_at(core->x - 8, core->y, 5, 9, 1.7f);
        }
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            enemy_t *e = &G->e[i];
            if (!e->on)
                continue;
            if (e->kind == K_SEG)
                wyrm_pos((e->k + 1) * 6.0f, &e->x, &e->y);
            if (e->kind == K_TAIL) {
                wyrm_pos(13 * 6.0f, &e->x, &e->y);
                if (b->t > 150 && b->t % (angry ? 140 : 190) == 0)
                    fire_ring(e->x, e->y, 10, (float)(b->t & 255), 1.2f);
            }
        }
        break;
    }
    }
}

/// The boss's core is gone: its end starts.
static void boss_killed(void) {
    G->boss.dying = 160;
    G->boss.beam = G->boss.beam_warn = false;
    for (int s = 0; s < MAX_ESHOTS; ++s)
        G->es[s].on = false;
    lsnd_fx_stop(FX_BEAM);
    lsnd_tune(-1);
}

// ---- Enemies ----------------------------------------------------------------------------------------

/// Takes an enemy out, with its points, its explosion and what it leaves.
/// @param i the enemy
static void kill_enemy(int i) {
    enemy_t *e = &G->e[i];
    unsigned pts = kinds[e->kind].score;
    if (e->kind == K_COIL && e->k == 0)
        pts = 500;
    e->on = false;
    add_score(pts);
    bool big = e->kind == K_HAULER || e->kind == K_HATCH || e->kind == K_CORE || e->kind == K_BTURRET ||
               e->kind == K_TAIL || e->kind == K_PLATE;
    boom(e->x, e->y, big, 0);
    if (pts >= 300)
        boom(e->x, e->y - 10, 2, pts);
    lsnd_fx(big ? FX_BOOM : FX_HIT);
    switch (e->kind) {
    case K_HAULER: drop_capsule(e->x, e->y, e->p); break;
    case K_MINE: fire_ring(e->x, e->y, 6, (float)rnd_n(256), 1.1f); break;
    case K_CORE: boss_killed(); break;
    default: break;
    }
}

/// Hurts an enemy (white for a moment); it goes when it has no hits left.
/// @param i the enemy
/// @param dmg the hits
/// @return true if that killed it
static bool hurt(int i, int dmg) {
    enemy_t *e = &G->e[i];
    if (e->armour) {
        lsnd_fx(FX_ARMOUR);
        return false;
    }
    e->hp -= (int16_t)dmg;
    e->flash = 3;
    if (e->hp <= 0) {
        kill_enemy(i);
        return true;
    }
    return false;
}

/// Brings on what an event asks for.
/// @param ev the event
static void run_event(const event_t *ev) {
    float right = G->cam + SW + 12;
    switch (ev->kind) {
    case EV_CHECK:
        G->check_cam = G->cam;
        break;
    case K_DART:
    case K_SWOOP:
        for (int k = 0; k < ev->n; ++k) {
            bool behind = ev->kind == K_SWOOP && ev->p == 1;
            int i = spawn(ev->kind, behind ? G->cam - 12 : right, ev->y);
            if (i < 0)
                break;
            G->e[i].t = (int16_t)(-k * (ev->kind == K_DART ? 14 : 18));
            G->e[i].p = ev->p;
            G->e[i].dx = behind ? 3.0f : -2.0f;
        }
        break;
    case K_TURRET:
    case K_HOPPER:
    case K_HATCH: {
        int side = ev->p ? -1 : 1;
        float y, x = ground_spot(ev->x, side, &y);
        int i = spawn(ev->kind, x, y);
        if (i >= 0) {
            G->e[i].side = (int8_t)side;
            G->e[i].dx = -0.5f;
            G->e[i].t = (int16_t)rnd_n(100);
        }
        break;
    }
    case K_HAULER: {
        int i = spawn(K_HAULER, right + 4, ev->y);
        if (i >= 0)
            G->e[i].p = ev->p;
        break;
    }
    case K_MINE:
        for (int k = 0; k < ev->n; ++k) {
            int i = spawn(K_MINE, right, ev->y + (k - (ev->n - 1) / 2.0f) * 28);
            if (i >= 0)
                G->e[i].t = (int16_t)(-k * 12);
        }
        break;
    case K_COIL:
        for (int k = 0; k < ev->n; ++k) {
            int i = spawn(K_COIL, right + 30, ev->y);
            if (i < 0)
                break;
            G->e[i].k = (uint8_t)k;
            G->e[i].p = ev->p;
            G->e[i].hp = k == 0 ? 5 : 2;
            G->e[i].hidden = true;
        }
        break;
    }
}

/// One frame of an enemy.
/// @param i the enemy
static void update_enemy(int i) {
    enemy_t *e = &G->e[i];
    if (e->flash)
        --e->flash;
    if (e->orb_cool)
        --e->orb_cool;
    if (e->t < 0) {  // not on yet: keep its start just off the screen
        ++e->t;
        e->bx += G->scroll_now;
        e->x = e->bx;
        return;
    }
    ++e->t;
    bool shoot = false;  // fire at the ship this frame
    float diff = hard();
    switch (e->kind) {
    case K_DART: {
        static const float amp[3] = {24, 50, 6}, freq[3] = {2.4f, 1.6f, 3.0f};
        int p = e->p % 3;
        e->x = e->bx - e->t * 1.4f;
        e->y = e->by + amp[p] * sin_a(e->t * freq[p]);
        if (--e->fire_t <= 0) {
            shoot = rnd_n(100) < 20 + 15 * G->loop + 5 * G->stage;
            e->fire_t = 1000;
        }
        break;
    }
    case K_SWOOP:
        if (e->p == 1) {  // from behind: past the ship, then turns back at it
            if (scr_x(e->x) > 200)
                e->dx -= 0.12f;
            if (e->dx < -2.4f)
                e->dx = -2.4f;
            if (e->dx < 0 && e->fire_t > 0 && e->dx > -0.2f)
                shoot = true, e->fire_t = 0;
        } else if (scr_x(e->x) < 230) {
            if (e->t > 110)
                e->dx -= 0.05f;
        }
        if (e->t > 20 && e->t < 150) {
            float want = G->py > e->y ? 1.1f : -1.1f;
            e->dy += (want - e->dy) * 0.05f;
        } else {
            e->dy *= 0.95f;
        }
        e->x += e->dx;
        e->y += e->dy;
        break;
    case K_TURRET:
    case K_HATCH:
        if (e->kind == K_TURRET) {
            bool in_front = e->side > 0 ? G->py < e->y : G->py > e->y;
            if (--e->fire_t <= 0 && in_front && on_screen(e->x, e->y, -8)) {
                shoot = true;
                e->fire_t = (int16_t)(110 / diff + rnd_n(40));
            }
        } else {
            int c = e->t % 240;
            e->armour = c < 180;
            if ((c == 196 || c == 210 || c == 224) && on_screen(e->x, e->y, -4)) {
                int s = spawn(K_SPORE, e->x, e->y - 6 * e->side);
                if (s >= 0)
                    G->e[s].dy = -1.2f * e->side, G->e[s].dx = -0.5f;
            }
        }
        break;
    case K_HOPPER: {
        // Walks along its floor (or ceiling), hops now and then, and fires at the top of a hop.
        float ground = e->side > 0 ? floor_y(e->x + e->dx) - 8 : ceil_y(e->x + e->dx) + 8;
        bool airborne = e->dy != 0;
        if (!airborne && fabsf(ground - e->y) > 9)
            e->dx = -e->dx;  // a wall: turns round
        else
            e->x += e->dx;
        if (!airborne) {
            e->y = e->side > 0 ? floor_y(e->x) - 8 : ceil_y(e->x) + 8;
            if (e->t % 110 == 60)
                e->dy = -3.0f * e->side;
        } else {
            float before = e->dy;
            e->y += e->dy;
            e->dy += 0.12f * e->side;
            if ((before < 0) != (e->dy < 0))
                shoot = true;
            float g = e->side > 0 ? floor_y(e->x) - 8 : ceil_y(e->x) + 8;
            if ((e->side > 0 && e->dy > 0 && e->y >= g) || (e->side < 0 && e->dy < 0 && e->y <= g))
                e->y = g, e->dy = 0;
        }
        break;
    }
    case K_HAULER:
        e->x -= 0.35f;
        e->y = e->by + 10 * sin_a(e->t * 1.2f);
        break;
    case K_MINE: {
        float a = aim(e->x, e->y);
        e->dx += (cos_a(a) * 0.5f - e->dx) * 0.02f;
        e->dy += (sin_a(a) * 0.5f - e->dy) * 0.02f;
        e->x += e->dx - 0.15f;
        e->y += e->dy;
        break;
    }
    case K_COIL: {
        static const float amp[2] = {40, 60}, freq[2] = {1.8f, 1.2f};
        int te = e->t - e->k * 7, p = e->p & 1;
        e->hidden = te < 0;
        if (te < 0)
            te = 0;
        e->x = e->bx - te * 1.25f;
        e->y = e->by + amp[p] * sin_a(te * freq[p]);
        if (e->k == 0 && --e->fire_t <= 0 && !e->hidden) {
            shoot = true;
            e->fire_t = (int16_t)(120 / diff);
        }
        break;
    }
    case K_SPORE: {
        float a = aim(e->x, e->y), sp = 1.0f + 0.15f * G->loop;
        e->dx += (cos_a(a) * sp - e->dx) * 0.03f;
        e->dy += (sin_a(a) * sp - e->dy) * 0.03f;
        e->x += e->dx;
        e->y += e->dy;
        if (e->t > 420) {
            boom(e->x, e->y, 0, 0);
            e->on = false;
        }
        break;
    }
    default:  // boss parts: moved by boss_update
        break;
    }
    if (shoot && on_screen(e->x, e->y, -4))
        fire_at(e->x, e->y, 1, 0, 1.6f);
    if (!boss_part(e) && !e->hidden && !on_screen(e->x, e->y, 48))
        e->on = false;
}

// ---- The stage --------------------------------------------------------------------------------------

/// Starts stage `n` (1-3) of the round: the map, its pictures, its music.
/// @param n the stage
static void start_stage(int n) {
    G->stage = n;
    G->st = &stages[n - 1];
    build_map(G->st);
    make_tiles(G->st->theme);
    memset(G->e, 0, sizeof G->e);
    memset(G->ps, 0, sizeof G->ps);
    memset(G->es, 0, sizeof G->es);
    memset(G->cap, 0, sizeof G->cap);
    memset(&G->boss, 0, sizeof G->boss);
    G->boss.core = -1;
    G->cam = G->check_cam = 0;
    G->cam_end = (float)(G->map_w * TILE - SW);
    G->ev = 0;
    G->phase = PH_INTRO;
    G->phase_t = 0;
    G->px = SHIP_X;
    G->py = open_row(SHIP_X);
    if (G->orb != O_NONE && G->orb != O_FRONT && G->orb != O_BACK)
        G->orb = O_FREE, G->ox = G->px + 60, G->oy = G->py;
    lsnd_tune(G->st->tune);
}

/// The ship is lost for good or starts again: from the last checkpoint
/// (or in the boss's arena), without its pod and power-ups.
static void restart_ship(void) {
    memset(G->ps, 0, sizeof G->ps);
    memset(G->es, 0, sizeof G->es);
    memset(G->cap, 0, sizeof G->cap);
    bool boss = G->phase == PH_BOSS;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (!boss || !boss_part(&G->e[i]))
            G->e[i].on = false;
    if (!boss && G->phase != PH_CLEAR) {
        if (G->phase == PH_WARN) {
            lsnd_fx_stop(FX_WARNING);
            lsnd_tune(G->st->tune);
        }
        G->cam = G->check_cam;
        G->phase = PH_RUN;
        G->ev = 0;
        while (G->st->ev[G->ev].x && G->st->ev[G->ev].x <= G->cam + SW + 24)
            ++G->ev;
    }
    G->pstate = P_ENTER;
    G->pt = 0;
    G->px = G->cam - 16;
    G->py = open_row(G->cam + SHIP_X);
    G->orb = O_NONE;
    G->olvl = 1;
    G->speed_lvl = 0;
    G->missiles = false;
    G->charge = 0;
    G->fire_held = 0;
    // A pod capsule to start over with: a hauler brings it, or in a boss's arena it is just there.
    if (boss) {
        drop_capsule(G->cam + 110, G->py, CAP_ORB);
    } else {
        int i = spawn(K_HAULER, G->cam + SW + 16, G->py);
        if (i >= 0)
            G->e[i].p = CAP_ORB;
    }
}

/// One frame of the stage: the view, the events, the boss, the end.
static void update_stage(void) {
    ++G->phase_t;
    if (G->msg_t)
        --G->msg_t;
    // The view moves on (not once it has reached the arena).
    float step = 0;
    if (G->phase <= PH_WARN && G->cam < G->cam_end) {
        step = G->st->scroll;
        if (G->cam + step > G->cam_end)
            step = G->cam_end - G->cam;
    }
    G->cam += step;
    G->scroll_now = step;
    // What keeps its place on the screen moves with it.
    G->px += step;
    if (G->orb != O_NONE)
        G->ox += step;
    for (int s = 0; s < MAX_PSHOTS; ++s)
        if (G->ps[s].on)
            G->ps[s].x += step;
    for (int s = 0; s < MAX_ESHOTS; ++s)
        if (G->es[s].on)
            G->es[s].x += step;
    switch (G->phase) {
    case PH_INTRO:
        if (G->phase_t > 200)
            G->phase = PH_RUN, G->phase_t = 0;
        break;
    case PH_RUN:
        while (G->st->ev[G->ev].x && G->st->ev[G->ev].x <= G->cam + SW + 24)
            run_event(&G->st->ev[G->ev++]);
        if (G->cam >= G->cam_end - 170) {
            G->phase = PH_WARN;
            G->phase_t = 0;
            lsnd_tune(-1);
            lsnd_fx(FX_WARNING);
        }
        break;
    case PH_WARN:
        if (G->cam >= G->cam_end && G->phase_t > 150) {
            G->phase = PH_BOSS;
            G->phase_t = 0;
            G->check_cam = G->cam;
            boss_start();
            lsnd_tune(TUNE_BOSS);
        }
        break;
    case PH_BOSS:
        boss_update();
        break;
    case PH_CLEAR:
        if (G->phase_t > 330 && G->pstate == P_ALIVE) {
            if (G->stage == 3) {
                ++G->loop;
                message("ROUND CLEAR - THEY ARE TOUGHER NOW", 260);
                start_stage(1);
            } else {
                start_stage(G->stage + 1);
            }
        }
        break;
    }
}

// ---- The ship and its pod -----------------------------------------------------------------------------

/// A free shot entry, or NULL.
/// @return the entry
static pshot_t *new_pshot(void) {
    for (int s = 0; s < MAX_PSHOTS; ++s)
        if (!G->ps[s].on)
            return &G->ps[s];
    return NULL;
}

/// Fires one of the ship's shots.
/// @param kind PS_ value
/// @param x from column (world)
/// @param y from row
/// @param ang heading
/// @param speed pixels a frame
/// @return the shot, or NULL if there was no room
static pshot_t *pshot(int kind, float x, float y, float ang, float speed) {
    pshot_t *s = new_pshot();
    if (!s)
        return NULL;
    memset(s, 0, sizeof *s);
    *s = (pshot_t){x, y, cos_a(ang) * speed, sin_a(ang) * speed, 0, 0, (uint8_t)kind, true, 1, 0, 0, 0};
    return s;
}

/// The pod's shots, by its colour and level and where it is.
static void orb_fire(void) {
    if (G->orb == O_NONE || G->orb == O_COMING)
        return;
    float x = G->ox, y = G->oy;
    if (G->orb != O_FRONT && G->orb != O_BACK) {
        // On its own: straight ahead (and behind, at level 2).
        pshot(PS_ORBSHOT, x + 6, y, 0, 6);
        if (G->olvl > 1)
            pshot(PS_ORBSHOT, x - 6, y, 128, 6);
        return;
    }
    int dir = G->orb == O_FRONT ? 1 : -1;
    float fwd = dir > 0 ? 0 : 128;
    switch (G->ocol) {
    case 0: {  // red: a spread
        static const float a2[5] = {0, -10, 10, -22, 22};
        int n = G->olvl > 1 ? 5 : 3;
        for (int k = 0; k < n; ++k)
            pshot(PS_SPREAD, x + dir * 6, y, fwd + a2[k] * (n == 3 ? 1.3f : 1.0f), 5.5f);
        break;
    }
    case 1: {  // blue: lasers that bounce off the walls
        pshot(PS_PRISM, x + dir * 6, y, fwd - 28 * dir, 6);
        pshot(PS_PRISM, x + dir * 6, y, fwd + 28 * dir, 6);
        if (G->olvl > 1)
            pshot(PS_PRISM, x + dir * 6, y, fwd, 6);
        break;
    }
    default: {  // yellow: up and down, then along the walls
        for (int s = -1; s <= 1; s += 2) {
            pshot_t *p = pshot(PS_CRAWL, x, y + s * 6, 0, 0);
            if (p)
                p->dx = dir * 0.6f, p->dy = s * 4.0f, p->dir = (int8_t)dir;
        }
        if (G->olvl > 1)
            pshot(PS_PELLET, x + dir * 6, y, fwd, 7);
        break;
    }
    }
    lsnd_fx(FX_LASER);
}

/// A tap of fire: the plain shot, the pod's weapon, and missiles.
static void fire_tap(void) {
    int n = 0;
    for (int s = 0; s < MAX_PSHOTS; ++s)
        n += G->ps[s].on && G->ps[s].kind == PS_PELLET;
    if (n < 6)
        pshot(PS_PELLET, G->px + 8, G->py, 0, 7);
    orb_fire();
    if (G->missiles && G->missile_cool <= 0) {
        pshot(PS_MISSILE, G->px, G->py - 4, 224, 2.5f);
        pshot(PS_MISSILE, G->px, G->py + 4, 32, 2.5f);
        G->missile_cool = 40;
        lsnd_fx(FX_MISSILE);
    }
    lsnd_fx(FX_SHOT);
}

/// Locks the pod on to the side of the ship it is on.
static void orb_attach(void) {
    G->orb = G->ox >= G->px ? O_FRONT : O_BACK;
    lsnd_fx(FX_ATTACH);
}

/// One frame of the pod.
static void update_orb(void) {
    float sx = G->ox - G->cam;
    switch (G->orb) {
    case O_NONE:
        return;
    case O_COMING: {  // homes in on the ship's nose and locks on there
        float dx = G->px + 14 - G->ox, dy = G->py - G->oy, d = sqrtf(dx * dx + dy * dy);
        if (d < 4) {
            G->orb = O_FRONT;
            lsnd_fx(FX_ATTACH);
        } else {
            G->ox += dx / d * 3.5f;
            G->oy += dy / d * 3.5f;
        }
        break;
    }
    case O_FRONT:
    case O_BACK:
        (void)sx;
        G->ox = G->px + (G->orb == O_FRONT ? 14 : -14);
        G->oy = G->py;
        break;
    case O_OUT:
        G->ox += G->odx;
        G->odx *= 0.95f;
        if (fabsf(G->odx) < 1 || sx > SW - 30 || sx < 24)
            G->orb = O_FREE;
        break;
    case O_FREE:
        G->oy += G->py > G->oy + 1 ? 1.0f : G->py < G->oy - 1 ? -1.0f : 0;
        if (G->pstate == P_ALIVE && fabsf(G->ox - G->px) < 14 && fabsf(G->oy - G->py) < 10)
            orb_attach();
        break;
    case O_RECALL: {
        float dx = G->px - G->ox, dy = G->py - G->oy, d = sqrtf(dx * dx + dy * dy);
        if (d < 15) {
            orb_attach();
        } else {
            G->ox += dx / d * 4.5f;
            G->oy += dy / d * 4.5f;
        }
        break;
    }
    }
    if (G->ox < G->cam + 8)
        G->ox = G->cam + 8;
    if (G->ox > G->cam + SW - 8)
        G->ox = G->cam + SW - 8;
}

/// The ship is hit.
static void ship_hit(void) {
    if (G->pstate != P_ALIVE || G->invuln > 0)
        return;
    G->pstate = P_DYING;
    G->pt = 0;
    boom(G->px, G->py, 1, 0);
    boom(G->px + 6, G->py - 4, 1, 0);
    lsnd_fx_stop(FX_CHARGE);
    lsnd_fx(FX_PLAYER_BOOM);
    if (G->orb != O_NONE) {
        boom(G->ox, G->oy, 0, 0);
        G->orb = O_NONE;
    }
}

/// One frame of the ship: moving, firing, charging, the pod key.
static void update_ship(void) {
    if (G->invuln > 0)
        --G->invuln;
    if (G->missile_cool > 0)
        --G->missile_cool;
    ++G->pt;
    switch (G->pstate) {
    case P_ENTER:
        G->px += 1.6f;
        if (G->px - G->cam >= SHIP_X) {
            G->pstate = P_ALIVE;
            G->pt = 0;
            G->invuln = 150;
        }
        return;
    case P_DYING:
        if (G->pt > 110) {
            if (G->lives > 0) {
                --G->lives;
                restart_ship();
            } else {
                G->pstate = P_OVER;
                G->mode = M_GAMEOVER;
                G->mode_t = 0;
                lsnd_tune(TUNE_GAME_OVER);
            }
        }
        return;
    case P_OVER:
        return;
    }
    // Moving.
    float sp = 1.3f + 0.35f * G->speed_lvl, dx = 0, dy = 0;
    if (G->in & IN_LEFT) dx -= sp;
    if (G->in & IN_RIGHT) dx += sp;
    if (G->in & IN_UP) dy -= sp;
    if (G->in & IN_DOWN) dy += sp;
    if (dx && dy)
        dx *= 0.75f, dy *= 0.75f;
    G->px += dx;
    G->py += dy;
    if (G->px < G->cam + 10) G->px = G->cam + 10;
    if (G->px > G->cam + SW - 12) G->px = G->cam + SW - 12;
    if (G->py < 6) G->py = 6;
    if (G->py > PH - 6) G->py = PH - 6;
    // Fire: a tap shoots; held, it charges; let go, the lance goes.
    if (pressed(IN_FIRE)) {
        fire_tap();
        G->fire_held = 0;
        G->charge = 0;
    }
    if (G->in & IN_FIRE) {
        if (++G->fire_held == 12)
            lsnd_fx(FX_CHARGE);
        if (G->fire_held > 12 && G->charge < CHARGE_MAX)
            G->charge += 1.6f;
    } else if (G->in_prev & IN_FIRE) {
        if (G->fire_held >= 12)
            lsnd_fx_stop(FX_CHARGE);
        int lvl = G->charge >= CHARGE_MAX ? 3 : G->charge >= 64 ? 2 : G->charge >= 32 ? 1 : 0;
        if (lvl) {
            pshot_t *s = pshot(PS_LANCE, G->px + 14 + lvl * 6, G->py, 0, 8);
            if (s) {
                s->lvl = (uint8_t)lvl;
                s->serial = ++G->lance_serial;
                ++G->lances;
            }
            lsnd_fx(FX_LANCE);
        }
        G->charge = 0;
        G->fire_held = 0;
    }
    // The pod key: launch it, or call it back.
    if (pressed(IN_ORB) && G->orb != O_NONE && G->orb != O_COMING) {
        if (G->orb == O_FRONT || G->orb == O_BACK) {
            G->odx = G->orb == O_FRONT ? 6.5f : -6.5f;
            G->orb = O_OUT;
        } else {
            G->orb = O_RECALL;
        }
        lsnd_fx(FX_ORB);
    }
    // The walls.
    if (!clear_box(G->px, G->py, 5, 3) || solid(G->px + 8, G->py)) {
        G->invuln = 0;  // the walls aren't kind to a ship just back
        ship_hit();
    }
}

/// Takes a capsule.
/// @param c the capsule
static void take_capsule(cap_t *c) {
    c->on = false;
    lsnd_fx(FX_POWER);
    switch (c->kind) {
    case CAP_ORB: {
        int col = (c->t / 80) % 3;
        if (G->orb == O_NONE) {
            G->orb = O_COMING;
            G->ox = G->cam - 10;
            G->oy = G->py;
            G->ocol = col;
            G->olvl = 1;
        } else if (col == G->ocol && G->olvl < 2) {
            ++G->olvl;
        } else if (col == G->ocol) {
            add_score(1000);
        } else {
            G->ocol = col;
        }
        break;
    }
    case CAP_SPEED:
        if (G->speed_lvl < 3)
            ++G->speed_lvl;
        else
            add_score(1000);
        break;
    default:
        if (G->missiles)
            add_score(1000);
        G->missiles = true;
        break;
    }
}

// ---- Shots and hits -----------------------------------------------------------------------------------

/// Half a shot's size, for hits.
/// @param s the shot
/// @param hw set to half its width
/// @param hh set to half its height
static void shot_size(const pshot_t *s, float *hw, float *hh) {
    switch (s->kind) {
    case PS_LANCE: *hw = 6.0f + 6 * s->lvl, *hh = 2.0f + 3 * s->lvl; break;
    case PS_PELLET: *hw = 4, *hh = 2; break;
    default: *hw = 3, *hh = 3; break;
    }
}

/// The hits a shot does.
/// @param s the shot
/// @return its damage
static int shot_damage(const pshot_t *s) {
    switch (s->kind) {
    case PS_LANCE: return s->lvl == 3 ? 16 : s->lvl == 2 ? 7 : 3;
    case PS_CRAWL: case PS_MISSILE: return 2;
    default: return 1;
    }
}

/// A crawler on a wall: steps along it, up over bumps and down into dips.
/// @param s the shot
/// @return false if it can't go on
static bool crawl(pshot_t *s) {
    for (int k = 0; k < 4; ++k) {
        float nx = s->x + s->dir;
        if (solid(nx, s->y)) {  // a bump: climb it
            int up = 1;
            while (up <= 3 && solid(nx, s->y - up * s->side))
                ++up;
            if (up > 3)
                return false;
            s->y -= up * s->side;
        } else if (!solid(nx, s->y + s->side)) {  // a dip: follow it down
            int down = 1;
            while (down <= 3 && !solid(nx, s->y + (down + 1) * s->side))
                ++down;
            if (down > 3) {  // off the edge: falls again
                s->x = nx;
                s->dy = 4.0f * s->side;
                s->side = 0;
                return true;
            }
            s->y += down * s->side;
        }
        s->x = nx;
    }
    return true;
}

/// The nearest enemy a missile can hurt, or -1.
/// @param x from column (world)
/// @param y from row
/// @return its index
static int nearest_target(float x, float y) {
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        if (!e->on || e->t < 0 || e->hidden || e->armour || !on_screen(e->x, e->y, -4))
            continue;
        float d = (e->x - x) * (e->x - x) + (e->y - y) * (e->y - y);
        if (d < bd)
            bd = d, best = i;
    }
    return best;
}

/// One frame of the ship's shots: moving, walls, bounces, crawling, homing.
static void update_pshots(void) {
    for (int k = 0; k < MAX_PSHOTS; ++k) {
        pshot_t *s = &G->ps[k];
        if (!s->on)
            continue;
        ++s->t;
        float ox = s->x, oy = s->y;
        switch (s->kind) {
        case PS_PRISM:
            s->x += s->dx;
            s->y += s->dy;
            if (solid(s->x, s->y)) {
                bool across = solid(s->x, oy), down = solid(ox, s->y);
                if (across || !down)
                    s->dx = -s->dx;
                if (down || !across)
                    s->dy = -s->dy;
                s->x = ox, s->y = oy;
                if (++s->bounces > 3)
                    s->on = false;
            }
            break;
        case PS_CRAWL:
            if (s->side == 0) {
                s->x += s->dx;
                s->y += s->dy;
                if (solid(s->x, s->y)) {
                    s->side = s->dy > 0 ? 1 : -1;
                    while (solid(s->x, s->y) && fabsf(s->y - oy) < 8)
                        s->y -= s->side;
                    s->y = floorf(s->y) + 0.5f;
                }
            } else if (!crawl(s)) {
                s->on = false;
            }
            if (s->t > 110)
                s->on = false;
            break;
        case PS_MISSILE: {
            int t = nearest_target(s->x, s->y);
            float a = heading(s->dx, s->dy), sp = s->t < 10 ? 2.5f : 4.0f;
            if (t >= 0 && s->t > 6)
                a = turn_to(a, heading(G->e[t].x - s->x, G->e[t].y - s->y), 7);
            else if (s->t > 6)
                a = turn_to(a, 0, 4);
            s->dx = cos_a(a) * sp, s->dy = sin_a(a) * sp;
            s->x += s->dx;
            s->y += s->dy;
            if (solid(s->x, s->y) || s->t > 130)
                s->on = false;
            break;
        }
        case PS_LANCE:
            s->x += s->dx;
            if (solid(s->x + 6 * s->lvl, s->y)) {
                boom(s->x + 6 * s->lvl, s->y, 0, 0);
                s->on = false;
            }
            break;
        default:
            s->x += s->dx;
            s->y += s->dy;
            if (solid(s->x, s->y))
                s->on = false;
            break;
        }
        if (!on_screen(s->x, s->y, 8))
            s->on = false;
    }
}

/// True if a point is inside the second boss's body.
/// @param x column (world)
/// @param y row
/// @param m a margin around it
/// @return true if it is
static bool in_fort(float x, float y, float m) {
    if (G->boss.type != BOSS_BASTION)
        return false;
    float fx, fy, fw, fh;
    fort_box(&fx, &fy, &fw, &fh);
    return x > fx - m && x < fx + fw + m && y > fy - m && y < fy + fh + m;
}

/// Shots against enemies, the pod against enemies and shots, and
/// everything against the ship.
static void collisions(void) {
    // The ship's shots.
    for (int k = 0; k < MAX_PSHOTS; ++k) {
        pshot_t *s = &G->ps[k];
        if (!s->on)
            continue;
        float hw, hh;
        shot_size(s, &hw, &hh);
        if (in_fort(s->x, s->y, 0)) {  // the second boss's body stops shots, but for its hatch
            const enemy_t *c = G->boss.core >= 0 && G->e[G->boss.core].on ? &G->e[G->boss.core] : NULL;
            if (!c || fabsf(s->x - c->x) >= 14 || fabsf(s->y - c->y) >= 14) {
                s->on = false;
                continue;
            }
        }
        for (int i = 0; i < MAX_ENEMIES && s->on; ++i) {
            enemy_t *e = &G->e[i];
            if (!e->on || e->t < 0 || e->hidden)
                continue;
            if (fabsf(s->x - e->x) >= hw + kinds[e->kind].hw || fabsf(s->y - e->y) >= hh + kinds[e->kind].hh)
                continue;
            if (s->kind == PS_LANCE) {
                if (e->lance == s->serial)
                    continue;
                e->lance = s->serial;
                bool killed = hurt(i, shot_damage(s));
                if (!killed && (s->lvl < 3 || e->armour))
                    s->on = false;
            } else {
                hurt(i, shot_damage(s));
                s->on = false;
            }
        }
    }
    bool pod = G->orb != O_NONE;
    bool ship = G->pstate == P_ALIVE;
    // The enemies' shots.
    for (int k = 0; k < MAX_ESHOTS; ++k) {
        eshot_t *s = &G->es[k];
        if (!s->on)
            continue;
        if (pod && fabsf(s->x - G->ox) < 9 && fabsf(s->y - G->oy) < 9) {
            s->on = false;
            continue;
        }
        if (ship && fabsf(s->x - G->px) < 4 && fabsf(s->y - G->py) < 3) {
            s->on = false;
            ship_hit();
        }
    }
    // The enemies themselves.
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *e = &G->e[i];
        if (!e->on || e->t < 0 || e->hidden)
            continue;
        const kind_t *kd = &kinds[e->kind];
        if (pod && !e->orb_cool && fabsf(e->x - G->ox) < kd->hw + 7 && fabsf(e->y - G->oy) < kd->hh + 7) {
            e->orb_cool = 6;
            if (!e->armour)
                hurt(i, G->olvl);
            if (!e->on)
                continue;
        }
        if (ship && fabsf(e->x - G->px) < kd->hw + 4 && fabsf(e->y - G->py) < kd->hh + 2)
            ship_hit();
    }
    if (!ship)
        return;
    // The second boss's body and its laser.
    if (in_fort(G->px + 6, G->py, 2))
        ship_hit();
    if (G->boss.beam && G->boss.core >= 0) {
        const enemy_t *c = &G->e[G->boss.core];
        if (G->px < c->x && fabsf(G->py - c->y) < 7)
            ship_hit();
    }
    // Capsules.
    for (int i = 0; i < MAX_CAPS; ++i) {
        cap_t *c = &G->cap[i];
        if (c->on && fabsf(c->x - G->px) < 12 && fabsf(c->y - G->py) < 10)
            take_capsule(c);
    }
}

/// One frame of everything else: enemy shots, capsules, explosions, stars.
static void update_things(void) {
    for (int k = 0; k < MAX_ESHOTS; ++k) {
        eshot_t *s = &G->es[k];
        if (!s->on)
            continue;
        s->x += s->dx;
        s->y += s->dy;
        if (!on_screen(s->x, s->y, 8) || solid(s->x, s->y) || in_fort(s->x, s->y, -4))
            s->on = false;
    }
    for (int i = 0; i < MAX_CAPS; ++i) {
        cap_t *c = &G->cap[i];
        if (!c->on)
            continue;
        ++c->t;
        c->x -= 0.25f;
        if (!on_screen(c->x, c->y, 16))
            c->on = false;
    }
    for (int i = 0; i < MAX_BOOMS; ++i)
        if (G->boom[i].t >= 0 && ++G->boom[i].t > (G->boom[i].big == 2 ? 50 : G->boom[i].big ? 34 : 18))
            G->boom[i].t = -1;
    float drift = G->mode == M_PLAY && G->scroll_now > 0 ? 1.0f : 0.5f;
    for (int i = 0; i < STARS; ++i) {
        star_t *s = &G->star[i];
        s->x -= s->speed * drift;
        if (s->x < 0) {
            s->x += SW;
            s->y = (uint8_t)rnd_n(PH);
        }
    }
}

/// Starts a new game.
static void new_game(void) {
    G->score = 0;
    G->next_extra = 50000;
    G->lives = 2;
    G->loop = 0;
    G->pstate = P_ALIVE;
    G->pt = 0;
    G->invuln = 120;
    G->orb = O_NONE;
    G->olvl = 1;
    G->speed_lvl = 0;
    G->missiles = false;
    G->charge = 0;
    G->fire_held = 0;
    G->lances = 0;
    G->msg_t = 0;
    G->mode = M_PLAY;
    G->mode_t = 0;
    G->paused = false;
    G->tick = 0;
    for (int i = 0; i < MAX_BOOMS; ++i)
        G->boom[i].t = -1;
    start_stage(1);
    lsnd_tune(TUNE_START);  // the stage's theme starts after it (see update)
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
        lsnd_enable(G->sound);
        if (G->sound && G->mode == M_PLAY && G->phase != PH_WARN)
            lsnd_tune(G->phase == PH_BOSS ? TUNE_BOSS : G->st->tune);
    }
    if (pressed(IN_MUSIC)) {
        G->music = !G->music;
        lsnd_music(G->music);
        if (G->music && G->mode == M_PLAY && G->phase != PH_WARN)
            lsnd_tune(G->phase == PH_BOSS ? TUNE_BOSS : G->st->tune);
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
            lsnd_fx(FX_COIN);
            new_game();
        }
        break;
    case M_PLAY:
        if (pressed(IN_QUIT)) {
            lsnd_tune(-1);
            lsnd_fx_stop(FX_CHARGE);
            lsnd_fx_stop(FX_BEAM);
            lsnd_fx_stop(FX_WARNING);
            G->mode = M_TITLE;
            G->mode_t = 0;
            return;
        }
        if (pressed(IN_PAUSE))
            G->paused = !G->paused;
        if (G->paused)
            return;
        ++G->tick;
        // The stage's theme after the starting tune.
        if (G->phase == PH_INTRO && G->phase_t > 20 && !lsnd_tune_playing())
            lsnd_tune(G->st->tune);
        update_stage();
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].on)
                update_enemy(i);
        update_ship();
        update_orb();
        update_pshots();
        collisions();
        break;
    case M_GAMEOVER:
        ++G->tick;
        update_stage();
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].on)
                update_enemy(i);
        update_pshots();
        if (G->mode_t > 240 || (G->mode_t > 60 && pressed(IN_START | IN_FIRE))) {
            G->entry_rank = hi_rank(G->score);
            G->mode_t = 0;
            if (G->entry_rank >= 0) {
                G->mode = M_ENTRY;
                G->entry_pos = 0;
                memcpy(G->entry_name, "A  ", 4);
                lsnd_tune(TUNE_HIGH);
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
        G->key_hold = dir ? G->key_hold + 1 : 0;
        bool step = G->key_hold == 1 || (G->key_hold > 20 && G->key_hold % 6 == 0);
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
            lsnd_fx(FX_COIN);
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

/// The stars behind everything.
static void draw_stars(void) {
    for (int i = 0; i < STARS; ++i)
        pset((int)G->star[i].x, G->star[i].y, G->star[i].colour);
}

/// The walls in view.
static void draw_map(void) {
    int cx = (int)floorf(G->cam), col0 = cx >> 3, off = cx & 7;
    for (int c = 0; c <= SW / TILE; ++c) {
        int col = col0 + c, sx = c * TILE - off;
        if (col >= G->map_w)
            break;
        int x0 = sx < 0 ? -sx : 0, x1 = sx + TILE > SW ? SW - sx : TILE;
        for (int r = 0; r < MAP_H; ++r) {
            int l = G->map[r][col];
            if (!l)
                continue;
            const uint8_t *pic = G->tile[l];
            bool full = l <= 32;
            for (int y = 0; y < TILE; ++y) {
                uint8_t *d = BACK + (r * TILE + y) * SW + sx;
                const uint8_t *s = pic + y * TILE;
                if (full) {
                    memcpy(d + x0, s + x0, (size_t)(x1 - x0));
                } else {
                    for (int x = x0; x < x1; ++x)
                        if (s[x])
                            d[x] = s[x];
                }
            }
        }
    }
}

/// An explosion or points.
/// @param b the explosion
static void draw_boom(const boom_t *b) {
    int x = scr_x(b->x), y = (int)b->y, t = b->t;
    if (b->big == 2) {
        char s[8];
        snprintf(s, sizeof s, "%u", b->points);
        if (y - t / 4 < PH - 7)
            text(x - 3 * (int)strlen(s), y - t / 4, s, (t >> 2) & 1 ? C_WHITE : C_YELLOW);
        return;
    }
    static const uint8_t cols[6] = {C_WHITE, C_YELLOW, C_ORANGE, RGB332(7, 2, 0), C_RED, RGB332(3, 0, 0)};
    int len = b->big ? 34 : 18, n = b->big ? 14 : 8;
    uint8_t c = cols[t * 6 / (len + 1)];
    if (t < len / 3)
        disc(x, y, (b->big ? 9 : 5) - t / 2, t < 3 ? C_WHITE : C_YELLOW);
    float r = t * (b->big ? 1.6f : 1.1f);
    for (int k = 0; k < n; ++k) {
        float a = k * 256.0f / n + (k & 1) * 9;
        int px = x + (int)(cos_a(a) * r), py = y + (int)(sin_a(a) * r * (k & 1 ? 0.7f : 1.0f));
        if (py >= 0 && py < PH - 1)
            fill(px, py, b->big ? 2 : 1 + (t < 8), 2, c);
    }
}

/// A charged lance: a bright spindle of plasma.
/// @param s the shot
static void draw_lance(const pshot_t *s) {
    int len = 12 + 12 * s->lvl, hh = 2 + 3 * s->lvl;
    int x0 = scr_x(s->x) - len / 2, y = (int)s->y;
    for (int i = 0; i < len; ++i) {
        float f = (float)i / len, w = 1 - (2 * f - 1) * (2 * f - 1);
        int h = (int)(hh * w + ((i + (int)G->tick) % 6 < 3));
        if (h < 1)
            h = 1;
        fill(x0 + i, y - h, 1, 2 * h + 1, RGB332(0, 4, 3));
        if (h > 2)
            fill(x0 + i, y - h + 2, 1, 2 * h - 3, RGB332(3, 7, 3));
        if (h > 4)
            fill(x0 + i, y - h / 3, 1, 2 * (h / 3) + 1, C_WHITE);
    }
}

/// The first boss's core: an eye in rings that looks at the ship.
/// @param e the core
static void draw_eye(const enemy_t *e) {
    int x = scr_x(e->x), y = (int)e->y;
    bool w = e->flash > 0;
    disc(x, y, 14, w ? C_WHITE : RGB332(2, 0, 1));
    disc(x, y, 12, w ? C_WHITE : ((G->tick >> 3) & 1 ? RGB332(6, 1, 2) : RGB332(5, 0, 2)));
    disc(x, y, 8, C_WHITE);
    float a = aim(e->x, e->y);
    disc(x + (int)(cos_a(a) * 3), y + (int)(sin_a(a) * 3), 4, RGB332(0, 0, 1));
    disc(x + (int)(cos_a(a) * 3) - 1, y + (int)(sin_a(a) * 3) - 1, 1, C_CYAN);
}

/// The second boss: a fortress block with guns, a hatch and its laser.
static void draw_fort(void) {
    const boss_t *b = &G->boss;
    float fx, fy, fw, fh;
    fort_box(&fx, &fy, &fw, &fh);
    int x0 = scr_x(fx), y0 = (int)fy;
    for (int r = 0; r < (int)fh / TILE; ++r)
        for (int c = 0; c < (int)fw / TILE; ++c) {
            const uint8_t *pic = G->tile[1 + (((r * 3 + c) & 1))];
            for (int y = 0; y < TILE; ++y) {
                int py = y0 + r * TILE + y;
                if ((unsigned)py >= PH)
                    continue;
                for (int x = 0; x < TILE; ++x) {
                    int px = x0 + c * TILE + x;
                    if ((unsigned)px < SW)
                        BACK[py * SW + px] = pic[y * TILE + x];
                }
            }
        }
    // Trim, lights, and a red stripe that runs along it.
    fill(x0, y0, (int)fw, 2, RGB332(3, 7, 3));
    fill(x0, y0 + (int)fh - 2, (int)fw, 2, RGB332(3, 7, 3));
    fill(x0, y0, 2, (int)fh, RGB332(3, 7, 3));
    for (int k = 0; k < 6; ++k)
        fill(x0 + 10 + k * 14, y0 + 8, 6, 3, ((G->tick >> 3) + k) % 6 == 0 ? C_RED : RGB332(3, 0, 0));
    for (int k = 0; k < 6; ++k)
        fill(x0 + 10 + k * 14, y0 + (int)fh - 11, 6, 3, ((G->tick >> 3) + k) % 6 == 0 ? C_RED : RGB332(3, 0, 0));
    // The guns.
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        if (!e->on || e->kind != K_BTURRET)
            continue;
        int x = scr_x(e->x), y = (int)e->y;
        uint8_t body = e->flash ? C_WHITE : C_GREY;
        fill(x - 6, y - 6, 12, 12, RGB332(1, 1, 1));
        fill(x - 5, y - 5, 10, 10, body);
        fill(x - 3, y - 3, 6, 6, RGB332(5, 1, 3));
        float a = aim(e->x, e->y);
        line(x, y, x + (int)(cos_a(a) * 10), y + (int)(sin_a(a) * 10), C_WHITE);
    }
    // The hatch: shut, or open with its eye glowing.
    if (b->core >= 0 && G->e[b->core].on) {
        const enemy_t *c = &G->e[b->core];
        int x = scr_x(c->x), y = (int)c->y;
        if (c->armour) {
            fill(x - 10, y - 12, 20, 24, RGB332(1, 1, 1));
            for (int k = 0; k < 5; ++k)
                fill(x - 9, y - 11 + k * 5, 18, 3, C_GREY);
        } else {
            fill(x - 10, y - 12, 20, 24, RGB332(2, 0, 0));
            disc(x, y, 8, c->flash ? C_WHITE : ((G->tick >> 2) & 1 ? C_RED : C_ORANGE));
            disc(x, y, 4, C_YELLOW);
        }
        if (b->beam_warn && (G->tick >> 2) & 1)
            for (int px = 0; px < x - 10; px += 4)
                pset(px, y, C_RED);
        if (b->beam) {
            int h = 4 + (int)(G->tick & 1);
            fill(0, y - h, x - 8, 2 * h + 1, RGB332(7, 1, 1));
            fill(0, y - h + 2, x - 8, 2 * h - 3, C_YELLOW);
            fill(0, y - 1, x - 8, 3, C_WHITE);
        }
    }
}

/// An enemy.
/// @param e the enemy
static void draw_enemy(const enemy_t *e) {
    bool w = e->flash > 0, f = (G->tick >> 3) & 1;
    switch (e->kind) {
    case K_DART: sprite_at(f ? IMG_DART_B : IMG_DART, e->x, e->y, w, 1); break;
    case K_SWOOP: sprite_at(f ? IMG_SWOOP_B : IMG_SWOOP, e->x, e->y, w, 1); break;
    case K_TURRET: {
        sprite_at(e->side > 0 ? IMG_TURRET : IMG_TURRET_C, e->x, e->y, w, 1);
        int x = scr_x(e->x), y = (int)e->y + 2 * e->side;
        float a = aim(e->x, e->y);
        if ((e->side > 0) == (sin_a(a) < 0))
            line(x, y, x + (int)(cos_a(a) * 8), y + (int)(sin_a(a) * 8), C_WHITE);
        break;
    }
    case K_HOPPER: {
        bool step = e->dy == 0 && ((G->tick >> 3) & 1);
        sprite_at(e->side > 0 ? (step ? IMG_HOPPER_B : IMG_HOPPER) : (step ? IMG_HOPPER_CB : IMG_HOPPER_C), e->x,
                  e->y, w, 1);
        break;
    }
    case K_HAULER: sprite_at(f ? IMG_HAULER_B : IMG_HAULER, e->x, e->y, w, 1); break;
    case K_MINE: sprite_at(((G->tick >> 4) & 1) ? IMG_MINE_B : IMG_MINE, e->x, e->y, w, 1); break;
    case K_COIL: sprite_at(e->k == 0 ? IMG_COIL_HEAD : IMG_COIL, e->x, e->y, w, 1); break;
    case K_HATCH: {
        bool open = !e->armour;
        sprite_at(e->side > 0 ? (open ? IMG_HATCH_OPEN : IMG_HATCH) : (open ? IMG_HATCH_OPEN_C : IMG_HATCH_C), e->x,
                  e->y, w, 1);
        break;
    }
    case K_SPORE: {
        int x = scr_x(e->x), y = (int)e->y, r = 2 + (int)((G->tick >> 2) & 1);
        disc(x, y, r + 1, RGB332(2, 4, 0));
        disc(x, y, r, w ? C_WHITE : RGB332(5, 7, 1));
        break;
    }
    case K_PLATE: sprite_at(IMG_PLATE, e->x, e->y, w, 1); break;
    case K_CORE:
        if (G->boss.type == BOSS_ORRERY)
            draw_eye(e);
        else if (G->boss.type == BOSS_WYRM)
            sprite_at(IMG_COIL_HEAD, e->x, e->y, w, 2);
        break;  // the second boss's is drawn with its body
    case K_SEG: sprite_at(IMG_COIL, e->x, e->y, false, 1); break;
    case K_TAIL: sprite_at(((G->tick >> 3) & 1) ? IMG_MINE_B : IMG_MINE, e->x, e->y, w, 1); break;
    default: break;
    }
}

/// A capsule: its colour (a pod capsule's changes as it goes) and its letter.
/// @param c the capsule
static void draw_capsule(const cap_t *c) {
    static const char letter[3] = {'P', 'S', 'M'};
    int img = c->kind == CAP_ORB ? IMG_CAP + (c->t / 80) % 3 : c->kind == CAP_SPEED ? IMG_CAP_S : IMG_CAP_M;
    float y = c->y + 2 * sin_a(c->t * 3.0f);
    sprite_at(img, c->x, y, false, 1);
    char s[2] = {letter[c->kind], 0};
    text(scr_x(c->x) - 2, (int)y - 3, s, C_WHITE);
}

/// The ship's shots.
static void draw_pshots(void) {
    for (int k = 0; k < MAX_PSHOTS; ++k) {
        const pshot_t *s = &G->ps[k];
        if (!s->on)
            continue;
        int x = scr_x(s->x), y = (int)s->y;
        if (y < 1 || y >= PH - 1)
            continue;
        switch (s->kind) {
        case PS_LANCE: draw_lance(s); break;
        case PS_PELLET:
        case PS_ORBSHOT:
            fill(x - 4, y - 1, 8, 2, C_YELLOW);
            fill(x - 1, y - 1, 4, 2, C_WHITE);
            break;
        case PS_SPREAD:
            fill(x - 2, y - 2, 4, 4, RGB332(7, 2, 0));
            fill(x - 1, y - 1, 2, 2, C_YELLOW);
            break;
        case PS_PRISM:
            line(x, y, x - (int)(s->dx * 1.6f), y - (int)(s->dy * 1.6f), C_CYAN);
            fill(x - 1, y - 1, 2, 2, C_WHITE);
            break;
        case PS_CRAWL:
            fill(x - 2, y - 2, 5, 4, (G->tick & 2) ? C_YELLOW : C_ORANGE);
            pset(x, y, C_WHITE);
            break;
        case PS_MISSILE:
            fill(x - 2, y - 1, 4, 2, C_GREY);
            pset(x - 3 - (int)(G->tick & 1), y, C_ORANGE);
            break;
        }
    }
}

/// The ship, its charge glow, and the pod.
static void draw_ship(void) {
    int ps = G->pstate;
    bool show = ps == P_ENTER || (ps == P_ALIVE && (G->invuln == 0 || (G->invuln >> 2) & 1));
    if (show) {
        sprite_at((G->tick >> 2) & 1 ? IMG_SHIP_B : IMG_SHIP, G->px, G->py, false, 1);
        if (G->charge > 8) {
            int x = scr_x(G->px) + 10, y = (int)G->py, r = 1 + (int)(G->charge / 24) + (int)(G->tick & 1);
            disc(x, y, r, G->charge >= CHARGE_MAX && (G->tick & 4) ? C_WHITE : RGB332(1, 6, 3));
            for (int k = 0; k < 3; ++k) {
                float a = (float)((G->tick * 13 + k * 85) & 255), d = 12 - (G->tick + k * 4) % 12;
                pset(x + (int)(cos_a(a) * d), y + (int)(sin_a(a) * d), C_CYAN);
            }
        }
    }
    if (G->orb != O_NONE) {
        int img = IMG_ORB + G->ocol * 2 + ((G->tick >> 3) & 1);
        sprite_at(img, G->ox, G->oy, false, 1);
        if (G->olvl > 1 && (G->tick & 8)) {
            int x = scr_x(G->ox), y = (int)G->oy;
            pset(x - 9, y, C_WHITE), pset(x + 8, y, C_WHITE), pset(x, y - 9, C_WHITE), pset(x, y + 8, C_WHITE);
        }
    }
}

/// The panel below the play area.
static void draw_panel(void) {
    fill(0, PH, SW, SH - PH, C_HUD);
    char s[40];
    // The beam's charge.
    text(4, PH + 1, "BEAM", C_CYAN);
    fill(30, PH + 1, 102, 7, RGB332(1, 1, 1));
    fill(31, PH + 2, 100, 5, C_BLACK);
    int w = (int)(G->charge * 100 / CHARGE_MAX);
    bool full = G->charge >= CHARGE_MAX;
    fill(31, PH + 2, w, 5, full && (G->tick & 4) ? C_WHITE : w >= 66 ? RGB332(3, 7, 3) : RGB332(0, 4, 3));
    pset(31 + 33, PH + 2, C_GREY), pset(31 + 66, PH + 2, C_GREY);
    // Ships in reserve.
    for (int i = 0; i < G->lives && i < 6; ++i) {
        int x = 140 + i * 10, y = PH + 2;
        fill(x, y + 2, 7, 2, C_BLUE);
        fill(x + 2, y + 1, 3, 4, C_GREY);
        pset(x + 7, y + 2, C_WHITE);
    }
    // Power-ups: speed, missiles, the pod's colour and level.
    for (int i = 0; i < G->speed_lvl; ++i)  // a chevron each
        for (int k = 0; k < 3; ++k)
            pset(206 + i * 6 + k, PH + 2 + k, C_GREEN), pset(206 + i * 6 + k, PH + 6 - k, C_GREEN);
    if (G->missiles)
        text(228, PH + 1, "M", C_PURPLE);
    if (G->orb != O_NONE) {
        static const char *const names[3] = {"RED", "BLUE", "YELLOW"};
        static const uint8_t cols[3] = {C_RED, RGB332(1, 4, 3), C_YELLOW};
        snprintf(s, sizeof s, "%s", names[G->ocol]);
        text(240, PH + 1, s, cols[G->ocol]);
        if (G->olvl > 1) {  // '+' isn't in the font: a small cross
            int x = 240 + 6 * (int)strlen(s) + 2;
            fill(x, PH + 4, 5, 1, C_WHITE);
            fill(x + 2, PH + 2, 1, 5, C_WHITE);
        }
    }
    snprintf(s, sizeof s, "1P %07lu", (unsigned long)G->score);
    text(4, PH + 9, s, C_WHITE);
    uint32_t hi = G->hi[0].score > G->score ? G->hi[0].score : G->score;
    snprintf(s, sizeof s, "HI %07lu", (unsigned long)hi);
    text(100, PH + 9, s, C_RED);
    if (G->loop)
        snprintf(s, sizeof s, "ROUND %d STAGE %d", G->loop + 1, G->stage);
    else
        snprintf(s, sizeof s, "STAGE %d", G->stage);
    text(SW - 4 - 6 * (int)strlen(s), PH + 9, s, C_YELLOW);
}

/// The title screen: the name, a ship and its pod going by, and either the
/// keys or the high scores.
static void draw_title(void) {
    text_s((SW - 6 * 6 * 6) / 2 + 3, 26, "LANCER", RGB332(1, 2, 3), 6);
    text_s((SW - 6 * 6 * 6) / 2, 24, "LANCER", (G->mode_t >> 4) % 8 == 0 ? C_WHITE : C_CYAN, 6);
    float x = G->cam + (float)((G->mode_t * 2) % (SW + 80)) - 40;
    sprite_at(IMG_SHIP, x, 82, false, 1);
    sprite_at(IMG_ORB + ((G->mode_t / 200) % 3) * 2 + ((G->mode_t >> 3) & 1), x + 14, 82, false, 1);
    char s[40];
    if (G->title_page == 0) {
        text_mid(102, "ARROWS OR W A S D   MOVE", C_WHITE);
        text_mid(114, "SPACE OR Z   FIRE - HOLD TO CHARGE", C_WHITE);
        text_mid(126, "X OR C   LAUNCH OR CALL THE POD", C_WHITE);
        text_mid(138, "P PAUSE   M SOUND   N MUSIC", C_GREY);
        text_mid(158, "SHOOT HAULERS FOR CAPSULES", C_YELLOW);
        text_mid(170, "P POD   S SPEED   M MISSILES", C_YELLOW);
    } else {
        text_mid(104, "- BEST PILOTS -", C_RED);
        for (int i = 0; i < 5; ++i) {
            snprintf(s, sizeof s, "%d   %07lu   %.3s", i + 1, (unsigned long)G->hi[i].score, G->hi[i].name);
            text_mid(122 + i * 12, s, i == 0 ? C_YELLOW : C_WHITE);
        }
    }
    if ((G->mode_t >> 5) & 1)
        text_mid(194, "PUSH ENTER", C_CYAN);
    text_mid(210, "ESC TO LEAVE", C_GREY);
}

/// The whole frame.
static void draw(void) {
    memset(BACK, 0, SW * SH);
    draw_stars();
    if (G->mode == M_TITLE) {
        draw_title();
        if (G->msg_t && G->msg[0])
            text_mid(180, G->msg, C_RED);
        if (G->msg_t)
            --G->msg_t;
        fill(0, PH, SW, SH - PH, C_HUD);
        return;
    }
    draw_map();
    if (G->boss.type == BOSS_BASTION)
        draw_fort();
    for (int i = 0; i < MAX_CAPS; ++i)
        if (G->cap[i].on)
            draw_capsule(&G->cap[i]);
    // The wyrm's body under its head and tail.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            const enemy_t *e = &G->e[i];
            if (e->on && e->t >= 0 && !e->hidden && (e->kind == K_SEG) == (pass == 0))
                draw_enemy(e);
        }
    for (int s = 0; s < MAX_ESHOTS; ++s)
        if (G->es[s].on) {
            int x = scr_x(G->es[s].x), y = (int)G->es[s].y;
            if (y < 2 || y >= PH - 2)
                continue;
            fill(x - 2, y - 1, 4, 2, (G->tick & 4) ? C_PINK : C_ORANGE);
            fill(x - 1, y - 2, 2, 4, (G->tick & 4) ? C_PINK : C_ORANGE);
            pset(x, y, C_WHITE);
        }
    draw_pshots();
    draw_ship();
    for (int i = 0; i < MAX_BOOMS; ++i)
        if (G->boom[i].t >= 0)
            draw_boom(&G->boom[i]);
    // Words.
    char s[40];
    if (G->mode == M_PLAY) {
        if (G->phase == PH_INTRO && G->phase_t > 20) {
            snprintf(s, sizeof s, "STAGE %d", G->stage);
            text_box(90, s, C_CYAN);
            text_box(106, G->st->name, C_YELLOW);
        }
        if (G->phase == PH_WARN && (G->phase_t >> 4) & 1)
            text_box(100, "WARNING", C_RED);
        if (G->phase == PH_CLEAR)
            text_box(96, "STAGE CLEAR", C_CYAN);
        if (G->msg_t && G->msg[0])
            text_box(G->phase == PH_CLEAR ? 112 : 140, G->msg, C_YELLOW);
        if (G->paused)
            text_box(120, "PAUSED", (G->mode_t >> 4) & 1 ? C_WHITE : C_YELLOW);
    } else if (G->mode == M_GAMEOVER) {
        text_box(104, "GAME OVER", C_RED);
    } else if (G->mode == M_ENTRY) {
        fill(40, 50, SW - 80, 120, C_BLACK);
        text_mid(60, "ENTER YOUR INITIALS !", C_RED);
        snprintf(s, sizeof s, "SCORE %lu", (unsigned long)G->score);
        text_mid(80, s, C_YELLOW);
        for (int k = 0; k < 3; ++k) {
            char ch[2] = {G->entry_name[k], 0};
            int x = SW / 2 - 30 + k * 24;
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
    unsigned in = 0, letter_keys = 0;  // letter_keys: WASD, Z X C, N, which also type initials
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
            case HID_KEY_Z: letter_keys |= IN_FIRE; break;
            case HID_KEY_X: case HID_KEY_C: letter_keys |= IN_ORB; break;
            case HID_KEY_N: letter_keys |= IN_MUSIC; break;
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
        if (mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
            in |= IN_ORB;
    }
    uint8_t x, y;
    uint32_t b;
    if (usb_gamepad_read_n(0, &x, &y, &b)) {
        if (x < 64) in |= IN_LEFT;
        if (x > 191) in |= IN_RIGHT;
        if (y < 64) in |= IN_UP;
        if (y > 191) in |= IN_DOWN;
        if (b & 0x05) in |= IN_FIRE;
        if (b & 0x0a) in |= IN_ORB;
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

/// Sets the world up in the framebuffer's free part (pictures, stars,
/// scores, sound).
/// @return false if it doesn't fit (a build mistake)
static bool world_start(void) {
    _Static_assert(sizeof(world_t) <= sizeof dvi_framebuf - 2 * SW * SH - 8, "world_t doesn't fit");
    G = (world_t *)(((uintptr_t)(dvi_framebuf + 2 * SW * SH) + 7) & ~(uintptr_t)7);
    memset(G, 0, sizeof *G);
    if (lsnd_state_size() > sizeof G->snd)
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
        static const uint8_t cols[] = {C_WHITE, RGB332(3, 3, 2), RGB332(2, 2, 2), C_CYAN, RGB332(1, 2, 3),
                                       RGB332(4, 4, 2), C_PINK, RGB332(2, 3, 3)};
        G->star[i] = (star_t){(float)rnd_n(SW), 0.2f + rnd_n(100) / 90.0f, (uint8_t)rnd_n(PH),
                              cols[rnd_n(sizeof cols)]};
    }
    for (int i = 0; i < MAX_BOOMS; ++i)
        G->boom[i].t = -1;
    G->boss.core = -1;
    G->sound = G->music = true;
    G->stage = 1;
    G->st = &stages[0];
    hi_load();
    lsnd_start(G->snd);
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
        audio_set_source(lsnd_source);
        while (!G->quit) {
            read_input();
            update();
            update_things();
            draw();
            present();
        }
        audio_set_source(NULL);
        sleep_ms(3);  // core 1 may still be in lsnd_source
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
    art_get(frame, 0, px);
    for (int i = 0; i < SPR * SPR; ++i)
        on[i] = px[i] != 0;
}

void lancer_command(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "sprites")) {
        const char *path = argc > 2 ? argv[2] : "LANCER.SPR";
        spr_info_t info = {SPR, SPR, ART_COUNT};
        const char *err = NULL;
        if (spr_save(path, &info, spr_get, NULL, &err))
            printf("%s: the game's %d sprites (16x16). Edit them with: sprite %s\n"
                   "Saved as %s at the top of the drive, the game uses them.\n",
                   path, ART_COUNT, path, SPR_FILE + 1);
        else
            printf("lancer: %s: %s\n", path, err ? err : "can't write it");
        return;
    }
    if (argc > 1) {
        printf("usage: lancer              play (Esc on the title screen to leave)\n"
               "       lancer sprites [file.spr]  write the game's sprites for the sprite editor\n");
        return;
    }
    play();
}
