/**
 * @file uitest.c
 * @brief Lancer played on a PC by a simple autopilot: checks the stages' maps, the pod and its three weapons, the charged lance, capsules, the three bosses, a second round, game over and the high-score entry and file, with ASan watching, and writes screenshots, pictures of the stages and the sounds.
 *
 *     ./uitest.sh  (builds this, writes lancer_*.png and lancer_sounds.wav into the folder given)
 *
 * The autopilot steers through the open part of the walls ahead, lines up
 * with what it wants to shoot, picks up capsules, taps fire, now and then
 * charges a full lance, and launches and calls back the pod. It plays the
 * first stretch of the first stage as it is (it may be shot down); after
 * that the test keeps its ship from being shot (not from the walls), so a
 * run gets through all three bosses into the second round. Then it flies
 * into the ceiling until the game is over.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/lancer/lancer.c"

uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];  ///< the screen
volatile bool dvi_line_double, dvi_pixel_double;  ///< doubling
volatile uint16_t dvi_double_top, dvi_double_rows;  ///< the rows shown
stdio_driver_t console_stdio_driver;            ///< the console's driver

static uint32_t frames;     ///< frames shown
static uint32_t us = 1000;  ///< the clock

uint32_t pc_frames(void) { return ++frames; }
uint32_t time_us_32(void) { return us += 7; }
void sleep_ms(uint32_t ms) { us += ms * 1000; }
int getchar_timeout_us(uint32_t t) { (void)t; return -1; }
void console_freeze(bool on) { (void)on; }
void stdio_set_driver_enabled(stdio_driver_t *d, bool on) { (void)d, (void)on; }
void scr_begin(void) {}
void scr_end(void) {}
void mouse_pointer(int mode) { (void)mode; }
bool usb_gamepad_read_n(unsigned n, uint8_t *x, uint8_t *y, uint32_t *b) { (void)n, (void)x, (void)y, (void)b; return false; }
static bool (*source)(int32_t *, int32_t *);  ///< the audio source
void audio_set_source(bool (*fn)(int32_t *left, int32_t *right)) { source = fn; }

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
FRESULT f_stat(const char *path, FILINFO *fi) {
    struct stat st;
    if (stat(local(path), &st))
        return 1;
    fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
    fi->fsize = (unsigned long)st.st_size;
    return FR_OK;
}
void *lodepng_malloc(size_t size) { return malloc(size); }  ///< for spr.c's PNG writer
void *lodepng_realloc(void *p, size_t n) { return realloc(p, n); }  ///< and realloc
void lodepng_free(void *p) { free(p); }  ///< and free

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

/// Writes the picture on the screen as shot_<name>.raw (320x240 RGB332).
/// @param name the shot's name
static void shot(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "shot_%s.raw", name);
    FILE *f = fopen(path, "wb");
    fwrite(dvi_framebuf, 1, SW * SH, f);
    fclose(f);
    printf("frame %6u: shot %s (round %d stage %d, score %lu)\n", frames, name, G->loop + 1, G->stage,
           (unsigned long)G->score);
}

/// What has happened, for the checks and the shots.
static struct {
    bool title0, title1, intro, pod, lance3, launched, recalled, back, blue, yellow, missiles, speed;
    bool boss[4], beaten[4], beam, clear, round2, over, entry, back_to_title, paused, warning;
    int deaths, typed, capsules, max_stage;
    uint32_t pause_at, launch_at, recall_at, back_at, blue_at, yellow_at;
} seen;

static int lances_fired;  ///< lances fired in the game (G->lances, kept)

/// Keys the autopilot holds this frame.
static uint8_t held[6];
/// How many.
static int nheld;
/// Modifier keys held.
static uint8_t held_mods;

/// Holds a key this frame.
/// @param k HID code
static void hold(uint8_t k) {
    if (nheld < 6)
        held[nheld++] = k;
}

/// The danger at a row for the next while: enemy shots and enemies heading there.
/// @param y the row
/// @return a cost
static float danger(float y) {
    float d = 0;
    for (int s = 0; s < MAX_ESHOTS; ++s) {
        const eshot_t *e = &G->es[s];
        if (!e->on)
            continue;
        for (int k = 4; k <= 40; k += 4) {
            float x = e->x + e->dx * k - (G->px + G->scroll_now * k), yy = e->y + e->dy * k;
            if (fabsf(x) < 12 && fabsf(yy - y) < 9)
                d += 300.0f / k;
        }
    }
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        if (!e->on || e->t < 0 || e->hidden)
            continue;
        float dx = e->x - G->px;
        if (dx > -20 && dx < 60 && fabsf(e->y - y) < kinds[e->kind].hh + 10)
            d += 60;
    }
    return d;
}

/// The autopilot: where to go, and fire, charge and pod keys.
static void pilot(void) {
    if (G->pstate != P_ALIVE)
        return;
    // What to line up with: a capsule, else the boss's weak spot, else the nearest enemy ahead.
    float want_y = G->py, want_x = G->cam + 70;
    bool target = false;
    for (int i = 0; i < MAX_CAPS; ++i)
        if (G->cap[i].on && on_screen(G->cap[i].x, G->cap[i].y, -8)) {
            want_y = G->cap[i].y, want_x = G->cap[i].x - 6, target = true;
            break;
        }
    if (!target && G->boss.type != BOSS_NONE && G->boss.core >= 0 && G->e[G->boss.core].on) {
        want_y = G->e[G->boss.core].y;
        want_x = G->cam + (G->boss.type == BOSS_WYRM ? 60 : 90);
        target = true;
        if (G->boss.type == BOSS_BASTION && G->e[G->boss.core].armour) {
            // Its guns first, while the hatch is shut.
            for (int i = 0; i < MAX_ENEMIES; ++i)
                if (G->e[i].on && G->e[i].kind == K_BTURRET) {
                    want_y = G->e[i].y;
                    break;
                }
        }
    }
    if (!target) {
        float best = 1e9f;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            const enemy_t *e = &G->e[i];
            if (!e->on || e->t < 0 || e->hidden || e->armour || !on_screen(e->x, e->y, -4) || e->x < G->px + 20)
                continue;
            float d = e->x - G->px;
            if (d < best)
                best = d, want_y = e->y;
        }
    }
    // The open rows reachable from here, then the best of them a little ahead.
    float lo = G->py, hi = G->py;
    while (lo > 8 && clear_box(G->px, lo - 2, 7, 5))
        lo -= 2;
    while (hi < PH - 8 && clear_box(G->px, hi + 2, 7, 5))
        hi += 2;
    float best_y = G->py, best_c = 1e9f;
    for (float y = lo; y <= hi; y += 2) {
        float c = fabsf(y - want_y) * 0.5f + danger(y);
        for (int ahead = 12; ahead <= 60; ahead += 12)
            if (!clear_box(G->px + ahead, y, 9, 7))
                c += 400.0f / ahead * 12;
        if (G->boss.type == BOSS_BASTION && G->boss.beam_warn | G->boss.beam && G->boss.core >= 0 &&
            fabsf(y - G->e[G->boss.core].y) < 14)
            c += 500;
        if (c < best_c)
            best_c = c, best_y = y;
    }
    if (best_y < G->py - 1)
        hold(HID_KEY_ARROW_UP);
    if (best_y > G->py + 1)
        hold(HID_KEY_ARROW_DOWN);
    if (want_x < G->px - 4 && clear_box(G->px - 6, G->py, 7, 5))
        hold(HID_KEY_ARROW_LEFT);
    if (want_x > G->px + 4 && clear_box(G->px + 14, G->py, 7, 5))
        hold(HID_KEY_ARROW_RIGHT);
    // Fire: taps, and every so often a full charge (two counts of `frames` a game frame).
    int cyc = (int)(frames % 840);
    if (cyc < 190)
        hold(HID_KEY_SPACE);
    else if (frames & 4)
        hold(HID_KEY_SPACE);
    // The pod: out and back now and then.
    if (G->orb == O_FRONT && G->tick % 900 == 300) {
        hold(HID_KEY_X);
        if (!seen.launched)
            seen.launch_at = frames;
        seen.launched = true;
    }
    if (G->orb == O_FREE && G->tick % 900 == 520) {
        held_mods |= KEYBOARD_MODIFIER_LEFTSHIFT;  // Shift works too
        seen.recalled = true;
    }
}

bool usb_keyboard_held_keys(uint8_t keys[6], uint8_t *mods) {
    nheld = 0;
    held_mods = 0;
    // The picture shown last frame is in FRONT: take what is worth a shot.
    if (G) {
        if (G->mode == M_TITLE && !seen.title0 && G->mode_t == 60 && G->title_page == 0)
            shot("title"), seen.title0 = true;
        if (G->mode == M_PLAY) {
            lances_fired = G->lances;
            if (G->stage > seen.max_stage)
                seen.max_stage = G->stage;
            if (G->phase == PH_INTRO && G->phase_t == 120 && G->stage == 1 && !seen.intro)
                shot("intro"), seen.intro = true;
            if (G->orb == O_FRONT && !seen.pod && G->stage == 1)
                seen.pod = true;
            static int pod_frames;
            if (seen.pod && ++pod_frames == 260)
                shot("pod");
            for (int s = 0; s < MAX_PSHOTS; ++s)
                if (G->ps[s].on && G->ps[s].kind == PS_LANCE && G->ps[s].lvl == 3 && G->ps[s].t == 8 && !seen.lance3)
                    shot("lance"), seen.lance3 = true;
            if (seen.launched && G->orb == O_FREE && frames == seen.launch_at + 40)
                shot("launched");
            if (G->missiles)
                seen.missiles = true;
            if (G->speed_lvl)
                seen.speed = true;
            if (G->phase == PH_WARN && G->phase_t == 60 && !seen.warning)
                shot("warning"), seen.warning = true;
            if (G->phase == PH_BOSS && G->boss.type) {
                int b = G->boss.type;
                if (!seen.boss[b] && G->boss.t == (b == BOSS_BASTION ? 330 : 400)) {
                    static const char *const names[4] = {"", "boss1", "boss2", "boss3"};
                    shot(names[b]);
                    seen.boss[b] = true;
                }
                if (b == BOSS_BASTION && G->boss.beam && !seen.beam) {
                    shot("beam");
                    seen.beam = true;
                }
            }
            if (G->phase == PH_CLEAR) {
                seen.beaten[G->stage] = true;
                if (G->phase_t == 60 && !seen.clear)
                    shot("clear"), seen.clear = true;
            }
            if (G->phase == PH_RUN && G->phase_t == 900) {
                static const char *const names[3] = {"stage1", "stage2", "stage3"};
                if (G->loop == 0)
                    shot(names[G->stage - 1]);
            }
            if (G->loop > 0)
                seen.round2 = true;
            if (G->pstate == P_DYING && G->pt == 1)
                ++seen.deaths;
        }
        if (G->mode == M_GAMEOVER && G->mode_t == 100 && !seen.over)
            shot("gameover"), seen.over = true;
        if (G->mode == M_TITLE && seen.entry && G->mode_t == 60 && !seen.back_to_title)
            shot("hiscores"), seen.back_to_title = true;
    }

    *mods = 0;
    if (G) {
        switch (G->mode) {
        case M_TITLE:
            if (seen.back_to_title)
                hold(HID_KEY_ESCAPE);  // done: leave
            else if (G->mode_t == 80)
                hold(HID_KEY_ENTER);
            break;
        case M_PLAY:
            // After the first checkpoint the ship can't be shot down (the walls still count).
            if ((G->stage > 1 || G->check_cam > 0 || G->loop > 0) && !seen.round2)
                G->invuln = G->invuln > 2 ? G->invuln : 2;
            if (seen.round2 && G->stage == 1 && G->phase == PH_RUN && G->phase_t > 400) {
                // The second round: into the ceiling until it's over.
                if (G->pstate == P_ALIVE)
                    hold(HID_KEY_ARROW_UP);
                break;
            }
            // A pause, once.
            if (G->stage == 2 && !seen.paused && G->phase == PH_RUN && G->phase_t == 300) {
                hold(HID_KEY_P);
                seen.paused = true;
                seen.pause_at = frames;
                break;
            }
            if (seen.paused && G->paused && frames == seen.pause_at + 30)
                shot("paused");
            if (seen.paused && G->paused && frames >= seen.pause_at + 60) {
                hold(HID_KEY_P);
                break;
            }
            // The other weapons, for a while each (as if their capsules had been taken).
            if (G->stage == 2 && G->phase == PH_RUN && G->orb == O_FRONT) {
                if (G->phase_t >= 1500 && !seen.blue_at)
                    G->ocol = 1, G->olvl = 2, seen.blue_at = frames;
                if (G->phase_t >= 2600 && seen.blue && !seen.yellow_at)
                    G->ocol = 2, G->olvl = 2, seen.yellow_at = frames;
            }
            if (seen.blue_at && !seen.blue && frames >= seen.blue_at + 500 && G->orb == O_FRONT && G->ocol == 1)
                shot("blue"), seen.blue = true;
            if (seen.yellow_at && !seen.yellow && frames >= seen.yellow_at + 500 && G->orb == O_FRONT && G->ocol == 2)
                shot("yellow"), seen.yellow = true;
            if (G->stage == 3 && G->phase == PH_RUN && G->phase_t >= 600 && !seen.back_at && G->orb == O_FRONT) {
                G->orb = O_BACK;
                seen.back_at = frames;
            }
            if (seen.back_at && !seen.back && frames >= seen.back_at + 200 && G->orb == O_BACK)
                shot("back"), seen.back = true;
            pilot();
            break;
        case M_ENTRY:
            if (G->mode_t == 40)
                shot("entry");
            // Type K, E, N (a key down every 20 frames).
            if (G->mode_t >= 60 && G->mode_t % 20 == 0 && seen.typed < 3) {
                static const uint8_t ken[3] = {0x0e, 0x08, 0x11};  // K, E, N
                hold(ken[seen.typed++]);
            }
            seen.entry = true;
            break;
        default:
            break;
        }
    }
    memset(keys, 0, 6);
    memcpy(keys, held, (size_t)nheld);
    *mods = held_mods;
    if (frames > 600000) {  // a safety stop
        keys[0] = HID_KEY_F10;
        printf("FAIL: still going after %u frames\n", frames);
        ++failures;
    }
    // Some sound each frame, as core 1 would make it.
    if (source) {
        int32_t l, r;
        for (int i = 0; i < 800; ++i)
            source(&l, &r);
    }
    return true;
}

/// Writes a stage's whole map (with its enemies marked) as map_<n>.raw.
/// @param n the stage
static void map_picture(int n) {
    int w = G->map_w * TILE;
    uint8_t *pic = calloc((size_t)w * PH, 1);
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < w; ++x) {
            int l = G->map[y >> 3][x >> 3];
            if (l && shape_px(look_shape(l), x & 7, y & 7))
                pic[y * w + x] = G->tile[l][(y & 7) * TILE + (x & 7)];
        }
    for (const event_t *ev = G->st->ev; ev->x; ++ev) {
        uint8_t c = ev->kind == EV_CHECK ? C_GREEN : C_RED;
        int y0 = ev->kind == EV_CHECK ? 0 : ev->kind == K_TURRET || ev->kind == K_HOPPER || ev->kind == K_HATCH
                                                ? (ev->p ? (int)ceil_y(ev->x) : (int)floor_y(ev->x) - 4)
                                                : ev->y;
        for (int k = 0; k < (ev->kind == EV_CHECK ? PH : 4); ++k)
            for (int j = -1; j <= 1; ++j)
                if (y0 + k < PH && ev->x + j < w)
                    pic[(y0 + k) * w + ev->x + j] = c;
    }
    char path[64];
    snprintf(path, sizeof path, "map_%d_%dx%d.raw", n, w, PH);
    FILE *f = fopen(path, "wb");
    fwrite(pic, 1, (size_t)w * PH, f);
    fclose(f);
    free(pic);
}

/// Checks each stage's map: a way through every column, things on the
/// ground on real ground, open room where a ship starts again.
static void check_maps(void) {
    static uint8_t mem[sizeof(world_t) + 8];
    G = (world_t *)(((uintptr_t)mem + 7) & ~(uintptr_t)7);
    memset(G, 0, sizeof *G);
    for (int i = 0; i < 256; ++i)
        G->sin_t[i] = sinf(i * (6.2831853f / 256.0f));
    G->rng = 1;
    for (int n = 1; n <= 3; ++n) {
        G->stage = n;
        G->st = &stages[n - 1];
        build_map(G->st);
        make_tiles(G->st->theme);
        CHECK(G->map_w * TILE >= SW * 4, "stage %d is only %d columns", n, G->map_w);
        int narrowest = PH, at = 0;
        for (int x = 0; x < G->map_w * TILE; x += 4) {
            int run = 0, longest = 0;
            for (int y = 0; y < PH; ++y) {
                run = solid((float)x, (float)y) ? 0 : run + 1;
                if (run > longest)
                    longest = run;
            }
            if (longest < narrowest)
                narrowest = longest, at = x;
        }
        printf("stage %d: %d columns, narrowest gap %d px at x %d\n", n, G->map_w, narrowest, at);
        CHECK(narrowest >= 40, "stage %d: a gap of %d px at x %d", n, narrowest, at);
        for (const event_t *ev = G->st->ev; ev->x; ++ev) {
            if (ev->kind == K_TURRET || ev->kind == K_HOPPER || ev->kind == K_HATCH) {
                float y, x = ground_spot(ev->x, ev->p ? -1 : 1, &y);
                CHECK(y > 8 && y < PH - 8 && clear_box(x, y, 5, 5),
                      "stage %d: the %s at x %d has no ground (row %.0f)", n, ev->p ? "ceiling unit" : "floor unit",
                      ev->x, y);
            } else if (ev->kind == EV_CHECK) {
                float cam = ev->x - SW - 24;
                float y = open_row(cam + SHIP_X);
                CHECK(clear_box(cam + SHIP_X, y, 12, 10), "stage %d: no room to start again at x %d", n, ev->x);
            } else {
                CHECK(ev->y > 10 && ev->y < PH - 10, "stage %d: event at x %d flies at row %d", n, ev->x, ev->y);
            }
        }
        map_picture(n);
    }
    G = NULL;
}

/// Writes the sound effects and tunes, one after another, as a 48 kHz mono WAV.
/// @param path the file
static void write_sounds(const char *path) {
    static uint8_t mem[2048];
    lsnd_start(mem);
    int each = 48000 * 2, total = (FX_COUNT + TUNE_COUNT) * each + 48000 * 12;
    int16_t *pcm = calloc((size_t)total, 2);
    int n = 0, loud = 0;
    for (int k = 0; k < FX_COUNT + TUNE_COUNT; ++k) {
        if (k < FX_COUNT)
            lsnd_fx(k);
        else
            lsnd_tune(k - FX_COUNT);
        int len = k == FX_COUNT + TUNE_STAGE1 ? each + 48000 * 12 : each;  // more of the first theme
        int32_t l, r;
        for (int i = 0; i < len && n < total; ++i) {
            lsnd_source(&l, &r);
            if (k == FX_CHARGE && i == 48000 * 3 / 2)
                lsnd_fx(FX_LANCE);
            pcm[n++] = (int16_t)l;
            loud += l > 2000 || l < -2000;
        }
        lsnd_fx_stop(FX_CHARGE);
        lsnd_fx_stop(FX_WARNING);
        lsnd_fx_stop(FX_BEAM);
        lsnd_tune(-1);
    }
    CHECK(loud > 48000, "the sounds are too quiet (%d loud samples)", loud);
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)n * 2, riff = 36 + data, rate = 48000, brate = 96000, fmt = 16;
    uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm_fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
    free(pcm);
}

int main(void) {
    remove("LANCER.HI");
    remove("LANCER.SPR");
    // Every art row is 16 letters.
    for (int a = 0; a < ART_COUNT; ++a)
        for (int r = 0; r < 16; ++r)
            CHECK(strlen(art_rows[a][r]) == 16, "art %d row %d is %zu long", a, r, strlen(art_rows[a][r]));
    // The parts of a tune that loops stay together.
    for (int t = 0; t < TUNE_COUNT; ++t) {
        int longest = 0;
        for (int p = 0; p < 3; ++p)
            if (lsnd_part_steps(t, p) > longest)
                longest = lsnd_part_steps(t, p);
        for (int p = 0; p < 3; ++p) {
            int n = lsnd_part_steps(t, p);
            if (lsnd_tune_loops(t) && n > 0)
                CHECK(longest % n == 0, "tune %d: part %d is %d steps, the longest %d", t, p, n, longest);
        }
    }
    check_maps();
    // The sprites written for the sprite editor read back.
    char *argv[] = {"lancer", "sprites", "SPRITES.SPR"};
    lancer_command(3, argv);
    FILINFO fi;
    CHECK(f_stat("/SPRITES.SPR", &fi) == FR_OK && fi.fsize == 8 + ART_COUNT * (256 + 32),
          "SPRITES.SPR is %lu bytes", (unsigned long)fi.fsize);

    char *play_argv[] = {"lancer"};
    lancer_command(1, play_argv);

    printf("%u frames; reached stage %d; %d ships lost; %d lances; blue %d yellow %d back %d\n", frames, seen.max_stage, seen.deaths, lances_fired, seen.blue, seen.yellow, seen.back);
    CHECK(seen.title0, "no title screen");
    CHECK(seen.intro, "no stage intro");
    CHECK(seen.pod, "never got the pod");
    CHECK(seen.lance3, "no full lance");
    CHECK(seen.launched && seen.recalled, "the pod wasn't launched and called back");
    CHECK(seen.speed && seen.missiles, "no speed-up or missiles");
    CHECK(seen.blue && seen.yellow && seen.back, "not every weapon was seen");
    CHECK(seen.warning, "no boss warning");
    for (int b = 1; b <= 3; ++b) {
        CHECK(seen.boss[b], "boss %d never seen", b);
        CHECK(seen.beaten[b], "boss %d never beaten", b);
    }
    CHECK(seen.round2, "never reached the second round");
    CHECK(seen.deaths >= 3, "only %d ships lost", seen.deaths);
    CHECK(seen.over && seen.entry, "no game over / entry");
    CHECK(seen.back_to_title, "not back to the title");
    CHECK(!dvi_line_double && !dvi_pixel_double, "the screen wasn't given back");
    FILE *f = fopen("LANCER.HI", "r");
    char table[200] = "";
    CHECK(f && fread(table, 1, sizeof table - 1, f) > 0, "LANCER.HI not written");
    if (f)
        fclose(f);
    CHECK(strstr(table, " KEN\n") != NULL, "KEN isn't in LANCER.HI:\n%s", table);
    printf("LANCER.HI:\n%s", table);
    write_sounds("sounds.wav");
    printf(failures ? "%d FAILED\n" : "all fine\n", failures);
    return failures != 0;
}
