/**
 * @file mod2xm.c
 * @brief A MOD song made into an XM song that plays the same.
 *
 * FT2's Amiga frequencies (not linear), notes from the periods (ProTracker's
 * C-1 is FT2's C-3, the same pitch), one instrument per sample with its
 * sound kept 8-bit, finetunes scaled to FT2's, the effects as they are (the
 * XM's are ProTracker's plus more). XM has no fixed channel panning, so the
 * Amiga's left-right-right-left goes into the volume column as a panning
 * command beside each sample number. Plain C (tools/xmtest checks it).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "mod.h"
#include "xm.h"

/// A MOD sample as an XM instrument.
/// @param in the instrument
/// @param m the sample
/// @return NULL, or an error
static const char *convert_instrument(xm_instrument_t *in, const mod_sample_t *m) {
    memset(in, 0, sizeof *in);
    memcpy(in->name, m->name, 22);
    in->header_size = 263;
    in->sample_header_size = 40;
    if (!m->length || !m->data)
        return NULL;
    xm_sample_t *sm = xm_alloc(sizeof *sm);
    if (!sm)
        return "not enough memory";
    memset(sm, 0, sizeof *sm);
    in->samples = sm;
    in->nsamples = 1;
    memcpy(sm->name, m->name, 22);
    sm->length = m->length;
    sm->data = xm_alloc((sm->length + 1) * sizeof(int16_t));
    if (!sm->data)
        return "not enough memory";
    for (uint32_t i = 0; i < sm->length; ++i)
        sm->data[i] = (int16_t)(m->data[i] * 256);
    sm->data[sm->length] = sm->data[sm->length - 1];
    sm->bits16 = false;
    sm->volume = m->volume > 64 ? 64 : m->volume;
    // ProTracker's C-2 is 8287 Hz (the PAL Amiga's clock), FT2's C-4 8363 Hz:
    // 15.8 cents (20 finetune steps) lower to sound the same.
    int ft = m->finetune * 16 - 20;
    if (ft < -128) {
        sm->relnote = -1;
        ft += 128;
    }
    sm->finetune = (int8_t)ft;
    sm->pan = 128;
    if (m->loop_len > 2 && m->loop_start < m->length) {
        sm->loop = 1;
        sm->loop_start = m->loop_start;
        sm->loop_len = m->loop_start + m->loop_len > m->length ? m->length - m->loop_start : m->loop_len;
    }
    return NULL;
}

const char *xm_from_mod(xm_song_t *x, const mod_song_t *m, int speed, int bpm, int separation) {
    memset(x, 0, sizeof *x);
    memcpy(x->title, m->title, 20);
    memcpy(x->tracker, "Pico Tracker", 12);
    x->version = 0x104;
    x->channels = m->channels;
    x->length = m->length;
    x->restart = m->restart < m->length ? m->restart : 0;
    x->linear = false;
    x->speed = speed;
    x->bpm = bpm;
    memcpy(x->orders, m->orders, 128);
    // The instruments: up to the last sample with something in it.
    int n = 0;
    for (int i = 0; i < m->nsamples; ++i)
        if (m->samples[i].length || m->samples[i].name[0])
            n = i + 1;
    x->ninstruments = n ? n : 1;
    x->instruments = xm_alloc(x->ninstruments * sizeof *x->instruments);
    if (!x->instruments)
        return "not enough memory";
    memset(x->instruments, 0, x->ninstruments * sizeof *x->instruments);
    for (int i = 0; i < n; ++i) {
        const char *err = convert_instrument(&x->instruments[i], &m->samples[i]);
        if (err)
            return err;
    }
    // Panning: hard left or right at 100% separation, towards the middle below.
    separation = separation < 0 ? 0 : separation > 100 ? 100 : separation;
    int off = 128 * separation / 100;
    uint8_t pan_left = (uint8_t)((128 - off) >> 4), pan_right = (uint8_t)((128 + off > 255 ? 255 : 128 + off) >> 4);
    // The speed where each row is first played (following Fxx down the
    // order list), for arpeggios: FT2 goes base, y, x where ProTracker goes
    // base, x, y; at speeds that are multiples of 3 swapping x and y makes
    // them the same.
    uint8_t (*speed_at)[MOD_ROWS] = xm_alloc(MOD_MAX_PATTERNS * MOD_ROWS);
    if (!speed_at)
        return "not enough memory";
    memset(speed_at, 0, MOD_MAX_PATTERNS * MOD_ROWS);
    int sp = speed;
    for (int o = 0; o < m->length; ++o) {
        int p = m->orders[o];
        for (int r = 0; r < MOD_ROWS && p < MOD_MAX_PATTERNS; ++r) {
            for (int c = 0; c < m->channels; ++c) {
                mod_cell_t v = mod_get(m, p, r, c);
                if (v.effect == 0xf && v.param && v.param < 0x20)
                    sp = v.param;
            }
            if (!speed_at[p][r])
                speed_at[p][r] = sp;
        }
    }
    x->npatterns = m->npatterns;
    for (int p = 0; p < m->npatterns; ++p) {
        xm_pattern_t *pt = &x->patterns[p];
        pt->rows = MOD_ROWS;
        pt->cells = xm_alloc(MOD_ROWS * m->channels * sizeof(xm_cell_t));
        if (!pt->cells) {
            xm_free(speed_at);
            return "not enough memory";
        }
        for (int r = 0; r < MOD_ROWS; ++r)
            for (int c = 0; c < m->channels; ++c) {
                mod_cell_t v = mod_get(m, p, r, c);
                xm_cell_t *o = &pt->cells[r * m->channels + c];
                memset(o, 0, sizeof *o);
                if (v.period) {
                    int note = mod_period_note(v.period);
                    if (note >= 0)
                        o->note = note + 37;
                }
                o->instrument = v.sample;
                o->effect = v.effect;
                o->param = v.param;
                if (v.effect == 0xe && (v.param >> 4) == 0)
                    o->effect = o->param = 0;  // the Amiga's filter
                int at = speed_at[p][r] ? speed_at[p][r] : speed;
                if (v.effect == 0 && v.param && at % 3 == 0)
                    o->param = (uint8_t)(v.param << 4 | v.param >> 4);
                if (v.sample && separation) {
                    bool left = (c & 3) == 0 || (c & 3) == 3;
                    o->volume = 0xc0 | (left ? pan_left : pan_right);
                }
            }
    }
    xm_free(speed_at);
    return NULL;
}
