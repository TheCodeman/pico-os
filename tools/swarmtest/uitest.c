/**
 * @file uitest.c
 * @brief Swarm played on a PC by a simple autopilot: checks the stages, the tractor beam, the capture, the rescue and twin fighter, a challenge stage, game over and the high-score entry and file, with ASan watching, and writes screenshots and the sounds.
 *
 *     ./uitest.sh  (builds this, writes swarm_*.png and swarm_sounds.wav into the folder given)
 *
 * The autopilot fires all the time, dodges shots, and flies under the first
 * tractor beam on purpose. To be sure of a rescue it sends the flagship
 * holding the fighter on a dive itself once the fighter is lost.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "../../src/swarm/swarm.c"

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
    printf("frame %6u: shot %s (stage %d, score %lu)\n", frames, name, G->stage, (unsigned long)G->score);
}

/// What has happened, for the checks and the shots.
static struct {
    bool title0, title1, entering, formation, dive, beam, captured, rescued, dual, challenge, bonus;
    bool over, results, entry, back_to_title, paused;
    int max_stage, captures, deaths, typed;
    uint32_t pause_at;
} seen;

/// Keys the autopilot holds this frame.
static uint8_t held[6];
/// How many.
static int nheld;

/// Holds a key this frame.
/// @param k HID code
static void hold(uint8_t k) {
    if (nheld < 6)
        held[nheld++] = k;
}

/// The autopilot: where to go and whether to fire.
static void pilot(void) {
    float target = PF_W / 2.0f;
    bool want_capture = seen.captures == 0;
    int beam_boss = -1, rescue = -1;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (G->e[i].state == E_BEAM)
            beam_boss = i;
        if (G->e[i].state == E_RESCUE)
            rescue = i;
    }
    if (G->pstate != P_ALIVE)
        return;
    if (want_capture && beam_active()) {
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].state != E_NONE && G->e[i].beam)
                target = G->e[i].x;
    } else if (rescue >= 0) {
        target = G->px;  // keep still and let it dock
    } else {
        // Under the lowest alien that is diving, else one in the formation.
        float best = -1;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            const enemy_t *e = &G->e[i];
            if (e->state == E_NONE || e->state == E_WAIT || e->state == E_CAPTIVE || e->y < 0)
                continue;
            float score = e->y + (diving(e) ? 200 : 0);
            if (score > best) {
                best = score;
                target = e->x;
            }
        }
        if (beam_boss >= 0 && fabsf(target - G->e[beam_boss].x) < 30)
            target = G->e[beam_boss].x + (G->e[beam_boss].x > PF_W / 2 ? -50 : 50);
    }
    // Dodge shots and divers coming at the fighter.
    float me = aim_x();
    for (int s = 0; s < MAX_ESHOTS; ++s) {
        const shot_t *sh = &G->es[s];
        if (!sh->on || sh->y < 140 || sh->y > PLAYER_Y + 4)
            continue;
        float at = sh->x + sh->dx * (PLAYER_Y - sh->y) / sh->dy;
        if (fabsf(at - me) < 14)
            target = me + (at > me ? -40 : 40);
    }
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const enemy_t *e = &G->e[i];
        if (diving(e) && e->state != E_BEAM && e->y > 150 && fabsf(e->x - me) < 18)
            target = me + (e->x > me ? -40 : 40);
    }
    if (target < me - 1.5f)
        hold(HID_KEY_ARROW_LEFT);
    if (target > me + 1.5f)
        hold(HID_KEY_ARROW_RIGHT);
    // Holds fire while the first stages fly in, so flagships live to use the beam.
    if ((frames & 2) && (G->attack || G->stage > 2) && !(want_capture && beam_active()))
        hold(HID_KEY_SPACE);
}

bool usb_keyboard_held_keys(uint8_t keys[6], uint8_t *mods) {
    // The picture shown last frame is in FRONT: take what is worth a shot.
    if (G) {
        if (G->mode == M_TITLE && !seen.title0 && G->mode_t == 40 && G->title_page == 0)
            shot("title"), seen.title0 = true;
        if (G->mode == M_PLAY && G->stage == 1 && G->phase == PH_WAVES && G->phase_t == 330 && !seen.entering)
            shot("entering"), seen.entering = true;
        if (G->mode == M_PLAY && G->attack && !seen.formation && G->stage == 1)
            shot("formation"), seen.formation = true;
        if (G->mode == M_PLAY && !seen.dive && divers() >= 2) {
            int n = 0;
            for (int s = 0; s < MAX_ESHOTS; ++s)
                n += G->es[s].on;
            if (n >= 1)
                shot("dive"), seen.dive = true;
        }
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (G->e[i].state == E_BEAM && G->e[i].timer == 120 && !seen.beam)
                shot("beam"), seen.beam = true;
        if (G->pstate == P_GONE && G->pt == 1) {
            ++seen.captures;
            if (!seen.captured)
                shot("captured"), seen.captured = true;
        }
        if (G->pstate == P_DYING && G->pt == 1)
            ++seen.deaths;
        if (G->dual && !seen.dual) {
            seen.dual = true;
            seen.rescued = true;
        }
        static int dual_frames;
        if (G->dual && ++dual_frames == 200)
            shot("dual");
        if (G->mode == M_PLAY && G->challenge && G->phase == PH_WAVES && G->phase_t == 420 && !seen.challenge)
            shot("challenge"), seen.challenge = true;
        if (G->mode == M_PLAY && G->phase == PH_BONUS && G->phase_t == 200 && !seen.bonus)
            shot("bonus"), seen.bonus = true;
        if (G->mode == M_GAMEOVER && G->mode_t == 100 && !seen.over)
            shot("gameover"), seen.over = true;
        if (G->mode == M_RESULTS && G->mode_t == 100 && !seen.results)
            shot("results"), seen.results = true;
        if (G->mode == M_TITLE && seen.entry && G->mode_t == 60 && !seen.back_to_title)
            shot("hiscores"), seen.back_to_title = true;
        if (G->stage > seen.max_stage)
            seen.max_stage = G->stage;
    }

    nheld = 0;
    *mods = 0;
    if (G) {
        switch (G->mode) {
        case M_TITLE:
            if (seen.back_to_title)
                hold(HID_KEY_ESCAPE);  // done: leave
            else if (G->mode_t == 60)
                hold(HID_KEY_ENTER);
            break;
        case M_PLAY:
            // Once the fighter is lost, send its flagship diving to be shot.
            if (seen.captures && !seen.rescued) {
                for (int i = 0; i < MAX_ENEMIES; ++i) {
                    enemy_t *e = &G->e[i];
                    if (e->kind == K_BOSS && e->link >= 0 && e->state == E_FORM && G->pstate == P_ALIVE &&
                        G->e[e->link].state == E_CAPTIVE)
                        start_dive(i, false);
                }
            }
            // A pause, once.
            if (G->stage == 2 && !seen.paused && G->phase == PH_WAVES && G->phase_t == 100) {
                hold(HID_KEY_P);
                seen.paused = true;
                seen.pause_at = frames;
            }
            if (seen.paused && G->paused && frames == seen.pause_at + 30)
                shot("paused");
            if (seen.paused && G->paused && frames >= seen.pause_at + 60) {
                hold(HID_KEY_P);
                break;
            }
            // From stage 6 on, stop dodging so the game ends.
            if (G->stage < 6)
                pilot();
            else if (frames & 2)
                hold(HID_KEY_SPACE);
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
    if (frames > 400000) {  // a safety stop
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

/// Writes the sound effects and tunes, one after another, as a 48 kHz mono WAV.
/// @param path the file
static void write_sounds(const char *path) {
    static uint8_t mem[2048];
    snd_start(mem);
    int total = (FX_COUNT + TUNE_COUNT) * 48000 * 3 / 2;
    int16_t *pcm = calloc((size_t)total, 2);
    int n = 0, loud = 0;
    for (int k = 0; k < FX_COUNT + TUNE_COUNT; ++k) {
        if (k < FX_COUNT)
            snd_fx(k);
        else
            snd_tune(k - FX_COUNT);
        int32_t l, r;
        for (int i = 0; i < 48000 * 3 / 2; ++i) {
            snd_source(&l, &r);
            pcm[n++] = (int16_t)l;
            loud += l > 2000 || l < -2000;
        }
        snd_fx_stop(FX_BEAM);
        snd_tune(-1);
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
    remove("SWARM.HI");
    remove("SWARM.SPR");
    // The sprites written for the sprite editor read back.
    char *argv[] = {"swarm", "sprites", "SPRITES.SPR"};
    swarm_command(3, argv);
    FILINFO fi;
    CHECK(f_stat("/SPRITES.SPR", &fi) == FR_OK && fi.fsize == 8 + ART_COUNT * (256 + 32),
          "SPRITES.SPR is %lu bytes", (unsigned long)fi.fsize);

    char *play_argv[] = {"swarm"};
    swarm_command(1, play_argv);

    printf("%u frames; reached stage %d; %d fighters shot down, %d captured\n", frames, seen.max_stage,
           seen.deaths, seen.captures);
    CHECK(seen.title0, "no title screen");
    CHECK(seen.formation, "the formation never finished");
    CHECK(seen.dive, "no dive with shots");
    CHECK(seen.beam, "no tractor beam");
    CHECK(seen.captures > 0, "never captured");
    CHECK(seen.rescued, "never rescued (no twin fighter)");
    CHECK(seen.challenge && seen.bonus, "no challenge stage");
    CHECK(seen.max_stage >= 4, "only reached stage %d", seen.max_stage);
    CHECK(seen.over && seen.results && seen.entry, "no game over / results / entry");
    CHECK(seen.back_to_title, "not back to the title");
    CHECK(!dvi_line_double && !dvi_pixel_double, "the screen wasn't given back");
    FILE *f = fopen("SWARM.HI", "r");
    char table[200] = "";
    CHECK(f && fread(table, 1, sizeof table - 1, f) > 0, "SWARM.HI not written");
    if (f)
        fclose(f);
    CHECK(strstr(table, " KEN\n") != NULL, "KEN isn't in SWARM.HI:\n%s", table);
    printf("SWARM.HI:\n%s", table);
    write_sounds("sounds.wav");
    printf(failures ? "%d FAILED\n" : "all fine\n", failures);
    return failures != 0;
}
