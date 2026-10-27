/**
 * @file xm.c
 * @brief XM files and a FastTracker 2-style player.
 *
 * See xm.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "xm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---- Loading ---------------------------------------------------------------------------------------

/// Reads 16 bits little-endian.
/// @param p where
/// @return the value
static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

/// Reads 32 bits little-endian.
/// @param p where
/// @return the value
static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

bool xm_is_xm(const uint8_t *d, size_t len) {
    return len >= 60 && !memcmp(d, "Extended Module: ", 17);
}

/// A name, 0-terminated.
/// @param out the name
/// @param in its bytes
/// @param n their count
static void name_copy(char *out, const uint8_t *in, int n) {
    memcpy(out, in, n);
    out[n] = 0;
}

void xm_clear(xm_song_t *s) {
    for (int p = 0; p < XM_MAX_PATTERNS; ++p)
        xm_free(s->patterns[p].cells);
    for (int i = 0; s->instruments && i < s->ninstruments; ++i) {
        xm_instrument_t *in = &s->instruments[i];
        for (int k = 0; k < in->nsamples; ++k)
            xm_free(in->samples[k].data);
        xm_free(in->samples);
    }
    xm_free(s->instruments);
    memset(s, 0, sizeof *s);
}

/// Reads an envelope.
/// @param e the envelope
/// @param points its 12 points
/// @param count points used
/// @param sus the sustain point
/// @param ls the loop's start
/// @param le its end
/// @param type its flags
static void read_env(xm_env_t *e, const uint8_t *points, uint8_t count, uint8_t sus, uint8_t ls,
                     uint8_t le, uint8_t type) {
    for (int i = 0; i < 12; ++i) {
        e->points[i].x = le16(points + i * 4);
        e->points[i].y = le16(points + i * 4 + 2);
    }
    e->count = count > 12 ? 12 : count;
    e->sustain = sus;
    e->loop_start = ls;
    e->loop_end = le;
    e->flags = type;
}

/// An instrument's keymap, envelopes, vibrato and fadeout, from b (the
/// same in .XM and .XI files).
/// @param in the instrument
/// @param b the bytes
static void read_body(xm_instrument_t *in, const uint8_t *b) {
    memcpy(in->keymap, b, 96);
    read_env(&in->venv, b + 96, b[192], b[194], b[195], b[196], b[200]);
    read_env(&in->penv, b + 144, b[193], b[197], b[198], b[199], b[201]);
    in->vib_type = b[202];
    in->vib_sweep = b[203];
    in->vib_depth = b[204];
    in->vib_rate = b[205];
    in->fadeout = le16(b + 206);
}

/// An instrument's sample headers and sounds at d + *off (moved past them).
/// @param in the instrument
/// @param d the file
/// @param len its length
/// @param poff the offset (moved past them)
/// @param ns sample count
/// @param shsize a sample header's size
/// @return NULL, or an error
static const char *read_samples(xm_instrument_t *in, const uint8_t *d, size_t len, size_t *poff, int ns,
                                uint32_t shsize) {
    size_t off = *poff;
    if (ns > 16)
        return "an instrument with too many samples";
    in->nsamples = ns;
    in->samples = xm_alloc(ns * sizeof *in->samples);
    if (!in->samples)
        return "not enough memory";
    memset(in->samples, 0, ns * sizeof *in->samples);
    uint32_t bytes[16];
    for (int k = 0; k < ns; ++k) {
        if (off + 40 > len)
            return "the file is cut short (samples)";
        const uint8_t *sh = d + off;
        xm_sample_t *sm = &in->samples[k];
        bytes[k] = le32(sh);
        uint32_t ls = le32(sh + 4), ll = le32(sh + 8);
        sm->volume = sh[12] > 64 ? 64 : sh[12];
        sm->finetune = (int8_t)sh[13];
        sm->loop = sh[14] & 3;
        if (sm->loop == 3)
            sm->loop = 2;
        sm->bits16 = sh[14] & 16;
        sm->pan = sh[15];
        sm->relnote = (int8_t)sh[16];
        sm->reserved = sh[17];
        name_copy(sm->name, sh + 18, 22);
        int w = sm->bits16 ? 2 : 1;
        sm->length = bytes[k] / w;
        sm->loop_start = ls / w;
        sm->loop_len = ll / w;
        off += shsize;
    }
    for (int k = 0; k < ns; ++k) {
        xm_sample_t *sm = &in->samples[k];
        uint32_t n = bytes[k];
        if (off + n > len)
            n = off < len ? (uint32_t)(len - off) : 0;
        // One more than the length: the mixer reads one past the end.
        sm->data = xm_alloc((sm->length + 1) * sizeof(int16_t));
        if (!sm->data)
            return "not enough memory for the samples";
        const uint8_t *p = d + off;
        if (sm->bits16) {
            int16_t v = 0;
            for (uint32_t j = 0; j < sm->length; ++j) {
                if (j * 2 + 1 < n)
                    v += (int16_t)le16(p + j * 2);
                sm->data[j] = v;
            }
        } else {
            int8_t v = 0;
            for (uint32_t j = 0; j < sm->length; ++j) {
                if (j < n)
                    v += (int8_t)p[j];
                sm->data[j] = (int16_t)(v * 256);
            }
        }
        sm->data[sm->length] = sm->length ? sm->data[sm->length - 1] : 0;
        if (sm->loop_start > sm->length)
            sm->loop_start = sm->length;
        if (sm->loop_start + sm->loop_len > sm->length)
            sm->loop_len = sm->length - sm->loop_start;
        if (sm->loop_len == 0)
            sm->loop = 0;
        off += bytes[k];
    }
    *poff = off;
    return NULL;
}

const char *xm_load(xm_song_t *s, const uint8_t *d, size_t len) {
    memset(s, 0, sizeof *s);
    if (!xm_is_xm(d, len) || len < 80)
        return "not an XM file";
    name_copy(s->title, d + 17, 20);
    name_copy(s->tracker, d + 38, 20);
    s->version = le16(d + 58);
    uint32_t hsize = le32(d + 60);
    s->length = le16(d + 64);
    s->restart = le16(d + 66);
    s->channels = le16(d + 68);
    s->npatterns = le16(d + 70);
    s->ninstruments = le16(d + 72);
    s->linear = le16(d + 74) & 1;
    s->speed = le16(d + 76);
    s->bpm = le16(d + 78);
    if (s->channels < 1 || s->channels > XM_MAX_CH)
        return "too many channels (the most is 32)";
    if (s->npatterns > XM_MAX_PATTERNS || s->ninstruments > XM_MAX_INSTR || s->length > 256)
        return "a damaged XM header";
    if (s->version < 0x104)
        return "an old XM version (only 1.04)";
    memcpy(s->orders, d + 80, 256);
    size_t off = 60 + hsize;
    // Patterns
    for (int p = 0; p < s->npatterns; ++p) {
        if (off + 9 > len)
            return "the file is cut short (patterns)";
        uint32_t plen = le32(d + off);
        int rows = le16(d + off + 5);
        uint16_t packed = le16(d + off + 7);
        off += plen;
        if (rows < 1 || rows > 256)
            rows = 64;
        xm_pattern_t *pt = &s->patterns[p];
        pt->rows = rows;
        pt->cells = xm_alloc((size_t)rows * s->channels * sizeof *pt->cells);
        if (!pt->cells)
            return "not enough memory";
        memset(pt->cells, 0, (size_t)rows * s->channels * sizeof *pt->cells);
        if (off + packed > len)
            return "the file is cut short (pattern data)";
        const uint8_t *q = d + off, *end = q + packed;
        for (int i = 0; packed && i < rows * s->channels && q < end; ++i) {
            xm_cell_t *c = &pt->cells[i];
            uint8_t b = *q++;
            if (b & 0x80) {
                if (b & 1 && q < end) c->note = *q++;
                if (b & 2 && q < end) c->instrument = *q++;
                if (b & 4 && q < end) c->volume = *q++;
                if (b & 8 && q < end) c->effect = *q++;
                if (b & 16 && q < end) c->param = *q++;
            } else {
                c->note = b;
                if (q + 4 <= end) {
                    c->instrument = q[0];
                    c->volume = q[1];
                    c->effect = q[2];
                    c->param = q[3];
                }
                q += 4;
            }
        }
        off += packed;
    }
    // Patterns the orders name that the file hasn't: empty ones of 64 rows.
    for (int i = 0; i < s->length; ++i) {
        int p = s->orders[i];
        if (p >= s->npatterns && !s->patterns[p].cells) {
            s->patterns[p].rows = 64;
            s->patterns[p].cells = xm_alloc(64 * s->channels * sizeof(xm_cell_t));
            if (!s->patterns[p].cells)
                return "not enough memory";
            memset(s->patterns[p].cells, 0, 64 * s->channels * sizeof(xm_cell_t));
        }
    }
    // Instruments
    s->instruments = xm_alloc((s->ninstruments ? s->ninstruments : 1) * sizeof *s->instruments);
    if (!s->instruments)
        return "not enough memory";
    memset(s->instruments, 0, (s->ninstruments ? s->ninstruments : 1) * sizeof *s->instruments);
    for (int i = 0; i < s->ninstruments; ++i) {
        if (off + 29 > len)
            return "the file is cut short (instruments)";
        xm_instrument_t *in = &s->instruments[i];
        const uint8_t *h = d + off;
        uint32_t isize = le32(h);
        name_copy(in->name, h + 4, 22);
        in->type = h[26];
        in->header_size = isize;
        int ns = le16(h + 27);
        uint32_t shsize = 40;
        if (isize >= 33 && off + 33 <= len)
            in->sample_header_size = le32(h + 29);
        if (ns > 0 && off + 243 <= len) {
            shsize = le32(h + 29);
            read_body(in, h + 33);
        }
        off += isize ? isize : 29;
        if (ns <= 0)
            continue;
        const char *err = read_samples(in, d, len, &off, ns, shsize);
        if (err)
            return err;
    }
    return NULL;
}

// ---- Saving ---------------------------------------------------------------------------------------

/// Writes 16 bits little-endian, moving on.
/// @param o where
/// @param v the value
static void put16(uint8_t **o, unsigned v) {
    (*o)[0] = v;
    (*o)[1] = v >> 8;
    *o += 2;
}

/// Writes 32 bits little-endian, moving on.
/// @param o where
/// @param v the value
static void put32(uint8_t **o, uint32_t v) {
    (*o)[0] = v;
    (*o)[1] = v >> 8;
    (*o)[2] = v >> 16;
    (*o)[3] = v >> 24;
    *o += 4;
}

/// A pattern packed as FT2 packs it, into o (NULL: just its size).
/// @param s the song
/// @param pt the pattern
/// @param o where (or NULL)
/// @return the size
static size_t pack(const xm_song_t *s, const xm_pattern_t *pt, uint8_t *o) {
    size_t n = 0;
    for (int i = 0; pt->cells && i < pt->rows * s->channels; ++i) {
        const xm_cell_t *c = &pt->cells[i];
        uint8_t f = 0x80 | (c->note ? 1 : 0) | (c->instrument ? 2 : 0) | (c->volume ? 4 : 0) |
                    (c->effect ? 8 : 0) | (c->param ? 16 : 0);
        if (f == 0x9f) {  // all five: unpacked is as short
            if (o) {
                o[n] = c->note;
                o[n + 1] = c->instrument;
                o[n + 2] = c->volume;
                o[n + 3] = c->effect;
                o[n + 4] = c->param;
            }
            n += 5;
            continue;
        }
        if (o)
            o[n] = f;
        ++n;
        const uint8_t v[5] = {c->note, c->instrument, c->volume, c->effect, c->param};
        for (int k = 0; k < 5; ++k)
            if (f & (1 << k)) {
                if (o)
                    o[n] = v[k];
                ++n;
            }
    }
    // FT2 stores an empty pattern as no data at all.
    bool empty = true;
    for (int i = 0; pt->cells && empty && i < pt->rows * s->channels; ++i)
        empty = !memcmp(&pt->cells[i], &(xm_cell_t){0}, sizeof(xm_cell_t));
    return empty ? 0 : n;
}

/// Writes an instrument's keymap, envelopes and the rest.
/// @param in the instrument
/// @param o where
static void write_body(const xm_instrument_t *in, uint8_t *o) {
    memcpy(o, in->keymap, 96);
    o += 96;
    for (int k = 0; k < 12; ++k) {
        put16(&o, in->venv.points[k].x);
        put16(&o, in->venv.points[k].y);
    }
    for (int k = 0; k < 12; ++k) {
        put16(&o, in->penv.points[k].x);
        put16(&o, in->penv.points[k].y);
    }
    *o++ = in->venv.count;
    *o++ = in->penv.count;
    *o++ = in->venv.sustain;
    *o++ = in->venv.loop_start;
    *o++ = in->venv.loop_end;
    *o++ = in->penv.sustain;
    *o++ = in->penv.loop_start;
    *o++ = in->penv.loop_end;
    *o++ = in->venv.flags;
    *o++ = in->penv.flags;
    *o++ = in->vib_type;
    *o++ = in->vib_sweep;
    *o++ = in->vib_depth;
    *o++ = in->vib_rate;
    put16(&o, in->fadeout);
}

/// The size of an instrument's sample headers and data.
/// @param in the instrument
/// @return bytes
static size_t samples_size(const xm_instrument_t *in) {
    size_t n = 0;
    for (int k = 0; k < in->nsamples; ++k)
        n += 40 + in->samples[k].length * (in->samples[k].bits16 ? 2 : 1);
    return n;
}

/// Writes an instrument's sample headers then data (delta encoded).
/// @param in the instrument
/// @param po where (moved past them)
static void write_samples(const xm_instrument_t *in, uint8_t **po) {
    uint8_t *o = *po;
    for (int k = 0; k < in->nsamples; ++k) {
        const xm_sample_t *sm = &in->samples[k];
        int w = sm->bits16 ? 2 : 1;
        put32(&o, sm->length * w);
        put32(&o, sm->loop_start * w);
        put32(&o, sm->loop_len * w);
        *o++ = sm->volume;
        *o++ = (uint8_t)sm->finetune;
        *o++ = sm->loop | (sm->bits16 ? 16 : 0);
        *o++ = sm->pan;
        *o++ = (uint8_t)sm->relnote;
        *o++ = sm->reserved;
        memcpy(o, sm->name, 22);
        o += 22;
    }
    for (int k = 0; k < in->nsamples; ++k) {
        const xm_sample_t *sm = &in->samples[k];
        if (sm->bits16) {
            int16_t prev = 0;
            for (uint32_t j = 0; j < sm->length; ++j) {
                put16(&o, (uint16_t)(sm->data[j] - prev));
                prev = sm->data[j];
            }
        } else {
            int8_t prev = 0;
            for (uint32_t j = 0; j < sm->length; ++j) {
                int8_t v = (int8_t)(sm->data[j] >> 8);
                *o++ = (uint8_t)(v - prev);
                prev = v;
            }
        }
    }
    *po = o;
}

size_t xm_save_size(const xm_song_t *s) {
    size_t n = 60 + 276;
    for (int p = 0; p < s->npatterns; ++p)
        n += 9 + pack(s, &s->patterns[p], NULL);
    for (int i = 0; i < s->ninstruments; ++i) {
        const xm_instrument_t *in = &s->instruments[i];
        n += in->nsamples ? 263 : in->header_size >= 29 && in->header_size <= 263 ? in->header_size : 29;
        n += samples_size(in);
    }
    return n;
}

void xm_save(const xm_song_t *s, uint8_t *o) {
    uint8_t *start = o;
    memset(o, 0, xm_save_size(s));
    memcpy(o, "Extended Module: ", 17);
    memcpy(o + 17, s->title, 20);  // the bytes as loaded (some trackers leave more after the 0)
    o[37] = 0x1a;
    memcpy(o + 38, s->tracker, 20);
    o += 58;
    put16(&o, 0x104);
    put32(&o, 276);
    put16(&o, s->length);
    put16(&o, s->restart);
    put16(&o, s->channels);
    put16(&o, s->npatterns);
    put16(&o, s->ninstruments);
    put16(&o, s->linear ? 1 : 0);
    put16(&o, s->speed);
    put16(&o, s->bpm);
    memcpy(o, s->orders, 256);
    o += 256;
    for (int p = 0; p < s->npatterns; ++p) {
        const xm_pattern_t *pt = &s->patterns[p];
        size_t n = pack(s, pt, NULL);
        put32(&o, 9);
        *o++ = 0;
        put16(&o, pt->rows ? pt->rows : 64);
        put16(&o, n);
        if (n)
            pack(s, pt, o);
        o += n;
    }
    for (int i = 0; i < s->ninstruments; ++i) {
        const xm_instrument_t *in = &s->instruments[i];
        uint8_t *h = o;
        uint32_t hs = in->nsamples ? 263 : in->header_size >= 29 && in->header_size <= 263 ? in->header_size : 29;
        put32(&o, hs);
        memcpy(o, in->name, 22);
        o += 22;
        *o++ = in->type;
        put16(&o, in->nsamples);
        if (!in->nsamples) {
            if (hs >= 33)
                put32(&o, in->sample_header_size);
            o = h + hs;
            continue;
        }
        put32(&o, 40);
        write_body(in, o);
        o = h + 263;
        write_samples(in, &o);
    }
    (void)start;
}

// ---- Instrument files (.XI) ---------------------------------------------------------------------
// "Extended Instrument: ", the name, 0x1a, the tracker, version 0x102, then
// the instrument as in an XM from its keymap on, 22 bytes kept, the number
// of samples, their headers and sounds.

bool xm_is_xi(const uint8_t *d, size_t len) {
    return len >= 298 && !memcmp(d, "Extended Instrument: ", 21);
}

const char *xm_xi_load(xm_instrument_t *in, const uint8_t *d, size_t len) {
    memset(in, 0, sizeof *in);
    if (!xm_is_xi(d, len))
        return "not an XI instrument file";
    name_copy(in->name, d + 21, 22);
    read_body(in, d + 66);
    in->header_size = 263;
    in->sample_header_size = 40;
    int ns = le16(d + 296);
    size_t off = 298;
    return ns > 0 ? read_samples(in, d, len, &off, ns, 40) : NULL;
}

size_t xm_xi_size(const xm_instrument_t *in) {
    return 298 + samples_size(in);
}

void xm_xi_save(const xm_instrument_t *in, uint8_t *o) {
    memset(o, 0, xm_xi_size(in));
    memcpy(o, "Extended Instrument: ", 21);
    memcpy(o + 21, in->name, 22);
    o[43] = 0x1a;
    memcpy(o + 44, "Pico Tracker", 12);
    o[64] = 0x02;
    o[65] = 0x01;
    write_body(in, o + 66);
    o[296] = in->nsamples;
    o += 298;
    write_samples(in, &o);
}

// ---- The player -------------------------------------------------------------------------------------

/// FT2's vibrato sine (half a wave, 0 up to 255 and back).
static const uint8_t sine_tab[32] = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                     224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                     212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

/// A voice: one channel's state.
typedef struct {
    /// The sound
    const xm_instrument_t *ins;
    const xm_sample_t *smp;  ///< the sample sounding
    uint64_t pos;        ///< 32.32
    uint32_t step;       ///< 16.16 samples per frame
    /// Sounding; going backwards (ping-pong loop).
    ///@{
    bool active, backwards;
    ///@}
    /// this tick's, 0..65536-ish
    ///@{
    int vol_l, vol_r;
    ///@}
    /// What the row asked for
    int note;            ///< 1..96 last note played (without relnote)
    int instrument;      ///< 1-based, 0 none
    /// The period, tone portamento's target, and what is heard.
    ///@{
    int period, target, out_period;
    ///@}
    /// The volume and pan, and what is heard.
    ///@{
    int volume, out_volume, pan, out_pan;
    ///@}
    /// The sample's finetune and relative note.
    ///@{
    int finetune, relnote;
    ///@}
    bool key_on;  ///< the key is down (not keyed off)
    int fade;            ///< 0..32768
    /// The volume and panning envelopes' positions.
    ///@{
    int venv_tick, penv_tick;
    ///@}
    /// 0..64
    ///@{
    int env_vol, env_pan;
    ///@}
    /// instrument auto-vibrato
    ///@{
    int av_pos, av_amp;
    ///@}
    /// Effects
    ///@{
    int effect, param, vcmd;
    ///@}
    /// Portamento memories: up, down, fine and extra fine, and tone portamento's speed.
    ///@{
    int porta_up, porta_down, fporta_up, fporta_down, xfporta_up, xfporta_down, porta_speed;
    ///@}
    /// Vibrato: speed, depth, position and waveform.
    ///@{
    int vib_speed, vib_depth, vib_pos, vib_wave;
    ///@}
    /// Tremolo: speed, depth, position and waveform.
    ///@{
    int trem_speed, trem_depth, trem_pos, trem_wave;
    ///@}
    /// Memories: volume slide, fine slides, global and panning slides, sample offset.
    ///@{
    int vslide, fvslide_up, fvslide_down, gvslide, pslide, offset;
    ///@}
    /// Retrigger: interval, volume change and count.
    ///@{
    int retrig, retrig_vol, retrig_count;
    ///@}
    int level;           ///< for meters, 0..64
    /// Tremor: on and off times, and position.
    ///@{
    int tremor, tremor_pos;
    ///@}
    /// Tremor has it off; glissando.
    ///@{
    bool tremor_off, gliss;
    ///@}
    /// E6x: the loop's start row and count.
    ///@{
    int loop_row, loop_count;
    ///@}
    /// The ticks for note delay, cut and key off.
    ///@{
    int delay_tick, cut_tick, keyoff_tick;
    ///@}
    xm_cell_t delayed;  ///< the cell held by EDx
} voice_t;

static struct {
    xm_song_t *song;  ///< the song
    /// Playing; paused.
    ///@{
    volatile bool playing, paused;
    ///@}
    bool loop;  ///< start over at the end
    /// The order, row, tick, speed, BPM and global volume.
    ///@{
    int order, row, tick, speed, bpm, gvol;
    ///@}
    int frames_left;  ///< frames until the next tick
    /// Where B and D jump to.
    ///@{
    int next_order, next_row;
    ///@}
    bool jump;  ///< B or D jumped this row
    /// EEx: rows to wait, and the count.
    ///@{
    int pattern_delay, delay_count;
    ///@}
    int master;          ///< 0..256
    voice_t *v;          ///< XM_MAX_CH of them, from the first play to xm_release (8 KB)
    int fixed_pattern;   ///< >= 0: that pattern over and over (the editor)
    bool muted[XM_MAX_CH];  ///< muted channels
    /// Requests from core 0
    volatile int req;    ///< 1 play, 2 stop, 3 jam
    /// Requests made and done.
    ///@{
    volatile unsigned req_seq, done_seq;
    ///@}
    xm_song_t *req_song;  ///< the song asked for
    /// The order, row and pattern asked for.
    ///@{
    int req_order, req_row, req_pattern;
    ///@}
    /// The note to jam: channel, instrument and note.
    ///@{
    int jam_ch, jam_ins, jam_note;
    ///@}
} X = {.loop = true, .master = 256, .fixed_pattern = -1};  ///< the player (core 1)

/// FT2 periods: 64 a semitone; C-4 (note 49, finetune 0) is 4608 in the linear table.
/// @param note the note (1-96)
/// @param finetune the finetune
/// @return the period
static int note_period(int note, int finetune) {
    // note 1..96 plus relnote; finetune -128..127 (16 a semitone step)
    if (X.song->linear)
        return 7680 - (note - 1) * 64 - finetune / 2;
    // Amiga: 1712 * 16 at C-0, halved each octave (1712 at C-4: ProTracker C-2 times 4).
    double n = (note - 1) + finetune / 128.0;
    return (int)(1712.0 * 16 * pow(2.0, -n / 12.0) + 0.5) / 4 * 4;
}

/// A period's step (16.16 per output frame).
/// @param period the period
/// @return the step
static uint32_t period_step(int period) {
    if (period < 1)
        period = 1;
    double hz;
    if (X.song->linear)
        hz = 8363.0 * pow(2.0, (4608 - period) / 768.0);
    else
        hz = 8363.0 * 1712.0 / period;  // C-4: 1712, 8363 Hz
    return (uint32_t)(hz * 65536.0 / XM_RATE);
}

/// An envelope's value at a tick.
/// @param e the envelope
/// @param tick the tick
/// @return 0-64
static int env_value(const xm_env_t *e, int tick) {
    if (e->count < 1)
        return 0;
    if (tick <= e->points[0].x || e->count == 1)
        return e->points[0].y;
    for (int i = 0; i + 1 < e->count; ++i) {
        int x0 = e->points[i].x, x1 = e->points[i + 1].x;
        if (tick < x1) {
            int y0 = e->points[i].y, y1 = e->points[i + 1].y;
            return x1 > x0 ? y0 + (y1 - y0) * (tick - x0) / (x1 - x0) : y1;
        }
    }
    return e->points[e->count - 1].y;
}

/// Moves an envelope on a tick: held at its sustain point while the key is
/// down, round its loop.
/// @param e the envelope
/// @param tick its position
/// @param key_on the key is down
static void env_advance(const xm_env_t *e, int *tick, bool key_on) {
    if (!(e->flags & 1) || e->count < 1)
        return;
    int t = *tick;
    if ((e->flags & 2) && key_on && e->sustain < e->count && t == e->points[e->sustain].x)
        return;
    ++t;
    if ((e->flags & 4) && e->loop_end < e->count && e->loop_start <= e->loop_end &&
        t >= e->points[e->loop_end].x &&
        !((e->flags & 2) && !key_on && e->sustain == e->loop_end && 0))
        t = e->points[e->loop_start].x;
    *tick = t;
}

/// Restarts a voice's envelopes, fadeout and auto-vibrato.
/// @param c the voice
static void reset_voice_env(voice_t *c) {
    c->venv_tick = c->penv_tick = 0;
    c->key_on = true;
    c->fade = 32768;
    c->av_pos = 0;
    c->av_amp = c->ins && c->ins->vib_sweep ? 0 : (c->ins ? c->ins->vib_depth << 8 : 0);
    c->tremor_pos = 0;
    if (!(c->vib_wave & 4))
        c->vib_pos = 0;
    if (!(c->trem_wave & 4))
        c->trem_pos = 0;
}

/// Key off: the envelopes go on past sustain (no envelope: silence).
/// @param c the voice
static void key_off(voice_t *c) {
    c->key_on = false;
    if (!c->ins || !(c->ins->venv.flags & 1))
        c->volume = 0;  // no envelope: silence (as FT2)
}

/// Starts the sample from the beginning.
/// @param c the voice
static void trigger(voice_t *c) {
    c->pos = 0;
    c->backwards = false;
    c->active = c->smp && c->smp->data && c->smp->length > 0;
}

/// The sample an instrument plays for a note.
/// @param c the voice
/// @param note the note
static void pick_sample(voice_t *c, int note) {
    c->smp = NULL;
    if (!c->ins || !c->ins->nsamples || note < 1 || note > 96)
        return;
    int k = c->ins->keymap[note - 1];
    if (k < c->ins->nsamples)
        c->smp = &c->ins->samples[k];
}

/// The volume column's tick 0 part.
/// @param c the voice
/// @param v the column
static void volume_column_tick0(voice_t *c, int v) {
    int hi = v >> 4, lo = v & 15;
    if (v >= 0x10 && v <= 0x50) {
        c->volume = v - 0x10;
        return;
    }
    switch (hi) {
    case 0x8: c->volume = c->volume - lo < 0 ? 0 : c->volume - lo; break;  // fine down
    case 0x9: c->volume = c->volume + lo > 64 ? 64 : c->volume + lo; break;  // fine up
    case 0xa: if (lo) c->vib_speed = lo * 4; break;
    case 0xb: if (lo) c->vib_depth = lo; break;
    case 0xc: c->pan = lo * 17; break;
    case 0xf: if (lo) c->porta_speed = lo * 64; break;
    }
}

/// Tone portamento's step.
/// @param c the voice
static void porta_to(voice_t *c) {
    if (!c->target)
        return;
    if (c->period < c->target) {
        c->period += c->porta_speed;
        if (c->period > c->target)
            c->period = c->target;
    } else if (c->period > c->target) {
        c->period -= c->porta_speed;
        if (c->period < c->target)
            c->period = c->target;
    }
}

/// Vibrato and tremolo's waveforms.
/// @param type 0 sine, 1 ramp, 2 square
/// @param pos the position
/// @return 0..255
static int wave(int type, int pos) {
    int i = (pos >> 2) & 31;
    switch (type & 3) {
    case 1: { int v = i << 3; return (pos & 0x80) ? ~v & 255 : v; }  // ramp
    case 2: case 3: return 255;
    default: return sine_tab[i];
    }
}

/// Axy: volume slide.
/// @param c the voice
/// @param param the parameter
static void vol_slide(voice_t *c, int param) {
    int up = param >> 4, down = param & 15;
    c->volume += up ? up : -down;
    c->volume = c->volume < 0 ? 0 : c->volume > 64 ? 64 : c->volume;
}

/// A row's cell, on tick 0 (or the delayed tick).
/// @param ci the channel
/// @param cell the cell
/// @param delayed it was held by EDx
static void start_cell(int ci, xm_cell_t cell, bool delayed) {
    voice_t *c = &X.v[ci];
    xm_song_t *s = X.song;
    int e = cell.effect, p = cell.param, x = p >> 4, y = p & 15;
    if (!delayed) {
        c->effect = e;
        c->param = p;
        c->vcmd = cell.volume;
        if (e == 0xe && x == 0xd && y) {  // EDx: everything waits for tick y
            c->delay_tick = y;
            c->delayed = cell;
            return;
        }
        c->delay_tick = 0;
    }
    bool porta = e == 3 || e == 5 || (cell.volume >> 4) == 0xf;
    int note = cell.note;
    // The instrument: its sample's volume and pan, envelopes from the start.
    if (cell.instrument) {
        if (cell.instrument <= s->ninstruments) {
            c->instrument = cell.instrument;
            c->ins = &s->instruments[cell.instrument - 1];
        } else {
            c->ins = NULL;  // no such instrument: silence
        }
    }
    if (note && note < 97 && !porta)
        pick_sample(c, note);
    else if (note && note < 97 && porta && !c->smp)
        pick_sample(c, note);
    if (cell.instrument && c->smp && note != 97) {
        c->volume = c->smp->volume;
        c->pan = c->smp->pan;
        reset_voice_env(c);
    }
    if (note == 97) {
        key_off(c);
    } else if (note) {
        if (c->smp) {
            c->relnote = c->smp->relnote;
            c->finetune = c->smp->finetune;
        }
        if (e == 0xe && x == 5)
            c->finetune = (y << 4) - 128;
        int n = note + c->relnote;
        if (n >= 1 && n <= 120) {
            c->note = note;
            int per = note_period(n, c->finetune);
            if (porta) {
                c->target = per;
            } else {
                c->period = per;
                trigger(c);
                if (!cell.instrument)
                    reset_voice_env(c);
                if (e == 9) {
                    if (p)
                        c->offset = p;
                    c->pos = (uint64_t)(c->offset * 256) << 32;
                    if (c->smp && (c->pos >> 32) >= c->smp->length)
                        c->active = false;
                }
            }
        }
    }
    if (cell.volume)
        volume_column_tick0(c, cell.volume);
    switch (e) {
    case 1: if (p) c->porta_up = p; break;
    case 2: if (p) c->porta_down = p; break;
    case 3: if (p) c->porta_speed = p * 4; break;
    case 4:
        if (x) c->vib_speed = x * 4;
        if (y) c->vib_depth = y;
        break;
    case 5: case 6: case 0xa: if (p) c->vslide = p; break;
    case 7:
        if (x) c->trem_speed = x * 4;
        if (y) c->trem_depth = y;
        break;
    case 8: c->pan = p; break;
    case 0xb:
        X.next_order = p;
        X.next_row = 0;
        X.jump = true;
        break;
    case 0xc: c->volume = p > 64 ? 64 : p; break;
    case 0xd:
        if (!X.jump)
            X.next_order = X.order + 1;
        X.next_row = x * 10 + y;
        X.jump = true;
        break;
    case 0xe:
        switch (x) {
        case 1: if (y) c->fporta_up = y; c->period -= c->fporta_up * 4; if (c->period < 1) c->period = 1; break;
        case 2: if (y) c->fporta_down = y; c->period += c->fporta_down * 4; break;
        case 3: c->gliss = y != 0; break;
        case 4: c->vib_wave = y; break;
        case 6:
            if (!y) {
                c->loop_row = X.row;
            } else if (!c->loop_count) {
                c->loop_count = y;
                X.next_row = c->loop_row;
                X.next_order = X.order;
                X.jump = true;
            } else if (--c->loop_count) {
                X.next_row = c->loop_row;
                X.next_order = X.order;
                X.jump = true;
            }
            break;
        case 7: c->trem_wave = y; break;
        case 0xa: if (y) c->fvslide_up = y; c->volume = c->volume + c->fvslide_up > 64 ? 64 : c->volume + c->fvslide_up; break;
        case 0xb: if (y) c->fvslide_down = y; c->volume = c->volume - c->fvslide_down < 0 ? 0 : c->volume - c->fvslide_down; break;
        case 0xc: c->cut_tick = y; if (!y) c->volume = 0; break;
        case 0xe: if (!X.delay_count) X.pattern_delay = y; break;
        }
        break;
    case 0xf:
        if (p && p < 32)
            X.speed = p;
        else if (p >= 32)
            X.bpm = p;
        break;
    case 0x10: X.gvol = p > 64 ? 64 : p; break;  // G
    case 0x11: if (p) c->gvslide = p; break;      // H
    case 0x14: c->keyoff_tick = p; if (!p) key_off(c); break;  // K
    case 0x15: c->venv_tick = c->penv_tick = p; break;  // L
    case 0x19: if (p) c->pslide = p; break;       // P
    case 0x1b:                                   // R
        if (x) c->retrig_vol = x;
        if (y) c->retrig = y;
        break;
    case 0x1d: if (p) c->tremor = p; break;       // T
    case 0x21:                                   // X
        if (x == 1) { if (y) c->xfporta_up = y; c->period -= c->xfporta_up; }
        if (x == 2) { if (y) c->xfporta_down = y; c->period += c->xfporta_down; }
        break;
    }
    if (e != 0x1b)
        c->retrig_count = 0;
}

/// Every tick but the row's first: effects that run over time.
/// @param c the voice
static void tick_effects(voice_t *c) {
    int e = c->effect, p = c->param, x = p >> 4, y = p & 15;
    int v = c->vcmd, vh = v >> 4, vl = v & 15;
    switch (vh) {
    case 0x6: c->volume = c->volume - vl < 0 ? 0 : c->volume - vl; break;
    case 0x7: c->volume = c->volume + vl > 64 ? 64 : c->volume + vl; break;
    case 0xb: break;  // vibrato: below
    case 0xd: c->pan = c->pan - vl < 0 ? 0 : c->pan - vl; break;
    case 0xe: c->pan = c->pan + vl > 255 ? 255 : c->pan + vl; break;
    case 0xf: porta_to(c); break;
    }
    switch (e) {
    case 1: c->period -= c->porta_up * 4; if (c->period < 1) c->period = 1; break;
    case 2: c->period += c->porta_down * 4; break;
    case 3: porta_to(c); break;
    case 5: porta_to(c); vol_slide(c, c->vslide); break;
    case 6: vol_slide(c, c->vslide); break;
    case 0xa: vol_slide(c, c->vslide); break;
    case 0xe:
        if (x == 9 && y && X.tick % y == 0)
            trigger(c);
        if (x == 0xc && X.tick == c->cut_tick)
            c->volume = 0;
        break;
    case 0x11: {
        int up = c->gvslide >> 4, down = c->gvslide & 15;
        X.gvol += up ? up : -down;
        X.gvol = X.gvol < 0 ? 0 : X.gvol > 64 ? 64 : X.gvol;
        break;
    }
    case 0x14: if (X.tick == c->keyoff_tick) key_off(c); break;
    case 0x19: {
        int r = c->pslide >> 4, l = c->pslide & 15;
        c->pan += r ? r : -l;
        c->pan = c->pan < 0 ? 0 : c->pan > 255 ? 255 : c->pan;
        break;
    }
    case 0x1b:
        if (c->retrig && ++c->retrig_count >= c->retrig) {
            static const int8_t add[16] = {0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0};
            c->retrig_count = 0;
            int r = c->retrig_vol;
            if (r == 6) c->volume = c->volume * 2 / 3;
            else if (r == 7) c->volume >>= 1;
            else if (r == 0xe) c->volume = c->volume * 3 / 2;
            else if (r == 0xf) c->volume *= 2;
            else c->volume += add[r];
            c->volume = c->volume < 0 ? 0 : c->volume > 64 ? 64 : c->volume;
            trigger(c);
        }
        break;
    }
    if (e == 4 || e == 6 || vh == 0xb)
        c->vib_pos += c->vib_speed;
    if (e == 7)
        c->trem_pos += c->trem_speed;
}

/// What is heard this tick: period and volume after vibrato, tremolo,
/// envelopes, fadeout and auto-vibrato.
/// @param c the voice
static void tick_output(voice_t *c) {
    int e = c->effect, p = c->param;
    int period = c->period, volume = c->volume;
    if (c->gliss && (e == 3 || e == 5) && X.song->linear)
        period = (period + 32) / 64 * 64;
    if (e == 0 && p) {  // arpeggio, FT2's way
        int pos = X.speed - X.tick % X.speed;
        pos = pos > 16 ? 2 : pos == 16 ? 0 : pos % 3;
        int add = pos == 1 ? p >> 4 : pos == 2 ? p & 15 : 0;
        if (X.song->linear)
            period -= add * 64;
        else if (add)
            period = (int)(period * pow(2.0, -add / 12.0) + 0.5);  // Amiga: semitones as a ratio
    }
    if (e == 4 || e == 6 || (c->vcmd >> 4) == 0xb) {
        int d = wave(c->vib_wave, c->vib_pos) * c->vib_depth >> 5;
        period += (c->vib_pos & 0x80) ? -d : d;
    }
    if (e == 7) {
        int d = wave(c->trem_wave, c->trem_pos) * c->trem_depth >> 6;
        volume += (c->trem_pos & 0x80) ? -d : d;
        volume = volume < 0 ? 0 : volume > 64 ? 64 : volume;
    }
    if (e == 0x1d && c->tremor) {
        int on = (c->tremor >> 4) + 1, off = (c->tremor & 15) + 1;
        if (c->tremor_pos >= on)
            volume = 0;
        if (++c->tremor_pos >= on + off)
            c->tremor_pos = 0;
    }
    const xm_instrument_t *in = c->ins;
    int env = 64, epan = 32;
    if (in) {
        if (in->venv.flags & 1) {
            env = env_value(&in->venv, c->venv_tick);
            env_advance(&in->venv, &c->venv_tick, c->key_on);
        }
        if (in->penv.flags & 1) {
            epan = env_value(&in->penv, c->penv_tick);
            env_advance(&in->penv, &c->penv_tick, c->key_on);
        }
        if (!c->key_on) {
            c->fade -= in->fadeout;
            if (c->fade < 0)
                c->fade = 0;
        }
        // Auto-vibrato
        if (in->vib_depth && in->vib_rate) {
            if (in->vib_sweep && c->key_on && c->av_amp < (in->vib_depth << 8))
                c->av_amp += (in->vib_depth << 8) / in->vib_sweep;
            if (c->av_amp > (in->vib_depth << 8))
                c->av_amp = in->vib_depth << 8;
            int pos = c->av_pos & 255, w;
            switch (in->vib_type & 3) {
            case 1: w = pos < 128 ? 64 : -64; break;
            case 2: w = ((pos >> 1) - 64) * -1; break;
            case 3: w = (pos >> 1) - 64; break;
            default: w = (pos < 128 ? 1 : -1) * sine_tab[(pos >> 2) & 31] / 4; break;
            }
            period += w * c->av_amp >> 14;
            c->av_pos += in->vib_rate;
        }
    }
    c->out_period = period;
    if (c->active && c->smp)
        c->step = period_step(period);
    // Volume: note 0..64, envelope 0..64, fadeout 0..32768, global 0..64.
    int64_t vol = (int64_t)volume * env * (c->fade >> 7) * X.gvol;  // up to 64*64*256*64
    int fv = (int)(vol >> 12);                                       // 0..16384
    int pan = c->pan + (epan - 32) * (128 - abs(c->pan - 128)) / 32;
    pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
    c->out_volume = volume;
    c->out_pan = pan;
    // 1.5 times: as loud as libopenmpt plays XMs (and a MOD made into one
    // about as loud as the MOD).
    c->vol_l = (int)((int64_t)fv * (256 - pan) * 3 >> 9);
    c->vol_r = (int)((int64_t)fv * pan * 3 >> 9);
}

/// Plays a row: each channel's cell, then where to go next.
static void do_row(void) {
    xm_song_t *s = X.song;
    X.jump = false;
    int pat = X.fixed_pattern >= 0 ? X.fixed_pattern : s->orders[X.order];
    const xm_pattern_t *pt = &s->patterns[pat];
    if (!pt->cells)
        return;
    if (X.row >= pt->rows)
        X.row = 0;
    for (int ci = 0; ci < s->channels; ++ci)
        start_cell(ci, pt->cells[X.row * s->channels + ci], false);
}

static void next_row(void) {
    xm_song_t *s = X.song;
    if (X.pattern_delay) {
        X.delay_count = X.pattern_delay;
        X.pattern_delay = 0;
    }
    if (X.delay_count) {
        --X.delay_count;
        return;
    }
    int rows = s->patterns[X.fixed_pattern >= 0 ? X.fixed_pattern : s->orders[X.order]].rows;
    if (X.jump) {
        X.row = X.next_row;
        if (X.fixed_pattern < 0)
            X.order = X.next_order;
    } else if (++X.row >= rows) {
        X.row = 0;
        if (X.fixed_pattern < 0)
            ++X.order;
    }
    if (X.order >= s->length) {
        if (!X.loop) {
            X.playing = false;
            for (int i = 0; i < XM_MAX_CH; ++i)
                X.v[i].active = false;
            return;
        }
        X.order = s->restart < s->length ? s->restart : 0;
    }
    do_row();
}

/// A tick: effects, then what is heard.
static void do_tick(void) {
    xm_song_t *s = X.song;
    if (X.tick) {  // (tick 0's effects ran in do_row)
        for (int ci = 0; ci < s->channels; ++ci) {
            voice_t *c = &X.v[ci];
            if (c->delay_tick && X.tick == c->delay_tick) {
                c->delay_tick = 0;
                start_cell(ci, c->delayed, true);
            }
            tick_effects(c);
        }
    }
    for (int ci = 0; ci < s->channels; ++ci)
        tick_output(&X.v[ci]);
}

/// Takes a request from core 0 (play, stop, jam).
static void take_requests(void) {
    if (X.req_seq == X.done_seq)
        return;
    if (!X.v) {  // released (or never had voices): nothing to play
        X.playing = false;
        X.done_seq = X.req_seq;
        return;
    }
    if (X.req == 1) {
        X.song = X.req_song;
        memset(X.v, 0, XM_MAX_CH * sizeof *X.v);
        for (int i = 0; i < XM_MAX_CH; ++i)
            X.v[i].pan = 128;
        X.order = X.req_order < X.song->length ? X.req_order : 0;
        X.fixed_pattern = X.req_pattern;
        X.row = X.req_row;
        X.speed = X.song->speed ? X.song->speed : 6;
        X.bpm = X.song->bpm ? X.song->bpm : 125;
        X.gvol = 64;
        X.tick = 0;
        X.pattern_delay = X.delay_count = 0;
        X.playing = true;
        do_row();
        do_tick();
        X.frames_left = XM_RATE * 5 / (X.bpm * 2);
    } else if (X.req == 2) {
        X.playing = false;
        for (int i = 0; i < XM_MAX_CH; ++i)
            X.v[i].active = false;
    } else if (X.req == 3) {
        // A note now on a channel (the editor's keyboard): played as a
        // cell would be, and kept ticking for its envelopes.
        if (!X.playing) {
            X.song = X.req_song;
            if (!X.speed) {
                X.speed = X.song->speed ? X.song->speed : 6;
                X.bpm = X.song->bpm ? X.song->bpm : 125;
            }
            X.gvol = X.gvol ? X.gvol : 64;
        }
        voice_t *c = &X.v[X.jam_ch];
        if (!c->pan && !c->active)
            c->pan = 128;
        start_cell(X.jam_ch, (xm_cell_t){(uint8_t)X.jam_note, (uint8_t)X.jam_ins, 0, 0, 0}, true);
        c->effect = c->param = c->vcmd = 0;
        tick_output(c);
    }
    X.done_seq = X.req_seq;
}

/// Mixes n frames of one voice into out.
/// @param c the voice
/// @param out the mix
/// @param n frames
static MOD_HOT void mix_voice(voice_t *c, int32_t *out, int n) {
    const xm_sample_t *sm = c->smp;
    const int16_t *d = sm->data;
    uint32_t len = sm->length, ls = sm->loop_start, le = sm->loop_start + sm->loop_len;
    int vl = c->vol_l, vr = c->vol_r;
    uint64_t pos = c->pos;
    uint64_t step = (uint64_t)c->step << 16;
    int peak = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t ip = (uint32_t)(pos >> 32);
        if (!c->backwards) {
            uint32_t end = sm->loop ? le : len;
            if (ip >= end) {
                if (!sm->loop) {
                    c->active = false;
                    break;
                }
                if (sm->loop == 1) {
                    while (ip >= le)
                        ip -= sm->loop_len;
                } else {
                    // Ping-pong: back from the end.
                    uint32_t over = ip - le;
                    ip = le - 1 - (over % sm->loop_len);
                    c->backwards = true;
                }
                pos = (uint64_t)ip << 32 | (uint32_t)pos;
            }
        } else if (ip < ls || (int64_t)pos < 0) {
            uint32_t under = ip < ls ? ls - ip : 0;
            ip = ls + (under % sm->loop_len);
            pos = (uint64_t)ip << 32 | (uint32_t)pos;
            c->backwards = false;
        }
        int s0 = d[ip];
        uint32_t in = ip + 1;
        if (sm->loop == 1 && in >= le)
            in = ls;
        int s1 = in < len ? d[in] : s0;
        int frac = (uint32_t)pos >> 16;
        int v = s0 + (((s1 - s0) * frac) >> 16);
        int l = (v * vl) >> 16, r = (v * vr) >> 16;  // a voice hard to one side: +-8192
        out[2 * i] += l;
        out[2 * i + 1] += r;
        int a = (l < 0 ? -l : l) + (r < 0 ? -r : r);
        if (a > peak)
            peak = a;
        if (c->backwards)
            pos -= step;
        else
            pos += step;
    }
    c->pos = pos;
    int lv = peak >> 7;
    c->level = lv > c->level ? (lv > 64 ? 64 : lv) : c->level - (c->level > 0);
}

MOD_HOT bool xm_render(int32_t *out, int n) {
    take_requests();
    if (X.paused || !X.song || !X.v)
        return X.song != NULL;
    bool any = false;
    while (n > 0) {
        if (X.frames_left <= 0 && X.bpm) {
            if (X.playing) {
                if (++X.tick >= X.speed) {
                    X.tick = 0;
                    next_row();
                }
                if (X.playing)
                    do_tick();
            } else {
                // Stopped: notes played from the keyboard still need their
                // envelopes and fadeout.
                for (int ci = 0; ci < X.song->channels; ++ci)
                    if (X.v[ci].active)
                        tick_output(&X.v[ci]);
            }
            X.frames_left = XM_RATE * 5 / (X.bpm * 2);
        }
        int chunk = X.bpm && X.frames_left < n ? X.frames_left : n;
        for (int ci = 0; ci < X.song->channels; ++ci) {
            voice_t *c = &X.v[ci];
            if (!c->active || !c->smp || (!c->vol_l && !c->vol_r && !c->step))
                continue;
            if ((!c->vol_l && !c->vol_r) || X.muted[ci]) {
                // Silent: just move it on.
                int32_t scratch[2 * 64];
                for (int k = 0; k < chunk; k += 64) {
                    int m = chunk - k < 64 ? chunk - k : 64;
                    memset(scratch, 0, sizeof scratch);
                    mix_voice(c, scratch, m);
                }
                continue;
            }
            mix_voice(c, out, chunk);
            any = true;
        }
        if (X.master != 256)
            for (int i = 0; i < chunk * 2; ++i)
                out[i] = out[i] * X.master >> 8;
        out += chunk * 2;
        n -= chunk;
        if (X.bpm)
            X.frames_left -= chunk;
    }
    return any || X.playing;
}

// ---- Core 0 side ---------------------------------------------------------------------------------

/// Makes a request (after the last is taken).
/// @param what 1 play, 2 stop, 3 jam
static void request(int what) {
    for (int i = 0; i < 2000000 && X.done_seq != X.req_seq; ++i)
        __sync_synchronize();
    X.req = what;
    __sync_synchronize();
    ++X.req_seq;
}

/// Waits for the player to take the last request.
static void wait_done(void) {
    for (int i = 0; i < 2000000 && X.done_seq != X.req_seq; ++i)
        __sync_synchronize();
}

/// The voices, made on first use (core 0). False if there's no memory.
/// @return there are voices
static bool voices(void) {
    if (!X.v)
        X.v = calloc(XM_MAX_CH, sizeof *X.v);
    return X.v != NULL;
}

void xm_release(void) {
    wait_done();
    X.playing = false;
    X.song = NULL;
    voice_t *v = X.v;
    X.v = NULL;
    free(v);
}

bool xm_play(xm_song_t *s, int order, int row) {
    if (!voices())
        return false;
    wait_done();
    X.req_song = s;
    X.req_order = order;
    X.req_row = row;
    X.req_pattern = -1;
    request(1);
    return true;
}

void xm_play_pattern(xm_song_t *s, int pattern, int row) {
    if (!voices())
        return;
    wait_done();
    X.req_song = s;
    X.req_order = 0;
    X.req_row = row;
    X.req_pattern = pattern;
    request(1);
}

void xm_jam(xm_song_t *s, int ch, int instrument, int note) {
    if (!voices())
        return;
    wait_done();
    X.req_song = s;
    X.jam_ch = ch;
    X.jam_ins = instrument;
    X.jam_note = note;
    request(3);
}

void xm_stop(void) {
    request(2);
    wait_done();
}

void xm_mute(int ch, bool muted) {
    if (ch >= 0 && ch < XM_MAX_CH)
        X.muted[ch] = muted;
}

bool xm_muted(int ch) {
    return ch >= 0 && ch < XM_MAX_CH && X.muted[ch];
}

void xm_set_loop(bool loop) {
    X.loop = loop;
}

void xm_pause(bool paused) {
    X.paused = paused;
}

void xm_set_volume(int percent) {
    X.master = (percent < 0 ? 0 : percent > 100 ? 100 : percent) * 256 / 100;
}

void xm_get_status(xm_status_t *st) {
    st->playing = X.playing;
    st->order = X.order;
    st->pattern = X.song ? X.song->orders[X.order] : 0;
    st->row = X.row;
    st->speed = X.speed;
    st->bpm = X.bpm;
    st->global_volume = X.gvol;
    int n = 0;
    for (int i = 0; X.v && X.song && i < X.song->channels; ++i)
        n += X.v[i].active;
    st->active = n;
    for (int i = 0; i < XM_MAX_CH; ++i)
        st->level[i] = X.v && X.v[i].active ? X.v[i].level : 0;
}

MOD_HOT bool xm_render_frame(int32_t *left, int32_t *right) {
    // For the audio code's one-frame-at-a-time source: blocks of 32.
    static int32_t buf[2 * 32];
    static int have, at;
    static bool more = true;
    if (at >= have) {
        memset(buf, 0, sizeof buf);
        more = xm_render(buf, 32);
        have = 32;
        at = 0;
    }
    *left = buf[2 * at];
    *right = buf[2 * at + 1];
    ++at;
    return more;
}
