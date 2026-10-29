/**
 * @file lancer_snd.c
 * @brief Lancer's synthesizer, sound effects and music (see lancer_snd.h).
 *
 * Eight voices. Voices 0-4 are the sound effects' (each effect has its own
 * one or two), 5-7 the music's. A voice is a waveform at a pitch that can
 * sweep up or down (a factor each millisecond), wobble (vibrato) and fade;
 * noise is a 15-bit shift register stepped at the voice's pitch. Core 0
 * hands a voice its new settings through a request slot and a count it
 * bumps last; core 1 picks requests up once a millisecond, when it also
 * moves the sweeps, fades and the music on. In a tune that loops each part
 * starts over by itself when it ends, so a short bass line can repeat under
 * a long melody (the parts' lengths divide the longest one's).
 *
 * The effects and the music were made up for this game.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include "lancer_snd.h"

#include <math.h>
#include <string.h>

/// The output rate.
#define RATE 48000
/// Samples in a millisecond: the control rate.
#define TICK (RATE / 1000)
/// Voices in all, and the first of the music's.
///@{
#define VOICES 8
#define MUSIC_VOICE 5
///@}
/// A voice at full volume, in sample units (eight of them can still add up past 16 bits, so the sum is clipped).
#define AMP 10000

/// Waveforms.
enum { W_OFF, W_SQUARE, W_PULSE, W_TRIANGLE, W_SAW, W_NOISE };

/// One voice's settings and where it is.
typedef struct {
    uint8_t wave;        ///< W_ value (W_OFF: silent)
    uint32_t phase;      ///< where in the wave (a full turn is 2^32)
    uint32_t inc;        ///< phase step a sample: the pitch
    uint32_t mul;        ///< the pitch's factor each millisecond, 16.16 (65536: steady)
    uint32_t inc_min;    ///< the sweep stops here (going down)
    uint32_t inc_max;    ///< or here (going up)
    int32_t vol;         ///< loudness, 0-65536
    int32_t fade;        ///< taken off vol each millisecond
    int32_t ms;          ///< milliseconds left (then it stops)
    uint16_t vib_depth;  ///< vibrato: how far the pitch swings, 1/65536ths of it
    uint16_t vib_phase;  ///< where the vibrato is (65536 a turn)
    uint16_t vib_step;   ///< how far it goes each millisecond
    uint16_t lfsr;       ///< the noise's shift register
    int16_t noise;       ///< the noise's level now (+1 or -1)
} voice_t;

/// A request from core 0 for a voice.
typedef struct {
    volatile uint32_t seq;  ///< bumped after v is written
    voice_t v;              ///< the voice's new settings
} request_t;

/// A note of a tune: MIDI note (0: rest) and steps.
typedef struct {
    uint8_t note, steps;
} note_t;

/// Where a music voice is in its part of the tune.
typedef struct {
    const note_t *at;     ///< the next note (NULL: the part has ended)
    const note_t *first;  ///< the part's first note (to loop back to)
    int32_t left;         ///< milliseconds of the note now playing
} part_t;

/// The whole state (in the framebuffer's free part while the game runs).
typedef struct {
    voice_t v[VOICES];          ///< core 1's voices
    request_t req[VOICES];      ///< core 0's requests
    uint32_t seen[VOICES];      ///< the requests core 1 has taken
    volatile uint32_t tune_seq; ///< bumped for a new tune
    volatile int tune_want;     ///< the tune asked for (-1: stop)
    uint32_t tune_seen;         ///< the tune request core 1 has taken
    part_t part[3];             ///< the tune's three parts
    int tune_step_ms;           ///< milliseconds a step
    bool tune_loop;             ///< the tune loops
    volatile bool tune_on;      ///< a tune plays
    volatile bool enabled;      ///< sound is on
    volatile bool music;        ///< music is on
    int tick;                   ///< samples to the next millisecond
} snd_t;

/// The state (NULL: none).
static snd_t *S;

size_t lsnd_state_size(void) {
    return sizeof(snd_t);
}

void lsnd_start(void *mem) {
    memset(mem, 0, sizeof(snd_t));
    S = mem;
    S->enabled = true;
    S->music = true;
    S->tune_want = -1;
    for (int i = 0; i < VOICES; ++i)
        S->v[i].lfsr = S->req[i].v.lfsr = 0x4a3d + i;
}

void lsnd_enable(bool on) {
    if (!S)
        return;
    S->enabled = on;
    if (!on) {
        for (int i = 0; i < VOICES; ++i)
            lsnd_fx_stop(-1 - i);
        lsnd_tune(-1);
    }
}

void lsnd_music(bool on) {
    if (!S)
        return;
    if (!on)
        lsnd_tune(-1);
    S->music = on;
}

// ---- Core 0: asking for sounds ---------------------------------------------------------------------

/// One part of a sound effect.
typedef struct {
    uint8_t voice;   ///< which voice
    uint8_t wave;    ///< W_ value
    float f0, f1;    ///< pitch at the start and the end, Hz (noise: how fast it changes)
    float vol;       ///< loudness at the start, 0-1
    uint16_t ms;     ///< how long
    uint16_t sweep;  ///< how long the pitch takes to go from f0 to f1 (0: ms)
    float vib_hz;    ///< vibrato speed (0: none)
    float vib;       ///< and depth, a fraction of the pitch
    bool hold;       ///< no fade: full loudness to the end
} fx_part_t;

/// The sound effects: up to two parts each (the second unused when its ms is 0).
static const fx_part_t fx_table[FX_COUNT][2] = {
    [FX_SHOT] = {{0, W_PULSE, 2400, 700, 0.30f, 70, 0, 0, 0, false}},
    [FX_LASER] = {{3, W_SQUARE, 1300, 2700, 0.16f, 60, 0, 0, 0, false}},
    [FX_CHARGE] = {{4, W_SAW, 150, 900, 0.20f, 6000, 1100, 14, 0.06f, true}},
    [FX_LANCE] = {{4, W_SAW, 1000, 80, 0.50f, 480, 0, 0, 0, false},
                  {2, W_NOISE, 7000, 1200, 0.40f, 320, 0, 0, 0, false}},
    [FX_HIT] = {{1, W_NOISE, 8000, 900, 0.60f, 180, 0, 0, 0, false},
                {2, W_SQUARE, 650, 110, 0.20f, 110, 0, 0, 0, false}},
    [FX_ARMOUR] = {{1, W_SQUARE, 1900, 1700, 0.16f, 45, 0, 0, 0, false}},
    [FX_BOOM] = {{1, W_NOISE, 6000, 380, 0.85f, 480, 0, 0, 0, false},
                 {2, W_SAW, 300, 40, 0.40f, 420, 0, 0, 0, false}},
    [FX_PLAYER_BOOM] = {{2, W_NOISE, 5000, 150, 1.0f, 1400, 0, 0, 0, false},
                        {4, W_SAW, 200, 25, 0.50f, 1200, 0, 8, 0.12f, false}},
    [FX_ORB] = {{3, W_TRIANGLE, 300, 1400, 0.35f, 180, 0, 0, 0, false}},
    [FX_ATTACH] = {{3, W_SQUARE, 900, 1800, 0.28f, 90, 0, 0, 0, false}},
    [FX_POWER] = {{3, W_SQUARE, 600, 2400, 0.32f, 380, 0, 18, 0.05f, false}},
    [FX_MISSILE] = {{3, W_NOISE, 3000, 1200, 0.28f, 150, 0, 0, 0, false}},
    [FX_WARNING] = {{4, W_SQUARE, 640, 640, 0.26f, 2600, 0, 2.5f, 0.30f, true}},
    [FX_BEAM] = {{2, W_SAW, 110, 90, 0.45f, 900, 0, 30, 0.20f, true},
                 {4, W_NOISE, 2500, 2500, 0.28f, 900, 0, 0, 0, true}},
    [FX_EXTRA] = {{3, W_PULSE, 1000, 3000, 0.35f, 500, 0, 12, 0.10f, false}},
    [FX_COIN] = {{0, W_SQUARE, 1320, 1760, 0.30f, 70, 0, 0, 0, false}},
};

/// The phase step for a pitch.
/// @param hz the pitch
/// @return phase step a sample
static uint32_t inc_of(float hz) {
    double inc = hz * 4294967296.0 / RATE;
    return inc > 4.2e9 ? 4200000000u : (uint32_t)inc;
}

/// Hands voice `n` its new settings.
/// @param n the voice
/// @param v the settings
static void request(int n, const voice_t *v) {
    request_t *r = &S->req[n];
    r->v = *v;
    __sync_synchronize();
    r->seq++;
}

void lsnd_fx(int fx) {
    if (!S || !S->enabled || fx < 0 || fx >= FX_COUNT)
        return;
    for (int i = 0; i < 2; ++i) {
        const fx_part_t *p = &fx_table[fx][i];
        if (!p->ms)
            continue;
        voice_t v;
        memset(&v, 0, sizeof v);
        v.wave = p->wave;
        v.inc = inc_of(p->f0);
        unsigned sweep = p->sweep ? p->sweep : p->ms;
        v.mul = (uint32_t)(powf(p->f1 / p->f0, 1.0f / (float)sweep) * 65536.0f + 0.5f);
        uint32_t a = inc_of(p->f0), b = inc_of(p->f1);
        v.inc_min = a < b ? a : b;
        v.inc_max = a < b ? b : a;
        v.vol = (int32_t)(p->vol * 65536.0f);
        v.fade = p->hold ? 0 : v.vol / p->ms;
        v.ms = p->ms;
        v.vib_depth = (uint16_t)(p->vib * 65535.0f);
        v.vib_step = (uint16_t)(p->vib_hz * 65536.0f / 1000.0f);
        v.lfsr = (uint16_t)(0x1234 + fx * 77 + i);
        request(p->voice, &v);
    }
}

void lsnd_fx_stop(int fx) {
    if (!S)
        return;
    voice_t v;
    memset(&v, 0, sizeof v);
    if (fx < 0) {  // -1 - voice: that voice
        request(-1 - fx, &v);
        return;
    }
    if (fx >= FX_COUNT)
        return;
    for (int i = 0; i < 2; ++i)
        if (fx_table[fx][i].ms)
            request(fx_table[fx][i].voice, &v);
}

// ---- The music ----------------------------------------------------------------------------------------

/// MIDI note names used in the tunes.
enum {
    C2 = 36, D2 = 38, E2 = 40, F2 = 41, G2 = 43, A2 = 45, B2 = 47,
    C3 = 48, D3 = 50, E3 = 52, F3 = 53, G3 = 55, A3 = 57, B3 = 59,
    C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71,
    C5 = 72, D5 = 74, E5 = 76, F5 = 77, G5 = 79, A5 = 81, B5 = 83,
    C6 = 84, D6 = 86, E6 = 88, F6 = 89, G6 = 91, A6 = 93, C7 = 96, R = 0
};
/// The end of a part.
#define END {255, 0}

/// A tune: milliseconds a step, whether it loops, and three parts (lead, harmony, bass).
typedef struct {
    int step_ms;            ///< milliseconds a step
    bool loop;              ///< starts over at the end
    const note_t *part[3];  ///< the parts (NULL: silent)
} tune_t;

// A new game: up the A minor chord and away.
static const note_t start_lead[] = {{A4, 1}, {C5, 1}, {E5, 1}, {A5, 3}, {G5, 1}, {A5, 1}, {C6, 1}, {E6, 5}, END};
static const note_t start_harm[] = {{E4, 3}, {A4, 3}, {E5, 3}, {A5, 5}, END};
static const note_t start_bass[] = {{A2, 3}, {E3, 3}, {A3, 3}, {A2, 5}, END};

// Stage 1, the rubble belt: A minor, driving octaves under a climbing tune.
static const note_t s1_lead[] = {
    {A4, 2}, {C5, 2}, {E5, 4}, {D5, 2}, {C5, 2}, {B4, 2}, {C5, 2},   // Am
    {A4, 4}, {F5, 4}, {E5, 2}, {D5, 2}, {C5, 4},                     // F
    {B4, 2}, {D5, 2}, {G5, 4}, {F5, 2}, {E5, 2}, {D5, 2}, {B4, 2},   // G
    {E5, 6}, {D5, 2}, {B4, 4}, {G4, 4},                              // E
    {A4, 2}, {C5, 2}, {E5, 4}, {A5, 4}, {G5, 2}, {E5, 2},            // Am
    {F5, 4}, {A5, 4}, {G5, 2}, {F5, 2}, {E5, 2}, {C5, 2},            // F
    {E5, 2}, {G5, 2}, {C6, 4}, {B5, 2}, {A5, 2}, {G5, 2}, {E5, 2},   // C
    {B4, 2}, {E5, 2}, {G5, 2}, {B5, 2}, {E5, 8},                     // E
    END};
static const note_t s1_harm[] = {
    {E4, 8}, {A4, 8}, {F4, 8}, {A4, 8}, {G4, 8}, {B4, 8}, {E4, 8}, {B4, 8},
    {E4, 8}, {C5, 8}, {F4, 8}, {C5, 8}, {E4, 8}, {G4, 8}, {B4, 8}, {E4, 8},
    END};
static const note_t s1_bass[] = {
    {A2, 2}, {A2, 2}, {A3, 2}, {A2, 2}, {E3, 2}, {A2, 2}, {A3, 2}, {E3, 2},
    {F2, 2}, {F2, 2}, {F3, 2}, {F2, 2}, {C3, 2}, {F2, 2}, {F3, 2}, {C3, 2},
    {G2, 2}, {G2, 2}, {G3, 2}, {G2, 2}, {D3, 2}, {G2, 2}, {G3, 2}, {D3, 2},
    {E2, 2}, {E2, 2}, {E3, 2}, {E2, 2}, {B2, 2}, {E2, 2}, {E3, 2}, {B2, 2},
    {A2, 2}, {A2, 2}, {A3, 2}, {A2, 2}, {E3, 2}, {A2, 2}, {A3, 2}, {E3, 2},
    {F2, 2}, {F2, 2}, {F3, 2}, {F2, 2}, {C3, 2}, {F2, 2}, {F3, 2}, {C3, 2},
    {C3, 2}, {C3, 2}, {C4, 2}, {C3, 2}, {G3, 2}, {C3, 2}, {C4, 2}, {G3, 2},
    {E2, 2}, {E2, 2}, {E3, 2}, {E2, 2}, {B2, 2}, {E2, 2}, {E3, 2}, {B2, 2},
    END};

// Stage 2, the iron citadel: D dorian, clipped and mechanical.
static const note_t s2_lead[] = {
    {D5, 2}, {R, 2}, {D5, 1}, {E5, 1}, {F5, 2}, {A5, 4}, {G5, 2}, {F5, 2},
    {E5, 2}, {D5, 2}, {C5, 2}, {D5, 2}, {A4, 8},
    {C5, 2}, {R, 2}, {C5, 1}, {D5, 1}, {E5, 2}, {G5, 4}, {F5, 2}, {E5, 2},
    {D5, 2}, {B4, 2}, {G4, 4}, {B4, 4}, {D5, 4},
    {D5, 2}, {R, 2}, {D5, 1}, {E5, 1}, {F5, 2}, {A5, 4}, {B5, 2}, {C6, 2},
    {D6, 4}, {C6, 2}, {A5, 2}, {F5, 4}, {A5, 4},
    {G5, 2}, {F5, 2}, {E5, 2}, {C5, 2}, {E5, 2}, {G5, 2}, {C6, 4},
    {B5, 4}, {A5, 2}, {G5, 2}, {D5, 8},
    END};
static const note_t s2_harm[] = {
    {R, 2}, {A4, 2}, {R, 2}, {A4, 2}, {R, 2}, {F4, 2}, {R, 2}, {F4, 2},
    {R, 2}, {A4, 2}, {R, 2}, {A4, 2}, {R, 2}, {F4, 2}, {R, 2}, {F4, 2},
    {R, 2}, {G4, 2}, {R, 2}, {G4, 2}, {R, 2}, {E4, 2}, {R, 2}, {E4, 2},
    {R, 2}, {B4, 2}, {R, 2}, {B4, 2}, {R, 2}, {G4, 2}, {R, 2}, {D5, 2},
    END};
static const note_t s2_bass[] = {
    {D3, 1}, {D3, 1}, {R, 1}, {D3, 1}, {C3, 2}, {D3, 2}, {F3, 1}, {D3, 1}, {R, 1}, {D3, 1}, {A2, 2}, {C3, 2},
    {D3, 1}, {D3, 1}, {R, 1}, {D3, 1}, {C3, 2}, {D3, 2}, {F3, 1}, {D3, 1}, {R, 1}, {D3, 1}, {A2, 2}, {C3, 2},
    {C3, 1}, {C3, 1}, {R, 1}, {C3, 1}, {B2, 2}, {C3, 2}, {E3, 1}, {C3, 1}, {R, 1}, {C3, 1}, {G2, 2}, {B2, 2},
    {G2, 1}, {G2, 1}, {R, 1}, {G2, 1}, {F2, 2}, {G2, 2}, {B2, 1}, {G2, 1}, {R, 1}, {G2, 1}, {D3, 2}, {F2, 2},
    END};

// Stage 3, the hive: E phrygian, slow and uneasy over a rocking bass.
static const note_t s3_lead[] = {
    {E5, 4}, {F5, 4}, {E5, 2}, {D5, 2}, {C5, 4},
    {B4, 8}, {R, 4}, {A4, 2}, {B4, 2},
    {C5, 4}, {D5, 2}, {E5, 2}, {F5, 4}, {G5, 2}, {F5, 2},
    {E5, 12}, {R, 4},
    {G5, 4}, {A5, 4}, {G5, 2}, {F5, 2}, {E5, 4},
    {F5, 4}, {E5, 2}, {D5, 2}, {C5, 4}, {B4, 4},
    {A4, 2}, {B4, 2}, {C5, 2}, {D5, 2}, {E5, 2}, {F5, 2}, {G5, 2}, {A5, 2},
    {B5, 8}, {F5, 4}, {E5, 4},
    END};
static const note_t s3_harm[] = {{B3, 16}, {C4, 16}, {A3, 16}, {B3, 16}, END};
static const note_t s3_bass[] = {{E2, 6}, {F2, 2}, {E2, 4}, {G2, 4}, {E2, 6}, {F2, 2}, {E2, 4}, {D2, 4}, END};

// A boss: A minor, a pounding pedal and a high, worried line.
static const note_t boss_lead[] = {
    {A5, 2}, {R, 2}, {A5, 2}, {G5, 2}, {A5, 2}, {C6, 2}, {B5, 4},
    {F5, 2}, {R, 2}, {F5, 2}, {E5, 2}, {F5, 2}, {A5, 2}, {G5, 4},
    {E5, 2}, {F5, 2}, {G5, 2}, {A5, 2}, {B5, 2}, {C6, 2}, {D6, 4},
    {B5, 4}, {G5, 4}, {F5, 4}, {D5, 4},
    END};
static const note_t boss_harm[] = {{E4, 8}, {F4, 8}, {F4, 8}, {D4, 8}, END};
static const note_t boss_bass[] = {
    {A2, 1}, {A2, 1}, {A3, 1}, {A2, 1}, {A2, 1}, {A2, 1}, {G3, 1}, {A2, 1},
    {A2, 1}, {A2, 1}, {F3, 1}, {A2, 1}, {A2, 1}, {A2, 1}, {E3, 1}, {F3, 1},
    {B2, 1}, {B2, 1}, {B3, 1}, {B2, 1}, {B2, 1}, {B2, 1}, {A3, 1}, {B2, 1},
    {B2, 1}, {B2, 1}, {G3, 1}, {B2, 1}, {B2, 1}, {B2, 1}, {F3, 1}, {G3, 1},
    END};

// A stage cleared: bright C major.
static const note_t clear_lead[] = {{C5, 1}, {E5, 1}, {G5, 1}, {C6, 2}, {B5, 1}, {C6, 1}, {D6, 1}, {E6, 6}, END};
static const note_t clear_harm[] = {{G4, 3}, {C5, 2}, {G4, 3}, {C5, 6}, END};
static const note_t clear_bass[] = {{C3, 4}, {G3, 4}, {C4, 6}, END};
// Game over: down to the floor.
static const note_t over_lead[] = {{E5, 2}, {D5, 2}, {C5, 2}, {B4, 2}, {A4, 2}, {G4, 2}, {A4, 6}, END};
static const note_t over_bass[] = {{A2, 6}, {E2, 6}, {A2, 6}, END};
// A high score.
static const note_t high_lead[] = {{E5, 1}, {G5, 1}, {A5, 2}, {C6, 2}, {B5, 1}, {A5, 1}, {G5, 2}, {E5, 1}, {G5, 1}, {A5, 6}, END};
static const note_t high_harm[] = {{C5, 4}, {E5, 4}, {D5, 4}, {E5, 6}, END};
static const note_t high_bass[] = {{A2, 4}, {C3, 4}, {G2, 4}, {A2, 6}, END};

/// The tunes.
static const tune_t tunes[TUNE_COUNT] = {
    [TUNE_START] = {95, false, {start_lead, start_harm, start_bass}},
    [TUNE_STAGE1] = {118, true, {s1_lead, s1_harm, s1_bass}},
    [TUNE_STAGE2] = {124, true, {s2_lead, s2_harm, s2_bass}},
    [TUNE_STAGE3] = {150, true, {s3_lead, s3_harm, s3_bass}},
    [TUNE_BOSS] = {104, true, {boss_lead, boss_harm, boss_bass}},
    [TUNE_CLEAR] = {95, false, {clear_lead, clear_harm, clear_bass}},
    [TUNE_GAME_OVER] = {160, false, {over_lead, NULL, over_bass}},
    [TUNE_HIGH] = {110, false, {high_lead, high_harm, high_bass}},
};

void lsnd_tune(int tune) {
    if (!S || tune >= TUNE_COUNT || (tune >= 0 && (!S->enabled || !S->music)))
        return;
    S->tune_want = tune < 0 ? -1 : tune;
    __sync_synchronize();
    S->tune_seq++;
    S->tune_on = tune >= 0;  // until core 1 finds it has ended
}

bool lsnd_tune_playing(void) {
    return S && S->tune_on;
}

int lsnd_part_steps(int tune, int part) {
    if (tune < 0 || tune >= TUNE_COUNT || part < 0 || part > 2 || !tunes[tune].part[part])
        return -1;
    int n = 0;
    for (const note_t *p = tunes[tune].part[part]; p->note != 255; ++p)
        n += p->steps;
    return n;
}

bool lsnd_tune_loops(int tune) {
    return tune >= 0 && tune < TUNE_COUNT && tunes[tune].loop;
}

// ---- Core 1: making the samples ---------------------------------------------------------------------

/// The pitch of a MIDI note (A4 = 440 Hz).
/// @param note the note
/// @return phase step a sample
static uint32_t note_inc(int note) {
    // By octaves and a table of semitones, so core 1 needn't call powf.
    static const float ratio[12] = {1.0f, 1.0594631f, 1.1224620f, 1.1892071f, 1.2599210f, 1.3348399f,
                                    1.4142136f, 1.4983071f, 1.5874011f, 1.6817928f, 1.7817974f, 1.8877486f};
    int n = note - 21;  // A0 = 27.5 Hz
    float hz = 27.5f * ratio[n % 12];
    for (int o = n / 12; o > 0; --o)
        hz *= 2.0f;
    return (uint32_t)(hz * (4294967296.0f / RATE));
}

/// Starts the next note of tune part `i` on its voice (back to the part's
/// start at its end, if the tune loops).
/// @param i the part (0-2)
static void next_note(int i) {
    part_t *p = &S->part[i];
    voice_t *v = &S->v[MUSIC_VOICE + i];
    if (p->at && p->at->note == 255)
        p->at = S->tune_loop ? p->first : NULL;
    if (!p->at) {
        v->wave = W_OFF;
        return;
    }
    const note_t *n = p->at++;
    p->left = n->steps * S->tune_step_ms;
    if (!n->note) {
        v->wave = W_OFF;
        return;
    }
    v->wave = i == 0 ? W_PULSE : i == 1 ? W_SQUARE : W_TRIANGLE;
    v->inc = note_inc(n->note);
    v->mul = 65536;
    v->inc_min = 0;
    v->inc_max = 0xffffffffu;
    // The music sits under the effects: quieter than Swarm's jingles.
    v->vol = i == 2 ? 44000 : i == 0 ? 22000 : 12000;
    v->fade = v->vol / (p->left * 3 + 1);  // a gentle fall over the note
    v->ms = p->left - 12;                  // and a short gap before the next
    v->vib_depth = i == 0 ? 500 : 0;
    v->vib_step = 360;
}

/// Once a millisecond: takes core 0's requests and moves sweeps, fades,
/// vibrato and the music on.
static void control(void) {
    for (int i = 0; i < VOICES; ++i) {
        request_t *r = &S->req[i];
        if (r->seq != S->seen[i]) {
            S->seen[i] = r->seq;
            __sync_synchronize();
            S->v[i] = r->v;
        }
    }
    if (S->tune_seq != S->tune_seen) {
        S->tune_seen = S->tune_seq;
        __sync_synchronize();
        int t = S->tune_want;
        for (int i = 0; i < 3; ++i) {
            S->part[i].at = S->part[i].first = t >= 0 ? tunes[t].part[i] : NULL;
            S->part[i].left = 0;
            S->v[MUSIC_VOICE + i].wave = W_OFF;
        }
        S->tune_step_ms = t >= 0 ? tunes[t].step_ms : 100;
        S->tune_loop = t >= 0 && tunes[t].loop;
        if (t >= 0)
            for (int i = 0; i < 3; ++i)
                next_note(i);
    }
    bool tune = false;
    for (int i = 0; i < 3; ++i) {
        part_t *p = &S->part[i];
        if (p->at && --p->left <= 0)
            next_note(i);
        tune |= p->at != NULL;
    }
    S->tune_on = tune;
    for (int i = 0; i < VOICES; ++i) {
        voice_t *v = &S->v[i];
        if (v->wave == W_OFF)
            continue;
        if (--v->ms <= 0 || (v->vol -= v->fade) <= 0) {
            v->wave = W_OFF;
            continue;
        }
        if (v->mul != 65536) {
            uint64_t inc = ((uint64_t)v->inc * v->mul) >> 16;
            v->inc = inc < v->inc_min ? v->inc_min : inc > v->inc_max ? v->inc_max : (uint32_t)inc;
        }
        v->vib_phase += v->vib_step;
    }
}

bool lsnd_source(int32_t *left, int32_t *right) {
    snd_t *s = S;
    *left = *right = 0;
    if (!s)
        return false;
    if (--s->tick <= 0) {
        s->tick = TICK;
        control();
    }
    if (!s->enabled)
        return true;
    int32_t sum = 0;
    for (int i = 0; i < VOICES; ++i) {
        voice_t *v = &s->v[i];
        if (v->wave == W_OFF)
            continue;
        uint32_t inc = v->inc;
        if (v->vib_depth) {
            // A triangle wobble, +-vib_depth of the pitch.
            int32_t tri = (int32_t)(v->vib_phase < 32768 ? v->vib_phase : 65535 - v->vib_phase) * 2 - 32768;
            inc += (uint32_t)(((int64_t)inc * v->vib_depth >> 16) * tri >> 15);
        }
        uint32_t old = v->phase;
        v->phase += inc;
        int32_t x;  // -32768..32767
        switch (v->wave) {
        case W_SQUARE: x = v->phase < 0x80000000u ? 32767 : -32768; break;
        case W_PULSE: x = v->phase < 0x40000000u ? 32767 : -32768; break;
        case W_TRIANGLE: {
            int32_t p = (int32_t)(v->phase >> 16);  // 0..65535
            x = (p < 32768 ? p : 65535 - p) * 2 - 32768;
            break;
        }
        case W_SAW: x = (int32_t)(v->phase >> 16) - 32768; break;
        default:  // W_NOISE: a new level each time the phase wraps
            if (v->phase < old) {
                unsigned bit = (v->lfsr ^ (v->lfsr >> 1)) & 1;
                v->lfsr = (uint16_t)((v->lfsr >> 1) | (bit << 14));
                v->noise = (v->lfsr & 1) ? 1 : -1;
            }
            x = v->noise * 32767;
            break;
        }
        sum += (int32_t)(((int64_t)x * v->vol) >> 16) * AMP >> 15;
    }
    if (sum > 32767)
        sum = 32767;
    if (sum < -32768)
        sum = -32768;
    *left = *right = sum;
    return true;
}
