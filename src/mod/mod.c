/**
 * @file mod.c
 * @brief MOD files and a ProTracker 2-style player.
 *
 * See mod.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "mod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Paula's clock on a PAL Amiga (Hz): period p plays at PAULA_PAL / p.
#define PAULA_PAL 3546895u

// ---- Notes -----------------------------------------------------------------------------------

/// The periods of the 3 octaves at finetune 0.
static const uint16_t base_periods[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

uint16_t mod_note_period(int note, int finetune) {
    // ProTracker's tables, worked out each time (no RAM for them): each
    // finetune step is 1/8 of a semitone, 2^(-1/96).
    if (note < 0 || note > 35)
        return 0;
    finetune = (int8_t)(finetune << 4) >> 4;  // -8..7
    double f = 1.0, step = finetune >= 0 ? 0.99280572 : 1.00724641;  // 2^(-1/96), 2^(1/96)
    for (int k = 0; k < (finetune >= 0 ? finetune : -finetune); ++k)
        f *= step;
    return (uint16_t)(base_periods[note] * f + 0.5);
}

int mod_period_note(uint16_t period) {
    if (!period)
        return -1;
    int best = 0, best_d = 1 << 30;
    for (int n = 0; n < 36; ++n) {
        int d = abs((int)base_periods[n] - period);
        if (d < best_d) {
            best_d = d;
            best = n;
        }
    }
    return best;
}

const char *mod_note_name(int note) {
    static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                          "F#", "G-", "G#", "A-", "A#", "B-"};
    static char buf[4];
    if (note < 0 || note > 35)
        return "---";
    snprintf(buf, sizeof buf, "%s%d", names[note % 12], note / 12 + 1);
    return buf;
}

// ---- The song ----------------------------------------------------------------------------------

/// A pattern's size.
/// @param s the song
/// @return bytes
static size_t pattern_bytes(const mod_song_t *s) {
    return (size_t)MOD_ROWS * s->channels * 4;
}

uint8_t *mod_cell_ptr(const mod_song_t *s, int pattern, int row, int ch) {
    return s->patterns + pattern * pattern_bytes(s) + ((size_t)row * s->channels + ch) * 4;
}

mod_cell_t mod_get(const mod_song_t *s, int pattern, int row, int ch) {
    const uint8_t *c = mod_cell_ptr(s, pattern, row, ch);
    return (mod_cell_t){(uint16_t)((c[0] & 15) << 8 | c[1]), (uint8_t)((c[0] & 0xf0) | c[2] >> 4),
                        (uint8_t)(c[2] & 15), c[3]};
}

void mod_put(mod_song_t *s, int pattern, int row, int ch, mod_cell_t v) {
    uint8_t *c = mod_cell_ptr(s, pattern, row, ch);
    c[0] = (v.sample & 0xf0) | (v.period >> 8 & 15);
    c[1] = v.period & 0xff;
    c[2] = (uint8_t)(v.sample << 4 | (v.effect & 15));
    c[3] = v.param;
}

void mod_clear(mod_song_t *s) {
    for (int i = 0; i < MOD_MAX_SAMPLES; ++i)
        mod_free(s->samples[i].data);
    mod_free(s->patterns);
    memset(s, 0, sizeof *s);
}

bool mod_new(mod_song_t *s, int channels) {
    memset(s, 0, sizeof *s);
    s->channels = channels;
    s->nsamples = 31;
    strcpy(s->sig, channels == 4 ? "M.K." : channels == 6 ? "6CHN" : "8CHN");
    s->length = 1;
    s->npatterns = 1;
    s->restart = 0;
    s->patterns = mod_alloc(MOD_MAX_PATTERNS * pattern_bytes(s));
    if (!s->patterns)
        return false;
    memset(s->patterns, 0, MOD_MAX_PATTERNS * pattern_bytes(s));
    for (int i = 0; i < MOD_MAX_SAMPLES; ++i)
        s->samples[i].volume = 64;
    return true;
}

/// Reads 16 bits big-endian.
/// @param p where
/// @return the value
static uint16_t be16(const uint8_t *p) {
    return p[0] << 8 | p[1];
}

const char *mod_load(mod_song_t *s, const uint8_t *d, size_t len) {
    memset(s, 0, sizeof *s);
    if (len < 600)
        return "too short to be a MOD";
    // The signature at 1080 says 31 samples and the channel count; without
    // one it is an old 15-sample Soundtracker song.
    int channels = 0, nsamples = 31;
    char sig[5] = "";
    if (len >= 1084) {
        memcpy(sig, d + 1080, 4);
        if (!strcmp(sig, "M.K.") || !strcmp(sig, "M!K!") || !strcmp(sig, "FLT4") ||
            !strcmp(sig, "4CHN") || !strcmp(sig, "M&K!") || !strcmp(sig, "N.T."))
            channels = 4;
        else if (!strcmp(sig, "6CHN"))
            channels = 6;
        else if (!strcmp(sig, "8CHN") || !strcmp(sig, "FLT8") || !strcmp(sig, "OCTA") ||
                 !strcmp(sig, "CD81"))
            channels = 8;
    }
    if (!channels) {
        channels = 4;
        nsamples = 15;
        sig[0] = 0;
    }
    s->channels = channels;
    s->nsamples = nsamples;
    strcpy(s->sig, sig);
    memcpy(s->title, d, 20);  // raw: saving gives back the same bytes
    size_t off = 20;
    for (int i = 0; i < nsamples; ++i, off += 30) {
        mod_sample_t *m = &s->samples[i];
        memcpy(m->name, d + off, 22);
        m->length = be16(d + off + 22) * 2u;
        m->finetune = (int8_t)((d[off + 24] & 15) << 4) >> 4;
        m->volume = d[off + 25] > 64 ? 64 : d[off + 25];
        m->loop_start = be16(d + off + 26) * 2u;
        m->loop_len = be16(d + off + 28) * 2u;
    }
    for (int i = nsamples; i < MOD_MAX_SAMPLES; ++i)
        s->samples[i].volume = 64;
    s->length = d[off];
    s->restart = d[off + 1] < 128 ? d[off + 1] : 0;
    memcpy(s->orders, d + off + 2, 128);
    off += 130 + (nsamples == 31 ? 4 : 0);
    if (s->length < 1 || s->length > 128)
        return "bad song length";
    // As ProTracker: the highest pattern in all 128 entries; if the file
    // hasn't that many, only the entries in use.
    size_t pb = pattern_bytes(s);
    int highest = 0;
    for (int i = 0; i < 128; ++i)
        if (s->orders[i] > highest)
            highest = s->orders[i];
    if (off + (highest + 1) * pb > len) {
        highest = 0;
        for (int i = 0; i < s->length; ++i)
            if (s->orders[i] > highest)
                highest = s->orders[i];
    }
    s->npatterns = highest + 1;
    if (s->npatterns > MOD_MAX_PATTERNS)
        return "too many patterns";
    if (off + s->npatterns * pb > len)
        return "the file is cut short (patterns)";
    s->patterns = mod_alloc(MOD_MAX_PATTERNS * pb);
    if (!s->patterns)
        return "not enough memory";
    memset(s->patterns, 0, MOD_MAX_PATTERNS * pb);
    memcpy(s->patterns, d + off, s->npatterns * pb);  // (FLT8's split layout isn't undone)
    off += s->npatterns * pb;
    for (int i = 0; i < nsamples; ++i) {
        mod_sample_t *m = &s->samples[i];
        if (!m->length)
            continue;
        uint32_t n = m->length;
        if (off + n > len)
            n = off < len ? (uint32_t)(len - off) : 0;  // short files: keep what is there
        m->data = mod_alloc(m->length + 2);
        if (!m->data) {
            mod_clear(s);
            return "not enough memory for the samples";
        }
        memset(m->data, 0, m->length + 2);
        memcpy(m->data, d + off, n);
        off += m->length;
        if (m->loop_start + m->loop_len > m->length) {
            // Some trackers stored the loop start in words: fix what is impossible.
            if (m->loop_start / 2 + m->loop_len <= m->length)
                m->loop_start /= 2;
            else if (m->loop_start < m->length)
                m->loop_len = m->length - m->loop_start;
            else
                m->loop_start = m->loop_len = 0;
        }
    }
    return NULL;
}

size_t mod_save_size(const mod_song_t *s) {
    size_t n = 1084 + s->npatterns * pattern_bytes(s);
    for (int i = 0; i < 31; ++i)
        n += s->samples[i].length & ~1u;
    return n;
}

/// Stores 16 bits big-endian.
/// @param p where
/// @param v the value
static void put16be(uint8_t *p, unsigned v) {
    p[0] = v >> 8;
    p[1] = v;
}

void mod_save(const mod_song_t *s, uint8_t *o) {
    // Always the 31-sample layout (with its signature).
    memset(o, 0, 1084);
    memcpy(o, s->title, 20);
    size_t off = 20;
    for (int i = 0; i < 31; ++i, off += 30) {
        const mod_sample_t *m = &s->samples[i];
        memcpy(o + off, m->name, 22);
        put16be(o + off + 22, m->length / 2);
        o[off + 24] = m->finetune & 15;
        o[off + 25] = m->volume;
        put16be(o + off + 26, m->loop_start / 2);
        put16be(o + off + 28, m->loop_len / 2);
    }
    o[off] = s->length;
    o[off + 1] = s->restart;
    memcpy(o + off + 2, s->orders, 128);
    memcpy(o + 1080, s->sig[0] ? s->sig : "M.K.", 4);
    off = 1084;
    memcpy(o + off, s->patterns, s->npatterns * pattern_bytes(s));
    off += s->npatterns * pattern_bytes(s);
    for (int i = 0; i < 31; ++i) {
        const mod_sample_t *m = &s->samples[i];
        uint32_t n = m->length & ~1u;
        if (n && m->data)
            memcpy(o + off, m->data, n);
        off += n;
    }
}

// ---- The player ------------------------------------------------------------------------------------

/// ProTracker's vibrato sine (half a wave, 0 up to 255 and back).
static const uint8_t sine_table[32] = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                       224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                       212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

/// A channel's state.
typedef struct {
    /// What is playing
    const mod_sample_t *smp;
    int sample;            ///< 1..31, 0 none
    uint64_t pos;          ///< 32.32 position in the sample
    uint64_t step;         ///< per output frame
    bool active;  ///< sounding
    int period;            ///< the base period (portamento changes it)
    int out_period;        ///< what is heard this tick (vibrato, arpeggio)
    int volume;            ///< 0..64, the base
    int out_volume;        ///< this tick (tremolo)
    int finetune;  ///< the sample's finetune (-8..7)
    /// Effects and their memories
    ///@{
    int effect, param;
    ///@}
    /// Tone portamento: the period it slides to, and the speed.
    ///@{
    int porta_target, porta_speed;
    ///@}
    /// Vibrato: position, speed, depth and waveform.
    ///@{
    int vib_pos, vib_speed, vib_depth, vib_wave;
    ///@}
    /// Tremolo: position, speed, depth and waveform.
    ///@{
    int trem_pos, trem_speed, trem_depth, trem_wave;
    ///@}
    int offset_mem;  ///< 9xx: the last offset
    bool gliss;            ///< E3x: tone portamento in semitones
    /// E6x: the loop's start row and count.
    ///@{
    int loop_row, loop_count;
    ///@}
    int delay_tick;        ///< EDx: the note waits
    mod_cell_t delayed;  ///< the cell held by EDx
    int pan;               ///< 0 left .. 256 right
    /// Display
    int level;
    uint8_t scope_pos;  ///< the scope's position
} channel_t;

static struct {
    mod_song_t *song;  ///< the song
    volatile int state;  ///< MOD_STOPPED, MOD_PLAYING or MOD_PATTERN
    /// The order, pattern, row and tick.
    ///@{
    int order, pattern, row, tick;
    ///@}
    /// Ticks a row, and BPM.
    ///@{
    int speed, tempo;
    ///@}
    int frames_left;          ///< until the next tick
    /// set by B/D
    ///@{
    int next_order, next_row;
    ///@}
    bool jump;  ///< B or D jumped this row
    /// EEx: rows to wait, and the count.
    ///@{
    int pattern_delay, delay_count;
    ///@}
    channel_t ch[MOD_MAX_CH];  ///< the channels
    bool muted[MOD_MAX_CH];  ///< muted channels
    int separation;           ///< 0..100
    bool once;                ///< stop at the end instead of starting over
    volatile bool paused;  ///< paused
    int volume;               ///< 0..256
    /// Requests from core 0, taken by the player at its next frame.
    volatile int req;         ///< 0 none, 1 play, 2 stop, 3 jam
    /// The order, row and pattern-only flag asked for.
    ///@{
    int req_order, req_row, req_pattern_only;
    ///@}
    mod_song_t *req_song;  ///< the song asked for
    /// The note to jam: channel, sample and note.
    ///@{
    int jam_ch, jam_sample, jam_note;
    ///@}
    int fixed_pattern;        ///< MOD_PATTERN: the pattern looped
    /// The speed and tempo a song starts at.
    ///@{
    int start_speed, start_tempo;
    ///@}
    /// Requests made and done.
    ///@{
    volatile unsigned req_seq, done_seq;
    ///@}
    int8_t (*scope)[256];     ///< each channel's last mixed values (from mod_alloc)
} P = {.separation = 70, .start_speed = 6, .start_tempo = 125, .volume = 256};  ///< the player (core 1)

/// A channel's pan at the start: Amiga L R R L.
/// @param ch the channel
/// @return 0 left, 256 right
static int default_pan(int ch) {
    return (ch & 3) == 0 || (ch & 3) == 3 ? 0 : 256;  // Amiga: L R R L (0..256)
}

/// Sets the step for a period.
/// @param c the channel
/// @param period the period
static void set_step(channel_t *c, int period) {
    if (period < 28)
        period = 28;
    c->out_period = period;
    c->step = ((uint64_t)PAULA_PAL << 32) / ((uint64_t)period * MOD_RATE);
}

/// Starts the sample from the beginning.
/// @param c the channel
static void trigger(channel_t *c) {
    c->pos = 0;
    c->active = c->smp && c->smp->data && c->smp->length > 2;
    if (!(c->vib_wave & 4))
        c->vib_pos = 0;
    if (!(c->trem_wave & 4))
        c->trem_pos = 0;
}

/// Vibrato and tremolo's waveforms.
/// @param wave 0 sine, 1 ramp, 2 square
/// @param pos the position
/// @return -255..255
static int waveform(int wave, int pos) {
    // 0 sine, 1 ramp down, 2 square (and 3 random, as square); -255..255
    pos &= 63;
    int v;
    switch (wave & 3) {
    case 1: v = 255 - pos * 8; break;  // ramp
    case 2:
    case 3: v = 255; break;
    default: v = sine_table[pos & 31]; break;
    }
    return (pos >= 32 && (wave & 3) != 1) ? -v : v;
}

/// Axy: volume slide.
/// @param c the channel
/// @param param the parameter
static void vol_slide(channel_t *c, int param) {
    int up = param >> 4, down = param & 15;
    c->volume += up ? up : -down;
    if (c->volume < 0)
        c->volume = 0;
    if (c->volume > 64)
        c->volume = 64;
}

/// 3xx: tone portamento's step.
/// @param c the channel
static void tone_porta(channel_t *c) {
    if (!c->porta_target)
        return;
    if (c->period < c->porta_target) {
        c->period += c->porta_speed;
        if (c->period > c->porta_target)
            c->period = c->porta_target;
    } else if (c->period > c->porta_target) {
        c->period -= c->porta_speed;
        if (c->period < c->porta_target)
            c->period = c->porta_target;
    }
}

/// A new row's cell for a channel (tick 0).
/// @param ci the channel
/// @param cell the cell
static void start_cell(int ci, mod_cell_t cell) {
    channel_t *c = &P.ch[ci];
    mod_song_t *s = P.song;
    int e = cell.effect, x = cell.param >> 4, y = cell.param & 15;
    c->effect = e;
    c->param = cell.param;
    if (e == 14 && x == 13 && y) {  // EDx: the note waits for tick y (its sample's volume doesn't)
        c->delay_tick = y;
        c->delayed = cell;
        if (cell.sample && cell.sample <= s->nsamples) {
            c->volume = s->samples[cell.sample - 1].volume;
            c->out_volume = c->volume;
        }
        return;
    }
    c->delay_tick = 0;
    if (cell.sample && cell.sample <= s->nsamples) {
        c->sample = cell.sample;
        c->smp = &s->samples[cell.sample - 1];
        c->volume = c->smp->volume;
        c->finetune = c->smp->finetune;
    }
    if (e == 14 && x == 5)  // E5x: finetune for this note
        c->finetune = (int8_t)(y << 4) >> 4;
    if (cell.period) {
        // The period for the sample's finetune: via the note it is nearest.
        int note = mod_period_note(cell.period);
        int period = c->finetune ? mod_note_period(note, c->finetune) : cell.period;
        if (e == 3 || e == 5) {
            c->porta_target = period;
        } else {
            c->period = period;
            trigger(c);
            if (e == 9) {
                if (cell.param)
                    c->offset_mem = cell.param;
                c->pos = (uint64_t)(c->offset_mem * 256) << 32;
                if (c->smp && (c->pos >> 32) >= c->smp->length) {
                    if (c->smp->loop_len > 2)
                        c->pos = (uint64_t)c->smp->loop_start << 32;
                    else
                        c->active = false;
                }
            }
        }
    }
    switch (e) {
    case 3:
        if (cell.param)
            c->porta_speed = cell.param;
        break;
    case 4:
        if (x)
            c->vib_speed = x;
        if (y)
            c->vib_depth = y;
        break;
    case 7:
        if (x)
            c->trem_speed = x;
        if (y)
            c->trem_depth = y;
        break;
    case 8: c->pan = cell.param + (cell.param >> 7); break;  // 0..256
    case 11:  // Bxx: jump to an order
        P.next_order = cell.param;
        P.next_row = 0;
        P.jump = true;
        break;
    case 12: c->volume = cell.param > 64 ? 64 : cell.param; break;
    case 13:  // Dxy: break to row x*10+y of the next pattern
        if (!P.jump)
            P.next_order = P.order + 1;
        P.next_row = x * 10 + y;
        if (P.next_row > 63)
            P.next_row = 0;
        P.jump = true;
        break;
    case 14:
        switch (x) {
        case 1: c->period -= y; if (c->period < 113) c->period = 113; break;
        case 2: c->period += y; if (c->period > 856) c->period = 856; break;
        case 3: c->gliss = y != 0; break;
        case 4: c->vib_wave = y; break;
        case 8: c->pan = y * 256 / 15; break;
        case 6:  // pattern loop
            if (!y) {
                c->loop_row = P.row;
            } else if (!c->loop_count) {
                c->loop_count = y;
                P.next_row = c->loop_row;
                P.next_order = P.order;
                P.jump = true;
            } else if (--c->loop_count) {
                P.next_row = c->loop_row;
                P.next_order = P.order;
                P.jump = true;
            }
            break;
        case 7: c->trem_wave = y; break;
        case 10: c->volume = c->volume + y > 64 ? 64 : c->volume + y; break;
        case 11: c->volume = c->volume - y < 0 ? 0 : c->volume - y; break;
        case 12: if (!y) c->volume = 0; break;
        case 14: if (!P.delay_count) P.pattern_delay = y; break;
        }
        break;
    case 15:
        if (cell.param) {
            if (cell.param < 32)
                P.speed = cell.param;
            else
                P.tempo = cell.param;
        }
        break;
    }
}

/// Every tick: effects that run over time; then what is heard.
/// @param ci the channel
static void tick_channel(int ci) {
    channel_t *c = &P.ch[ci];
    int e = c->effect, x = c->param >> 4, y = c->param & 15;
    int period = c->period, volume = c->volume;
    if (P.tick && c->delay_tick && P.tick == c->delay_tick) {
        start_cell(ci, (mod_cell_t){c->delayed.period, c->delayed.sample, 0, 0});
        period = c->period;
        volume = c->volume;
    }
    if (P.tick) {
        switch (e) {
        case 1: c->period -= c->param; if (c->period < 113) c->period = 113; break;
        case 2: c->period += c->param; if (c->period > 856) c->period = 856; break;
        case 3: tone_porta(c); break;
        case 5: tone_porta(c); vol_slide(c, c->param); break;
        case 6: vol_slide(c, c->param); break;
        case 10: vol_slide(c, c->param); break;
        case 14:
            if (x == 9 && y && P.tick % y == 0)
                trigger(c);
            if (x == 12 && P.tick == y)
                c->volume = 0;
            break;
        }
        if (e == 4 || e == 6)
            c->vib_pos += c->vib_speed;
        if (e == 7)
            c->trem_pos += c->trem_speed;
        period = c->period;
        volume = c->volume;
    }
    if (e == 0 && c->param) {  // arpeggio
        int add = P.tick % 3 == 1 ? x : P.tick % 3 == 2 ? y : 0;
        if (add) {
            int note = mod_period_note(c->period);
            period = mod_note_period(note + add > 35 ? 35 : note + add, c->finetune);
        }
    }
    if (c->gliss && (e == 3 || e == 5) && period)
        period = mod_note_period(mod_period_note(period), c->finetune);
    if (e == 4 || e == 6)
        period += waveform(c->vib_wave, c->vib_pos) * c->vib_depth / 128;
    if (e == 7) {
        volume += waveform(c->trem_wave, c->trem_pos) * c->trem_depth / 64;
        volume = volume < 0 ? 0 : volume > 64 ? 64 : volume;
    }
    c->out_volume = volume;
    if (period)
        set_step(c, period);
}

/// Clears the channels.
static void reset_channels(void) {
    for (int i = 0; i < MOD_MAX_CH; ++i) {
        channel_t *c = &P.ch[i];
        memset(c, 0, sizeof *c);
        c->pan = default_pan(i);
    }
}

/// Plays a row: each channel's cell, then where to go next.
static void do_row(void) {
    mod_song_t *s = P.song;
    P.jump = false;
    P.pattern = P.state == MOD_PATTERN ? P.fixed_pattern : s->orders[P.order];
    if (P.pattern >= s->npatterns)
        P.pattern = 0;
    for (int ci = 0; ci < s->channels; ++ci)
        start_cell(ci, mod_get(s, P.pattern, P.row, ci));
}

static void next_row(void) {
    mod_song_t *s = P.song;
    if (P.pattern_delay) {  // EEx: the row lasts x more rows' time
        P.delay_count = P.pattern_delay;
        P.pattern_delay = 0;
    }
    if (P.delay_count) {
        --P.delay_count;
        return;
    }
    if (P.jump) {
        P.row = P.next_row;
        if (P.state == MOD_SONG)
            P.order = P.next_order;
        P.jump = false;
    } else if (++P.row >= MOD_ROWS) {
        P.row = 0;
        if (P.state == MOD_SONG)
            ++P.order;
    }
    if (P.order >= s->length) {
        if (P.once && P.state == MOD_SONG) {
            P.state = MOD_STOPPED;  // the end (looped samples would drone on)
            for (int i = 0; i < MOD_MAX_CH; ++i)
                P.ch[i].active = false;
            return;
        }
        P.order = s->restart < s->length ? s->restart : 0;
    }
    do_row();
}

/// Takes a request from core 0 (play, stop, jam).
static void take_requests(void) {
    if (P.req_seq == P.done_seq)
        return;
    switch (P.req) {
    case 1:
        P.song = P.req_song;
        reset_channels();
        P.order = !P.req_pattern_only && P.req_order < P.song->length ? P.req_order : 0;
        P.fixed_pattern = P.req_order < P.song->npatterns ? P.req_order : 0;
        P.row = P.req_row;
        P.speed = P.start_speed;
        P.tempo = P.start_tempo;
        P.tick = 0;
        P.pattern_delay = P.delay_count = 0;
        P.state = P.req_pattern_only ? MOD_PATTERN : MOD_SONG;
        do_row();
        for (int i = 0; i < P.song->channels; ++i)
            tick_channel(i);
        P.frames_left = MOD_RATE * 5 / (P.tempo * 2);
        break;
    case 2:
        P.state = MOD_STOPPED;
        for (int i = 0; i < MOD_MAX_CH; ++i)
            P.ch[i].active = false;
        break;
    case 3: {
        mod_song_t *s = P.req_song;
        channel_t *c = &P.ch[P.jam_ch];
        if (P.state == MOD_STOPPED) {
            P.song = s;
            P.speed = P.start_speed;
            P.tempo = P.start_tempo;
            c->pan = default_pan(P.jam_ch);
        }
        if (P.jam_sample >= 1 && P.jam_sample <= 31) {
            c->smp = &s->samples[P.jam_sample - 1];
            c->sample = P.jam_sample;
            c->volume = c->out_volume = c->smp->volume;
            c->finetune = c->smp->finetune;
            c->effect = c->param = 0;
            c->period = mod_note_period(P.jam_note, c->finetune);
            trigger(c);
            set_step(c, c->period);
        }
        break;
    }
    }
    P.done_seq = P.req_seq;
}

MOD_HOT bool mod_render(int32_t *left, int32_t *right) {
    take_requests();
    if (P.paused) {
        *left = *right = 0;
        return true;
    }
    if (P.state != MOD_STOPPED && P.song) {
        if (--P.frames_left <= 0) {
            if (++P.tick >= P.speed) {
                P.tick = 0;
                next_row();
            }
            for (int i = 0; i < P.song->channels; ++i)
                tick_channel(i);
            P.frames_left = MOD_RATE * 5 / (P.tempo * 2);
        }
    }
    int32_t l = 0, r = 0;
    bool any = false;
    int nch = P.song ? P.song->channels : 4;
    for (int i = 0; i < nch; ++i) {
        channel_t *c = &P.ch[i];
        if (!c->active || !c->smp) {
            if (c->level && !(P.frames_left & 63))
                c->level--;
            continue;
        }
        const mod_sample_t *m = c->smp;
        uint32_t ip = (uint32_t)(c->pos >> 32);
        uint32_t end = m->loop_len > 2 ? m->loop_start + m->loop_len : m->length;
        if (end > m->length)
            end = m->length;
        if (ip >= end) {
            if (m->loop_len > 2) {
                while (ip >= end)
                    ip -= m->loop_len;
                c->pos = ((uint64_t)ip << 32) | (uint32_t)c->pos;
            } else {
                c->active = false;
                continue;
            }
        }
        // Linear interpolation towards the next sample (the loop's start at its end).
        int s0 = m->data[ip];
        uint32_t in = ip + 1;
        if (in >= end)
            in = m->loop_len > 2 ? m->loop_start : ip;
        int s1 = m->data[in];
        int frac = (int)((uint32_t)c->pos >> 16);  // 16 bits
        int v = s0 + (((s1 - s0) * frac) >> 16);
        c->pos += c->step;
        int out = P.muted[i] ? 0 : v * c->out_volume;  // -8192..8128
        // Panning with the stereo separation: hard left/right at 100%.
        int pan = 128 + (c->pan - 128) * P.separation / 100;
        l += out * (256 - pan) >> 7;
        r += out * pan >> 7;
        any = true;
        int a = (out < 0 ? -out : out) >> 7;
        if (a > c->level)
            c->level = a;
        else if (c->level && !(P.frames_left & 63))
            c->level--;
        if (!(P.frames_left & 7) && P.scope)  // the scope: every 8th frame
            P.scope[i][c->scope_pos++] = (int8_t)(out >> 6);
    }
    *left = P.volume == 256 ? l : l * P.volume >> 8;
    *right = P.volume == 256 ? r : r * P.volume >> 8;
    return any || P.state != MOD_STOPPED;
}

// ---- Core 0 side ---------------------------------------------------------------------------------

/// Makes a request and lets the player take it.
/// @param what 1 play, 2 stop, 3 jam
static void request(int what) {
    if (!P.scope && (P.scope = mod_alloc(MOD_MAX_CH * 256)))  // kept from then on
        memset(P.scope, 0, MOD_MAX_CH * 256);
    P.req = what;
    __sync_synchronize();
    ++P.req_seq;
}

/// Waits for the player to take the last request.
static void wait_done(void) {
    // The player takes a request within a frame; without audio (a PC test),
    // nothing renders, so don't wait for ever.
    for (int i = 0; i < 2000000 && P.done_seq != P.req_seq; ++i)
        __sync_synchronize();
}

/// Starts playing.
/// @param s the song
/// @param order_or_pattern the order (or pattern)
/// @param row the row
/// @param pattern_only loop the pattern
static void play(mod_song_t *s, int order_or_pattern, int row, bool pattern_only) {
    wait_done();
    P.req_song = s;
    P.req_order = order_or_pattern;
    P.req_row = row & 63;
    P.req_pattern_only = pattern_only;
    request(1);
}

void mod_play(mod_song_t *s, int order, int row, bool pattern_only) {
    play(s, pattern_only ? s->orders[order] : order, row, pattern_only);
}

void mod_play_pattern(mod_song_t *s, int pattern, int row) {
    play(s, pattern, row, true);
}

void mod_set_tempo(int speed, int tempo) {
    // Taken at the next tick if playing; and the start of the next play.
    if (speed >= 1 && speed <= 31)
        P.start_speed = P.speed = speed;
    if (tempo >= 32 && tempo <= 255)
        P.start_tempo = P.tempo = tempo;
}

void mod_stop(void) {
    wait_done();
    request(2);
    wait_done();
}

void mod_jam(mod_song_t *s, int ch, int sample, int note) {
    wait_done();
    P.req_song = s;
    P.jam_ch = ch;
    P.jam_sample = sample;
    P.jam_note = note;
    request(3);
}

void mod_mute(int ch, bool muted) {
    if (ch >= 0 && ch < MOD_MAX_CH)
        P.muted[ch] = muted;
}

void mod_set_loop(bool loop) {
    P.once = !loop;
}

void mod_pause(bool paused) {
    P.paused = paused;
}

void mod_set_volume(int percent) {
    P.volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) * 256 / 100;
}

void mod_set_separation(int percent) {
    P.separation = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

void mod_get_status(mod_status_t *st) {
    st->state = P.state;
    st->order = P.order;
    st->pattern = P.pattern;
    st->row = P.row;
    st->speed = P.speed;
    st->tempo = P.tempo;
    for (int i = 0; i < MOD_MAX_CH; ++i) {
        st->level[i] = P.ch[i].level > 64 ? 64 : P.ch[i].level;
        st->muted[i] = P.muted[i];
        st->sample[i] = P.ch[i].active ? P.ch[i].sample : 0;
        st->pos[i] = (uint32_t)(P.ch[i].pos >> 32);
    }
}

void mod_scope(int ch, int8_t *out, int n) {
    if (n > 256)
        n = 256;
    channel_t *c = &P.ch[ch];
    uint8_t p = c->scope_pos - n;
    for (int i = 0; i < n; ++i)
        out[i] = P.scope ? P.scope[ch][(uint8_t)(p + i)] : 0;
}
