/**
 * @file trk_xmi.c
 * @brief The tracker's instrument screen for XM songs (F3).
 *
 * the instruments, an instrument's settings and its samples, the chosen
 * sample's settings and waveform with its loop, the volume and panning
 * envelopes (points dragged with the mouse) and the note-to-sample map. See
 * trk.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit a field on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvi.h"
#include "mc.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "psram_heap.h"
#include "trk.h"
#include "xm.h"

// ---- Layout -----------------------------------------------------------------------------------------
#define TOP_Y    16  ///< the panels under the menu bar
#define LIST_W   192    ///< the instruments
#define PARAM_X  192    ///< the instrument's and the sample's settings
#define PARAM_W  256  ///< the settings' width
#define SMP_X    448    ///< the instrument's samples
#define SMP_W    192  ///< the samples' width
#define TOP_H    176  ///< the panels' height
#define WAVE_Y   192  ///< the waveform's top
#define WAVE_H   96  ///< its height
#define ENV_Y    290  ///< the envelopes' top
#define ENV_H    112  ///< their height
#define ENV_W    320  ///< an envelope's width
#define ENVBAR_Y (ENV_Y + ENV_H)  ///< the envelope buttons' row
#define KEYS_Y   424  ///< the keymap's row
#define KEYS_X   32  ///< its left
#define KEY_W    6  ///< a key's width
#define STATUS_Y 464  ///< the status line's top
#define ROWS     10  ///< instrument rows shown

/// The settings, in the order Up/Down go through them.
enum {
    F_NAME, F_FADEOUT, F_VTYPE, F_VSWEEP, F_VDEPTH, F_VRATE,
    F_SNAME, F_VOLUME, F_PAN, F_FINE, F_REL, F_LOOP, F_LSTART, F_LLEN,
    F_VENV, F_VSUS, F_VLOOPS, F_VLOOPE, F_PENV, F_PSUS, F_PLOOPS, F_PLOOPE,
    F_COUNT,
};
/// The settings' names.
static const char *const field_names[F_COUNT] = {
    "Name", "Fadeout", "Vib type", "Vib sweep", "Vib depth", "Vib rate",
    "Sample", "Volume", "Pan", "Finetune", "Rel note", "Loop", "Loop at", "Loop len",
    "Vol env", "Sustain", "Loop from", "Loop to", "Pan env", "Sustain", "Loop from", "Loop to",
};

/// What a click hits.
enum {
    IH_INSTR = 500, IH_SAMPLE, IH_FIELD, IH_DEC, IH_INC, IH_WAVE, IH_ENV, IH_KEY, IH_TOOL,
};
/// The tools menu's items.
enum { IT_LOAD = 1, IT_ADD, IT_DEL, IT_NEWINS, IT_PATTERNS };

static struct {
    int smp;          ///< the sample chosen in the instrument (0-based)
    /// The setting with the keyboard, and the first instrument shown.
    ///@{
    int field, ins_top;
    ///@}
    /// dragging an envelope point (env 0 vol, 1 pan)
    ///@{
    int drag_env, drag_point;
    ///@}
    int drag_loop;    ///< 1 loop start, 2 loop end
    bool selecting;   ///< dragging out a selection on the waveform
    /// the selection, in samples (sel1 > sel0 if any)
    ///@{
    uint32_t sel0, sel1;
    ///@}
} I;  ///< the instrument editor's state

/// The song.
/// @return it
static xm_song_t *S(void) {
    return T->xm;
}

/// The instrument chosen, made (and the list grown) if it doesn't exist yet.
/// @param make make it if it doesn't exist
/// @return it, or NULL
static xm_instrument_t *instrument(bool make) {
    xm_song_t *s = S();
    int n = T->sample;
    if (n <= s->ninstruments)
        return &s->instruments[n - 1];
    if (!make)
        return NULL;
    // The player points into the list: stop it while the list moves.
    xm_stop();
    xm_instrument_t *list = xm_alloc(n * sizeof *list);
    if (!list) {
        trk_message("Not enough memory.");
        return NULL;
    }
    memset(list, 0, n * sizeof *list);
    memcpy(list, s->instruments, s->ninstruments * sizeof *list);
    for (int i = s->ninstruments; i < n; ++i)
        list[i].header_size = 263, list[i].sample_header_size = 40;
    xm_free(s->instruments);
    s->instruments = list;
    s->ninstruments = n;
    T->dirty = true;
    return &list[n - 1];
}

/// The sample chosen.
/// @return it, or NULL
static xm_sample_t *sample(void) {
    xm_instrument_t *in = instrument(false);
    if (!in || I.smp >= in->nsamples)
        return NULL;
    return &in->samples[I.smp];
}

// ---- Changing things -------------------------------------------------------------------------

/// Keeps a sample's loop inside it.
/// @param sm the sample
static void fix_loop(xm_sample_t *sm) {
    if (sm->loop_start > sm->length)
        sm->loop_start = sm->length;
    if (sm->loop_start + sm->loop_len > sm->length)
        sm->loop_len = sm->length - sm->loop_start;
    if (!sm->loop_len)
        sm->loop = 0;
}

/// A value kept within limits.
/// @param v the value
/// @param lo the lowest
/// @param hi the highest
/// @return v, clamped
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/// The envelope a setting belongs to.
/// @param f the setting
/// @return it, or NULL
static xm_env_t *env_of(int f) {
    xm_instrument_t *in = instrument(false);
    if (!in)
        return NULL;
    return f >= F_PENV ? &in->penv : &in->venv;
}

/// Changes a setting by a step.
/// @param f the setting
/// @param d the step (16 or more: a big one)
static void field_step(int f, int d) {
    xm_instrument_t *in = instrument(f <= F_VRATE);
    xm_sample_t *sm = sample();
    int big = abs(d) >= 16 ? 16 : 1, step = d > 0 ? big : -big;
    switch (f) {
    case F_FADEOUT: if (in) in->fadeout = clampi(in->fadeout + step * 16, 0, 4095); break;
    case F_VTYPE: if (in) in->vib_type = (in->vib_type + (d > 0 ? 1 : 3)) % 4; break;
    case F_VSWEEP: if (in) in->vib_sweep = clampi(in->vib_sweep + step, 0, 255); break;
    case F_VDEPTH: if (in) in->vib_depth = clampi(in->vib_depth + step, 0, 15); break;
    case F_VRATE: if (in) in->vib_rate = clampi(in->vib_rate + step, 0, 63); break;
    case F_VOLUME: if (sm) sm->volume = clampi(sm->volume + step, 0, 64); break;
    case F_PAN: if (sm) sm->pan = clampi(sm->pan + step * 4, 0, 255); break;
    case F_FINE: if (sm) sm->finetune = clampi(sm->finetune + step, -128, 127); break;
    case F_REL: if (sm) sm->relnote = clampi(sm->relnote + step, -96, 95); break;
    case F_LOOP:
        if (sm) {
            sm->loop = (sm->loop + (d > 0 ? 1 : 2)) % 3;
            if (sm->loop && !sm->loop_len)
                sm->loop_start = 0, sm->loop_len = sm->length;
            fix_loop(sm);
        }
        break;
    case F_LSTART:
    case F_LLEN:
        if (sm && sm->length) {
            int by = (int)(sm->length / 640 + 1) * step;
            if (f == F_LSTART) {
                int end = sm->loop_start + sm->loop_len, v = clampi((int)sm->loop_start + by, 0, end - 1);
                sm->loop_start = v;
                sm->loop_len = end - v;
            } else {
                sm->loop_len = clampi((int)sm->loop_len + by, 1, sm->length - sm->loop_start);
            }
            if (!sm->loop)
                sm->loop = 1;
        }
        break;
    case F_VENV: case F_PENV: {
        xm_env_t *e = env_of(f);
        if (e) {
            e->flags ^= 1;
            if ((e->flags & 1) && e->count < 2) {
                // Something to start from: up, held, then down.
                e->count = 3;
                e->points[0] = (xm_point_t){0, f == F_VENV ? 64 : 32};
                e->points[1] = (xm_point_t){16, f == F_VENV ? 48 : 32};
                e->points[2] = (xm_point_t){64, f == F_VENV ? 0 : 32};
            }
        }
        break;
    }
    case F_VSUS: case F_PSUS: case F_VLOOPS: case F_PLOOPS: case F_VLOOPE: case F_PLOOPE: {
        xm_env_t *e = env_of(f);
        if (!e || !e->count)
            break;
        bool sus = f == F_VSUS || f == F_PSUS, start = f == F_VLOOPS || f == F_PLOOPS;
        uint8_t *which = sus ? &e->sustain : start ? &e->loop_start : &e->loop_end;
        uint8_t bit = sus ? 2 : 4;
        // Off, then each point in turn.
        int v = (e->flags & bit) ? *which + 1 : 0;
        v = (v + (d > 0 ? 1 : e->count)) % (e->count + 1);
        if (v) {
            *which = v - 1;
            e->flags |= bit;
        } else if (sus || start) {
            e->flags &= ~bit;
        }
        if (e->loop_end < e->loop_start)
            e->loop_end = e->loop_start;
        break;
    }
    default:
        return;
    }
    T->dirty = true;
}

/// Edits a setting by typing.
/// @param f the setting
static void field_enter(int f) {
    xm_instrument_t *in = instrument(f == F_NAME);
    xm_sample_t *sm = sample();
    char buf[24];
    if (f == F_NAME || f == F_SNAME) {
        char *name = f == F_NAME ? (in ? in->name : NULL) : (sm ? sm->name : NULL);
        if (!name)
            return;
        snprintf(buf, sizeof buf, "%.22s", name);
        trk_dialog_begin();
        if (gui_input(field_names[f], "Name (22 letters):", buf, 23)) {
            memset(name, 0, 23);
            memcpy(name, buf, strnlen(buf, 22));
            T->dirty = true;
        }
        trk_dialog_end();
        return;
    }
    if (f == F_LOOP || f == F_VENV || f == F_PENV) {
        field_step(f, 1);
        return;
    }
    int v = 0;
    switch (f) {
    case F_FADEOUT: v = in ? in->fadeout : 0; break;
    case F_VSWEEP: v = in ? in->vib_sweep : 0; break;
    case F_VDEPTH: v = in ? in->vib_depth : 0; break;
    case F_VRATE: v = in ? in->vib_rate : 0; break;
    case F_VOLUME: v = sm ? sm->volume : 0; break;
    case F_PAN: v = sm ? sm->pan : 0; break;
    case F_FINE: v = sm ? sm->finetune : 0; break;
    case F_REL: v = sm ? sm->relnote : 0; break;
    case F_LSTART: v = sm ? (int)sm->loop_start : 0; break;
    case F_LLEN: v = sm ? (int)sm->loop_len : 0; break;
    default: field_step(f, 1); return;
    }
    snprintf(buf, sizeof buf, "%d", v);
    trk_dialog_begin();
    bool ok = gui_input(field_names[f], "New value:", buf, 12);
    trk_dialog_end();
    if (!ok)
        return;
    v = atoi(buf);
    switch (f) {
    case F_FADEOUT: if (in) in->fadeout = clampi(v, 0, 4095); break;
    case F_VSWEEP: if (in) in->vib_sweep = clampi(v, 0, 255); break;
    case F_VDEPTH: if (in) in->vib_depth = clampi(v, 0, 15); break;
    case F_VRATE: if (in) in->vib_rate = clampi(v, 0, 63); break;
    case F_VOLUME: if (sm) sm->volume = clampi(v, 0, 64); break;
    case F_PAN: if (sm) sm->pan = clampi(v, 0, 255); break;
    case F_FINE: if (sm) sm->finetune = clampi(v, -128, 127); break;
    case F_REL: if (sm) sm->relnote = clampi(v, -96, 95); break;
    case F_LSTART: if (sm) { sm->loop_start = clampi(v, 0, sm->length); if (!sm->loop) sm->loop = 1; fix_loop(sm); } break;
    case F_LLEN: if (sm) { sm->loop_len = clampi(v, 0, sm->length); if (!sm->loop) sm->loop = 1; fix_loop(sm); } break;
    }
    T->dirty = true;
}

/// Relative note and finetune so that C-4 plays the sample at `rate` (FT2
/// plays C-4 at 8363 Hz with neither).
/// @param sm the sample
/// @param rate its rate (Hz)
static void tune_to_rate(xm_sample_t *sm, double rate) {
    if (rate < 100)
        return;
    double semis = 12 * log2(rate / 8363.0);
    int rel = (int)floor(semis + 0.5);
    int ft = (int)lround((semis - rel) * 128);
    sm->relnote = clampi(rel, -96, 95);
    sm->finetune = clampi(ft, -128, 127);
}

/// The rate C-4 plays a sample at.
/// @param sm the sample
/// @return Hz
static double rate_of(const xm_sample_t *sm) {
    return 8363.0 * pow(2.0, (sm->relnote + sm->finetune / 128.0) / 12.0);
}

/// A WAV file as 16-bit mono (stereo mixed), its loop from the smpl chunk.
/// @param f the file
/// @param size its size
/// @param sm the sample
/// @return NULL, or an error
static const char *load_wav16(const uint8_t *f, uint32_t size, xm_sample_t *sm) {
    if (size < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4))
        return "only WAV files";
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0, la = 0, lb = 0;
    bool loop = false;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t n = f[off + 4] | f[off + 5] << 8 | f[off + 6] << 16 | (uint32_t)f[off + 7] << 24;
        const uint8_t *c = f + off + 8;
        if (n > size - off - 8)
            n = size - off - 8;
        if (!memcmp(f + off, "fmt ", 4) && n >= 16) {
            format = c[0] | c[1] << 8;
            channels = c[2] | c[3] << 8;
            bits = c[14] | c[15] << 8;
            rate = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            if (format == 0xfffe && n >= 26)
                format = c[24] | c[25] << 8;
        } else if (!memcmp(f + off, "data", 4)) {
            data = c;
            data_len = n;
        } else if (!memcmp(f + off, "smpl", 4) && n >= 60 && c[28]) {
            loop = true;
            la = c[44] | c[45] << 8 | c[46] << 16 | (uint32_t)c[47] << 24;
            lb = (c[48] | c[49] << 8 | c[50] << 16 | (uint32_t)c[51] << 24) + 1;
        }
        off += 8 + n + (n & 1);
    }
    if (!data || !channels || !bits)
        return "a WAV file without sound";
    if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(format == 3 && bits == 32))
        return "only PCM or float WAV files";
    int bytes = bits / 8;
    uint32_t frames = data_len / (bytes * channels);
    if (frames > (4u << 20) / 2)
        frames = (4u << 20) / 2;  // 4 MB of sound at most
    int16_t *d = xm_alloc((frames + 1) * sizeof *d);
    if (!d)
        return "not enough memory";
    for (uint32_t i = 0; i < frames; ++i) {
        float sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            const uint8_t *p = data + ((size_t)i * channels + ch) * bytes;
            float v;
            switch (bits) {
            case 8: v = (p[0] - 128) / 128.0f; break;
            case 16: v = (int16_t)(p[0] | p[1] << 8) / 32768.0f; break;
            case 24: v = (int32_t)((uint32_t)p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f; break;
            default: {
                uint32_t u = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
                if (format == 3)
                    memcpy(&v, &u, 4);
                else
                    v = (int32_t)u / 2147483648.0f;
            }
            }
            sum += v;
        }
        float v = sum / channels * 32767;
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    d[frames] = frames ? d[frames - 1] : 0;
    xm_free(sm->data);
    sm->data = d;
    sm->length = frames;
    sm->bits16 = true;
    sm->loop = 0;
    sm->loop_start = sm->loop_len = 0;
    if (loop && lb > la && la < frames) {
        sm->loop = 1;
        sm->loop_start = la;
        sm->loop_len = (lb < frames ? lb : frames) - la;
    }
    tune_to_rate(sm, rate);
    return NULL;
}

/// Loads a WAV into the instrument as a new sample.
static void load_sample(void) {
    xm_instrument_t *in = instrument(true);
    if (!in)
        return;
    char path[256];
    trk_dialog_begin();
    bool ok = file_pick("Load a sample into the instrument", ".WAV.", false, path, sizeof path);
    trk_dialog_end();
    if (!ok)
        return;
    uint32_t size;
    uint8_t *f = file_read_all(path, &size, 8u << 20);
    if (!f)
        return;
    undo_keep_xm_instrument(T->sample);
    xm_stop();
    if (I.smp >= in->nsamples) {
        // A new sample at the end of the instrument's.
        if (in->nsamples >= 16) {
            psram_free(f);
            trk_message("An instrument holds 16 samples at most.");
            return;
        }
        xm_sample_t *list = xm_alloc((in->nsamples + 1) * sizeof *list);
        if (!list) {
            psram_free(f);
            trk_message("Not enough memory.");
            return;
        }
        memcpy(list, in->samples, in->nsamples * sizeof *list);
        memset(&list[in->nsamples], 0, sizeof *list);
        list[in->nsamples].volume = 64;
        list[in->nsamples].pan = 128;
        xm_free(in->samples);
        in->samples = list;
        I.smp = in->nsamples++;
    }
    xm_sample_t *sm = &in->samples[I.smp];
    const char *err = load_wav16(f, size, sm);
    psram_free(f);
    if (err) {
        trk_message("%s: %s.", path, err);
        return;
    }
    const char *base = strrchr(path, '/');
    memset(sm->name, 0, sizeof sm->name);
    memcpy(sm->name, base ? base + 1 : path, strnlen(base ? base + 1 : path, 22));
    if (!in->name[0])
        memcpy(in->name, sm->name, 22);
    T->dirty = true;
    trk_message("Loaded %lu samples (16-bit) into sample %d.", (unsigned long)sm->length, I.smp);
}

/// Deletes the sample chosen.
static void delete_sample(void) {
    xm_instrument_t *in = instrument(false);
    if (!in || I.smp >= in->nsamples)
        return;
    undo_keep_xm_instrument(T->sample);
    xm_stop();
    xm_free(in->samples[I.smp].data);
    memmove(&in->samples[I.smp], &in->samples[I.smp + 1], (in->nsamples - I.smp - 1) * sizeof *in->samples);
    in->nsamples--;
    for (int k = 0; k < 96; ++k)
        if (in->keymap[k] > I.smp)
            in->keymap[k]--;
        else if (in->keymap[k] == I.smp)
            in->keymap[k] = 0;
    if (I.smp > 0 && I.smp >= in->nsamples)
        I.smp--;
    T->dirty = true;
}

// ---- The tools (the Instrument menu) -------------------------------------------------------------

/// The selection, or all of the sample.
/// @param sm the sample
/// @param a set to the first
/// @param b to the one after the last
static void range(const xm_sample_t *sm, uint32_t *a, uint32_t *b) {
    if (I.sel1 > I.sel0 && I.sel1 <= sm->length) {
        *a = I.sel0;
        *b = I.sel1;
    } else {
        *a = 0;
        *b = sm->length;
    }
}

/// The sample chosen, if it has sound (else a message).
/// @return it, or NULL
static xm_sample_t *have_sample(void) {
    xm_sample_t *sm = sample();
    if (sm && sm->length && sm->data)
        return sm;
    trk_message("Instrument %02X has no sample %X.", T->sample, I.smp);
    return NULL;
}

/// Whether part of a sample is selected (else a message).
/// @param sm the sample
/// @return it is
static bool have_selection(const xm_sample_t *sm) {
    if (I.sel1 > I.sel0 && I.sel1 <= sm->length)
        return true;
    trk_message("Select part of the sample first (drag across the waveform).");
    return false;
}

/// A value clamped to 16 bits.
/// @param v the value
/// @return it
static int clamp16(int v) {
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/// Sound for a sample: one more than the length (the mixer reads past the end).
/// @param len the length
/// @return the sound, or NULL
static int16_t *new16(uint32_t len) {
    int16_t *d = xm_alloc((len + 1) * sizeof *d);
    if (d)
        memset(d, 0, (len + 1) * sizeof *d);
    else
        trk_message("Not enough memory.");
    return d;
}

/// New sound in a sample (the old freed; the player stopped first).
/// @param sm the sample
/// @param d the sound
/// @param len its length
static void set_data(xm_sample_t *sm, int16_t *d, uint32_t len) {
    xm_stop();
    xm_free(sm->data);
    sm->data = d;
    sm->length = d ? len : 0;
    if (d)
        d[len] = len ? d[len - 1] : 0;
    sm->bits16 = true;
    fix_loop(sm);
    if (I.sel1 > sm->length)
        I.sel0 = I.sel1 = 0;
    T->dirty = true;
}

/// A new sample slot at the end of an instrument's; NULL if full.
/// @param in the instrument
/// @return it, or NULL
static xm_sample_t *add_sample(xm_instrument_t *in) {
    if (in->nsamples >= 16) {
        trk_message("An instrument holds 16 samples at most.");
        return NULL;
    }
    xm_sample_t *list = xm_alloc((in->nsamples + 1) * sizeof *list);
    if (!list) {
        trk_message("Not enough memory.");
        return NULL;
    }
    xm_stop();
    memcpy(list, in->samples, in->nsamples * sizeof *list);
    memset(&list[in->nsamples], 0, sizeof *list);
    list[in->nsamples].volume = 64;
    list[in->nsamples].pan = 128;
    list[in->nsamples].bits16 = true;
    xm_free(in->samples);
    in->samples = list;
    return &list[in->nsamples++];
}

/// Amplifies the selection (or all).
/// @param sm the sample
/// @param by the factor
static void amplify(xm_sample_t *sm, double by) {
    uint32_t a, b;
    range(sm, &a, &b);
    for (uint32_t i = a; i < b; ++i)
        sm->data[i] = clamp16((int)lrint(sm->data[i] * by));
    sm->data[sm->length] = sm->data[sm->length - 1];
}

/// Generate: a sine, square, saw, triangle or noise sample.
static void generate(void) {
    static char *const kinds[] = {"Sine", "Square", "Saw", "Triangle", "Noise"};
    int k = gui_list("Generate a sample", kinds, 5, 0);
    if (k < 0)
        return;
    char buf[12];
    strcpy(buf, k == 4 ? "16384" : "128");
    if (!gui_input("Generate a sample",
                   k == 4 ? "Length in samples:" : "One cycle, in samples (it plays C-4 at C-4):", buf,
                   sizeof buf))
        return;
    uint32_t len = strtoul(buf, NULL, 10);
    if (len < 2 || len > (1u << 20)) {
        trk_message("The length must be 2 to %u.", 1u << 20);
        return;
    }
    xm_instrument_t *in = instrument(true);
    if (!in)
        return;
    int16_t *d = new16(len);
    if (!d)
        return;
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < len; ++i) {
        double x = (double)i / len, v;
        switch (k) {
        case 0: v = sin(x * 6.283185307); break;
        case 1: v = x < 0.5 ? 1 : -1; break;
        case 2: v = 2 * x - 1; break;
        case 3: v = x < 0.5 ? 4 * x - 1 : 3 - 4 * x; break;
        default: seed = seed * 1103515245 + 12345; v = (int)(seed >> 8 & 0xffff) / 32768.0 - 1; break;
        }
        d[i] = clamp16((int)lrint(v * 24000));
    }
    undo_keep_xm_instrument(T->sample);
    xm_sample_t *sm = sample();
    if (!sm && !(sm = add_sample(in))) {
        xm_free(d);
        return;
    }
    I.smp = sm - in->samples;
    set_data(sm, d, len);
    if (!sm->name[0])
        snprintf(sm->name, sizeof sm->name, "%s", kinds[k]);
    if (!in->name[0])
        snprintf(in->name, sizeof in->name, "%s", kinds[k]);
    if (!sm->volume)
        sm->volume = 64;
    sm->loop = k == 4 ? 0 : 1;
    sm->loop_start = 0;
    sm->loop_len = k == 4 ? 0 : len;
    // In tune: one cycle of `len` samples at C-4 is middle C (261.63 Hz).
    if (k != 4)
        tune_to_rate(sm, 261.6256 * len);
    else
        sm->relnote = sm->finetune = 0;
    fix_loop(sm);
}

/// Cuts the selection, or crops to it.
/// @param sm the sample
/// @param crop keep only the selection
static void cut_or_crop(xm_sample_t *sm, bool crop) {
    uint32_t a = I.sel0, b = I.sel1;
    uint32_t len = crop ? b - a : sm->length - (b - a);
    if (len < 2) {
        trk_message("That would leave nothing.");
        return;
    }
    int16_t *d = new16(len);
    if (!d)
        return;
    if (crop) {
        memcpy(d, sm->data + a, len * sizeof *d);
    } else {
        memcpy(d, sm->data, a * sizeof *d);
        memcpy(d + a, sm->data + b, (sm->length - b) * sizeof *d);
    }
    uint32_t ls = sm->loop_start;
    undo_keep_xm_instrument(T->sample);
    // The loop moves with what it was on, if it can.
    if (crop)
        ls = ls > a ? ls - a : 0;
    else if (ls >= b)
        ls -= b - a;
    sm->loop_start = ls;
    set_data(sm, d, len);
    I.sel0 = I.sel1 = 0;
}

/// Halves or doubles the sample's rate.
/// @param sm the sample
/// @param halve halve (else double)
static void resample(xm_sample_t *sm, bool halve) {
    uint32_t len = halve ? sm->length / 2 : sm->length * 2;
    if (len < 2 || len > (4u << 20)) {
        trk_message(halve ? "Too short to halve." : "Too long to double.");
        return;
    }
    int16_t *d = new16(len);
    if (!d)
        return;
    for (uint32_t i = 0; i < len; ++i) {
        if (halve) {
            d[i] = (sm->data[2 * i] + sm->data[2 * i + 1]) / 2;
        } else {
            int a = sm->data[i / 2], b = i / 2 + 1 < sm->length ? sm->data[i / 2 + 1] : a;
            d[i] = i & 1 ? (a + b) / 2 : a;
        }
    }
    undo_keep_xm_instrument(T->sample);
    sm->loop_start = halve ? sm->loop_start / 2 : sm->loop_start * 2;
    sm->loop_len = halve ? sm->loop_len / 2 : sm->loop_len * 2;
    set_data(sm, d, len);
    // The relative note follows, so it sounds the same.
    sm->relnote = clampi(sm->relnote + (halve ? -12 : 12), -96, 95);
    trk_message("%s; the relative note moved an octave so it sounds the same.",
                halve ? "Halved" : "Doubled");
}

/// Asks for an instrument number (1-128); 0 if cancelled.
/// @param title the dialog's title
/// @param prompt its prompt
/// @return the instrument, or 0
static int ask_instrument(const char *title, const char *prompt) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", T->sample < 128 ? T->sample + 1 : 1);
    if (!gui_input(title, prompt, buf, sizeof buf))
        return 0;
    int n = atoi(buf);
    if (n < 1 || n > 128 || n == T->sample) {
        trk_message("An instrument number 1-128, not this one.");
        return 0;
    }
    return n;
}

/// Copies a sample with its sound.
/// @param to the copy
/// @param from the sample
/// @return false when out of memory
static bool copy_sound(xm_sample_t *to, const xm_sample_t *from) {
    *to = *from;
    to->data = NULL;
    if (!from->data)
        return true;
    to->data = xm_alloc((from->length + 1) * sizeof(int16_t));
    if (!to->data)
        return false;
    memcpy(to->data, from->data, (from->length + 1) * sizeof(int16_t));
    return true;
}

/// Copies the sample into another instrument.
static void copy_sample_to(void) {
    xm_sample_t *sm = have_sample();
    if (!sm)
        return;
    int n = ask_instrument("Copy the sample", "Into instrument number (1-128), as a new sample:");
    if (!n)
        return;
    int here = T->sample, smp = I.smp;
    T->sample = n;
    xm_instrument_t *to = instrument(true);
    T->sample = here;
    if (!to)
        return;
    undo_keep_xm_instrument(n);
    xm_instrument_t *in = instrument(false);
    xm_sample_t copy;
    if (!copy_sound(&copy, &in->samples[smp])) {
        trk_message("Not enough memory.");
        return;
    }
    xm_sample_t *slot = add_sample(to);
    if (!slot) {
        xm_free(copy.data);
        return;
    }
    *slot = copy;
    if (!to->name[0])
        memcpy(to->name, copy.name, 22);
    T->dirty = true;
    trk_message("Copied into instrument %02X as its sample %X.", n, (int)(slot - to->samples));
}

/// Frees an instrument's samples.
/// @param in the instrument
static void free_samples(xm_instrument_t *in) {
    for (int i = 0; i < in->nsamples; ++i)
        xm_free(in->samples[i].data);
    xm_free(in->samples);
    in->samples = NULL;
    in->nsamples = 0;
}

/// The whole instrument (settings and samples) into another number.
static void copy_instrument(void) {
    if (!instrument(false)) {
        trk_message("Instrument %02X is empty.", T->sample);
        return;
    }
    int n = ask_instrument("Copy the instrument", "Copy to instrument number (1-128):");
    if (!n)
        return;
    int here = T->sample;
    T->sample = n;
    xm_instrument_t *to = instrument(true);
    T->sample = here;
    if (!to)
        return;
    undo_keep_xm_instrument(n);
    xm_instrument_t *in = instrument(false);
    xm_sample_t *list = in->nsamples ? xm_alloc(in->nsamples * sizeof *list) : NULL;
    bool ok = !in->nsamples || list;
    for (int i = 0; ok && i < in->nsamples; ++i)
        if (!copy_sound(&list[i], &in->samples[i])) {
            for (int k = 0; k < i; ++k)
                xm_free(list[k].data);
            ok = false;
        }
    if (!ok) {
        xm_free(list);
        trk_message("Not enough memory.");
        return;
    }
    xm_stop();
    free_samples(to);
    *to = *in;
    to->samples = list;
    T->dirty = true;
    trk_message("Instrument %02X copied to %02X.", T->sample, n);
}

/// Swaps the instrument with another.
static void swap_instrument(void) {
    int n = ask_instrument("Swap instruments", "Swap with instrument number (1-128):");
    if (!n)
        return;
    int here = T->sample;
    T->sample = n > here ? n : here;
    bool made = instrument(true) != NULL;  // both exist
    T->sample = here;
    if (!made)
        return;
    undo_keep_xm_instrument(n);
    undo_keep_xm_instrument(here);
    xm_stop();
    xm_instrument_t *a = &S()->instruments[here - 1], *b = &S()->instruments[n - 1], t = *a;
    *a = *b;
    *b = t;
    T->dirty = true;
    trk_message("Instruments %02X and %02X swapped (the patterns still name them by number).", here, n);
}

/// Clears the instrument (after asking).
static void clear_instrument(void) {
    xm_instrument_t *in = instrument(false);
    if (!in)
        return;
    char text[64];
    snprintf(text, sizeof text, "Clear instrument %02X (its settings and samples)?", T->sample);
    if (gui_message("Clear instrument", text, "Clear|Cancel", false) != 0)
        return;
    undo_keep_xm_instrument(T->sample);
    xm_stop();
    free_samples(in);
    uint32_t hs = in->header_size, ss = in->sample_header_size;
    memset(in, 0, sizeof *in);
    in->header_size = hs;
    in->sample_header_size = ss;
    I.smp = 0;
    T->dirty = true;
}

/// Envelope presets: piano, organ, pad, pluck and the like.
static void envelope_preset(void) {
    static char *const names[] = {
        "Volume: piano (struck, then dies away)", "Volume: organ (held while the key is)",
        "Volume: pad (slow rise, slow release)",  "Volume: pluck (short)",
        "Volume: tremolo loop",                   "Volume: off",
        "Panning: left to right",                 "Panning: autopan loop",
        "Panning: off",
    };
    static const struct {
        ///@{
        uint8_t count, sus, ls, le, flags;
        ///@}
        xm_point_t p[6];
        uint16_t fadeout;
    } presets[] = {
        {4, 0, 0, 0, 1, {{0, 64}, {8, 48}, {64, 20}, {200, 0}}, 512},
        {3, 1, 0, 0, 3, {{0, 64}, {4, 64}, {12, 0}}, 1024},
        {4, 1, 0, 0, 3, {{0, 0}, {48, 64}, {80, 0}, {81, 0}}, 64},
        {3, 0, 0, 0, 1, {{0, 64}, {6, 24}, {24, 0}}, 2048},
        {3, 0, 0, 2, 5, {{0, 64}, {8, 24}, {16, 64}}, 256},
        {0, 0, 0, 0, 0, {{0, 0}}, 0},
        {2, 0, 0, 0, 1, {{0, 0}, {128, 64}}, 0},
        {3, 0, 0, 2, 5, {{0, 0}, {32, 64}, {64, 0}}, 0},
        {0, 0, 0, 0, 0, {{0, 0}}, 0},
    };
    int k = gui_list("Envelope preset", names, 9, 0);
    if (k < 0)
        return;
    xm_instrument_t *in = instrument(true);
    if (!in)
        return;
    bool pan = k >= 6;
    xm_env_t *e = pan ? &in->penv : &in->venv;
    if (presets[k].count) {
        memset(e, 0, sizeof *e);
        e->count = presets[k].count;
        memcpy(e->points, presets[k].p, e->count * sizeof *e->points);
        e->sustain = presets[k].sus;
        e->loop_start = presets[k].ls;
        e->loop_end = presets[k].le;
        e->flags = presets[k].flags;
        if (!pan)
            in->fadeout = presets[k].fadeout;
    } else {
        e->flags = 0;
    }
    T->dirty = true;
}

/// Loads an .XI instrument.
static void load_xi(void) {
    char path[256];
    trk_dialog_begin();
    bool ok = file_pick("Load an instrument (.XI)", ".XI.", false, path, sizeof path);
    trk_dialog_end();
    if (!ok)
        return;
    uint32_t size;
    uint8_t *f = file_read_all(path, &size, 8u << 20);
    if (!f)
        return;
    xm_instrument_t got;
    const char *err = xm_xi_load(&got, f, size);
    psram_free(f);
    if (err) {
        free_samples(&got);
        trk_message("%s: %s.", path, err);
        return;
    }
    xm_instrument_t *in = instrument(true);
    if (!in) {
        free_samples(&got);
        return;
    }
    undo_keep_xm_instrument(T->sample);
    xm_stop();
    free_samples(in);
    *in = got;
    I.smp = 0;
    T->dirty = true;
    trk_message("Loaded %s: %d samples.", path, got.nsamples);
}

/// Saves the instrument as .XI.
static void save_xi(void) {
    xm_instrument_t *in = instrument(false);
    if (!in || !in->nsamples) {
        trk_message("Instrument %02X has no samples to save.", T->sample);
        return;
    }
    char path[256];
    trk_dialog_begin();
    bool ok = file_pick("Save the instrument as .XI", ".XI.", true, path, sizeof path);
    trk_dialog_end();
    if (!ok)
        return;
    size_t n = xm_xi_size(in);
    uint8_t *f = psram_malloc(n);
    if (!f) {
        trk_message("Not enough memory.");
        return;
    }
    xm_xi_save(in, f);
    if (file_write_all(path, f, n))
        trk_message("Saved %s (%lu bytes).", path, (unsigned long)n);
    psram_free(f);
}

/// Stores 32 bits little-endian.
/// @param p where
/// @param v the value
static void put32le(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/// The sample as a 16-bit mono WAV, at the rate C-4 plays it, with its loop.
static void save_wav16(void) {
    xm_sample_t *sm = have_sample();
    if (!sm)
        return;
    char path[256];
    trk_dialog_begin();
    bool ok = file_pick("Save the sample as WAV (16-bit)", ".WAV.", true, path, sizeof path);
    trk_dialog_end();
    if (!ok)
        return;
    bool loop = sm->loop && sm->loop_len;
    uint32_t rate = (uint32_t)lround(rate_of(sm)), smpl = loop ? 8 + 60 : 0;
    uint32_t len = sm->length * 2, size = 44 + len + smpl;
    uint8_t *f = psram_malloc(size);
    if (!f) {
        trk_message("Not enough memory.");
        return;
    }
    memset(f, 0, size);
    memcpy(f, "RIFF", 4);
    put32le(f + 4, size - 8);
    memcpy(f + 8, "WAVEfmt ", 8);
    put32le(f + 16, 16);
    f[20] = 1;  // PCM
    f[22] = 1;  // mono
    put32le(f + 24, rate);
    put32le(f + 28, rate * 2);
    f[32] = 2;  // block align
    f[34] = 16;
    uint32_t off = 36;
    if (loop) {
        memcpy(f + off, "smpl", 4);
        put32le(f + off + 4, 60);
        uint8_t *c = f + off + 8;
        put32le(c + 8, (uint32_t)(1e9 / rate));  // sample period, ns
        put32le(c + 12, 60);                     // MIDI unity note: middle C (FT2's C-4)
        put32le(c + 28, 1);                      // one loop
        put32le(c + 36 + 4, sm->loop == 2 ? 1 : 0);  // forward, or alternating
        put32le(c + 36 + 8, sm->loop_start);
        put32le(c + 36 + 12, sm->loop_start + sm->loop_len - 1);
        off += 8 + 60;
    }
    memcpy(f + off, "data", 4);
    put32le(f + off + 4, len);
    for (uint32_t i = 0; i < sm->length; ++i) {
        f[off + 8 + i * 2] = (uint8_t)sm->data[i];
        f[off + 9 + i * 2] = (uint8_t)(sm->data[i] >> 8);
    }
    if (file_write_all(path, f, size))
        trk_message("Saved %s (%lu samples, %lu Hz%s).", path, (unsigned long)sm->length,
                    (unsigned long)rate, loop ? ", with its loop" : "");
    psram_free(f);
}

void xmi_tool(int tool) {
    xm_sample_t *sm;
    uint32_t a, b;
    trk_dialog_begin();
    switch (tool) {
    case TOOL_LOAD: load_sample(); break;
    case TOOL_SAVE: save_wav16(); break;
    case TOOL_NAME: field_enter(F_SNAME); break;
    case TOOL_COPY_TO: copy_sample_to(); break;
    case TOOL_CLEAR:
        if (have_sample() && gui_message("Delete sample", "Delete this sample from the instrument?",
                                         "Delete|Cancel", false) == 0)
            delete_sample();
        break;
    case TOOL_GENERATE: generate(); break;
    case TOOL_AMPLIFY: {
        if (!(sm = have_sample()))
            break;
        static char buf[8] = "150";
        if (gui_input("Amplify", "Percent (100 = as it is):", buf, sizeof buf) && atoi(buf) > 0) {
            undo_keep_xm_instrument(T->sample);
            amplify(sm, atoi(buf) / 100.0);
        }
        break;
    }
    case TOOL_NORMALIZE: {
        if (!(sm = have_sample()))
            break;
        range(sm, &a, &b);
        int peak = 1;
        for (uint32_t i = a; i < b; ++i)
            peak = abs(sm->data[i]) > peak ? abs(sm->data[i]) : peak;
        undo_keep_xm_instrument(T->sample);
        amplify(sm, 32767.0 / peak);
        break;
    }
    case TOOL_REVERSE:
        if (!(sm = have_sample()))
            break;
        undo_keep_xm_instrument(T->sample);
        range(sm, &a, &b);
        for (uint32_t i = a, j = b - 1; i < j; ++i, --j) {
            int16_t t = sm->data[i];
            sm->data[i] = sm->data[j];
            sm->data[j] = t;
        }
        sm->data[sm->length] = sm->data[sm->length - 1];
        break;
    case TOOL_FADE_IN:
    case TOOL_FADE_OUT:
        if (!(sm = have_sample()))
            break;
        undo_keep_xm_instrument(T->sample);
        range(sm, &a, &b);
        for (uint32_t i = a; i < b; ++i) {
            uint32_t k = tool == TOOL_FADE_IN ? i - a : b - 1 - i;
            sm->data[i] = (int16_t)((int64_t)sm->data[i] * k / (b - a));
        }
        sm->data[sm->length] = sm->data[sm->length - 1];
        break;
    case TOOL_CENTRE: {
        if (!(sm = have_sample()))
            break;
        undo_keep_xm_instrument(T->sample);
        range(sm, &a, &b);
        int64_t sum = 0;
        for (uint32_t i = a; i < b; ++i)
            sum += sm->data[i];
        int dc = (int)(sum / (int64_t)(b - a));
        for (uint32_t i = a; i < b; ++i)
            sm->data[i] = clamp16(sm->data[i] - dc);
        sm->data[sm->length] = sm->data[sm->length - 1];
        trk_message("Moved by %d.", -dc);
        break;
    }
    case TOOL_SILENCE:
        if (!(sm = have_sample()) || !have_selection(sm))
            break;
        undo_keep_xm_instrument(T->sample);
        memset(sm->data + I.sel0, 0, (I.sel1 - I.sel0) * sizeof *sm->data);
        sm->data[sm->length] = sm->data[sm->length - 1];
        break;
    case TOOL_CROP:
    case TOOL_CUT:
        if ((sm = have_sample()) && have_selection(sm))
            cut_or_crop(sm, tool == TOOL_CROP);
        break;
    case TOOL_LOOP_SEL:
        if (!(sm = have_sample()) || !have_selection(sm))
            break;
        sm->loop_start = I.sel0;
        sm->loop_len = I.sel1 - I.sel0;
        if (!sm->loop)
            sm->loop = 1;
        fix_loop(sm);
        T->dirty = true;
        break;
    case TOOL_HALVE:
    case TOOL_DOUBLE:
        if ((sm = have_sample()))
            resample(sm, tool == TOOL_HALVE);
        break;
    case TOOL_SEL_ALL:
        if ((sm = have_sample()))
            I.sel0 = 0, I.sel1 = sm->length;
        break;
    case TOOL_ZOOM_SEL:
    case TOOL_ZOOM_ALL: break;  // the instrument screen shows all of the sample
    case TOOL_XI_LOAD: load_xi(); break;
    case TOOL_XI_SAVE: save_xi(); break;
    case TOOL_INS_COPY: copy_instrument(); break;
    case TOOL_INS_SWAP: swap_instrument(); break;
    case TOOL_INS_CLEAR: clear_instrument(); break;
    case TOOL_ENV_PRESET: envelope_preset(); break;
    }
    trk_dialog_end();
    T->screen = SCREEN_SAMPLE;
    T->full = true;
}

// ---- Drawing --------------------------------------------------------------------------------------

/// Draws the instrument list.
static void draw_list(void) {
    xm_song_t *s = S();
    if (T->full)
        gfx_bevel(0, TOP_Y, LIST_W, TOP_H, true, C_FACE);
    if (T->sample < I.ins_top + 1)
        I.ins_top = T->sample - 1;
    if (T->sample > I.ins_top + ROWS)
        I.ins_top = T->sample - ROWS;
    for (int i = 0; i < ROWS; ++i) {
        int n = I.ins_top + 1 + i, y = TOP_Y + 6 + i * 16;
        const xm_instrument_t *in = n <= s->ninstruments ? &s->instruments[n - 1] : NULL;
        bool on = n == T->sample;
        uint8_t bg = on ? C_CURROW : C_FIELD;
        char name[23] = "";
        for (int k = 0; in && k < 22 && in->name[k]; ++k)
            name[k] = in->name[k] < 0x20 || in->name[k] > 0x7e ? '.' : in->name[k];
        gfx_printf(4, y, 3, C_YELLOW, bg, "%02X", n);
        gfx_text(28, y, name, 19, in && in->nsamples ? C_TEXT : C_DIM, bg);
        hit_add(4, y, LIST_W - 8, 16, IH_INSTR, n);
    }
}

/// A setting's line.
/// @param f the setting
/// @param y its row
/// @param value its value
static void param_line(int f, int y, const char *value) {
    bool focus = I.field == f;
    gfx_text(PARAM_X + 6, y, field_names[f], 10, focus ? C_YELLOW : C_LABEL, C_FACE);
    uint8_t bg = focus ? C_FOCUS : C_FIELD;
    gfx_fill(PARAM_X + 86, y, 2, 16, bg);
    int end = gfx_text(PARAM_X + 88, y, value, f == F_NAME || f == F_SNAME ? 16 : 8, C_TEXT, bg);
    gfx_fill(end, y, 2, 16, bg);
    hit_add(PARAM_X + 86, y, end + 2 - PARAM_X - 86, 16, IH_FIELD, f);
    if (f != F_NAME && f != F_SNAME) {
        gfx_button(PARAM_X + 160, y, 18, 16, "-", IH_DEC, f, false);
        gfx_button(PARAM_X + 180, y, 18, 16, "+", IH_INC, f, false);
    }
}

/// Draws the settings.
static void draw_params(void) {
    static const char *const vib[4] = {"sine", "square", "ramp dn", "ramp up"};
    static const char *const loops[3] = {"off", "forward", "ping-pong"};
    if (T->full)
        gfx_bevel(PARAM_X, TOP_Y, PARAM_W, TOP_H, true, C_FACE);
    xm_instrument_t *in = instrument(false);
    xm_sample_t *sm = sample();
    char v[32];
    // Two pages of ten in the panel: the instrument's, or the sample's.
    bool smp_page = I.field >= F_SNAME && I.field < F_VENV;
    int first = smp_page ? F_SNAME : F_NAME, last = smp_page ? F_LLEN : F_VRATE;
    for (int f = first, i = 0; f <= last; ++f, ++i) {
        switch (f) {
        case F_NAME: snprintf(v, sizeof v, "%.22s", in ? in->name : ""); break;
        case F_FADEOUT: snprintf(v, sizeof v, "%d", in ? in->fadeout : 0); break;
        case F_VTYPE: snprintf(v, sizeof v, "%s", vib[in ? in->vib_type & 3 : 0]); break;
        case F_VSWEEP: snprintf(v, sizeof v, "%d", in ? in->vib_sweep : 0); break;
        case F_VDEPTH: snprintf(v, sizeof v, "%d", in ? in->vib_depth : 0); break;
        case F_VRATE: snprintf(v, sizeof v, "%d", in ? in->vib_rate : 0); break;
        case F_SNAME: snprintf(v, sizeof v, "%.22s", sm ? sm->name : "(none)"); break;
        case F_VOLUME: snprintf(v, sizeof v, "%d", sm ? sm->volume : 0); break;
        case F_PAN: snprintf(v, sizeof v, "%d", sm ? sm->pan : 0); break;
        case F_FINE: snprintf(v, sizeof v, "%d", sm ? sm->finetune : 0); break;
        case F_REL: snprintf(v, sizeof v, "%+d", sm ? sm->relnote : 0); break;
        case F_LOOP: snprintf(v, sizeof v, "%s", loops[sm ? sm->loop % 3 : 0]); break;
        case F_LSTART: snprintf(v, sizeof v, "%lu", sm ? (unsigned long)sm->loop_start : 0ul); break;
        case F_LLEN: snprintf(v, sizeof v, "%lu", sm ? (unsigned long)sm->loop_len : 0ul); break;
        }
        param_line(f, TOP_Y + 6 + i * 16, v);
    }
    for (int i = last - first + 1; i < ROWS; ++i)
        gfx_fill(PARAM_X + 4, TOP_Y + 6 + i * 16, PARAM_W - 8, 16, C_FACE);
    gfx_text(PARAM_X + 6, TOP_Y + 6 + 8 * 16, smp_page ? "Up/Down: settings" : "Down: the sample's", 30, C_DIM, C_FACE);
    gfx_printf(PARAM_X + 6, TOP_Y + 6 + 9 * 16, 30, C_DIM, C_FACE, "%s", sm ? (sm->bits16 ? "16-bit" : "8-bit") : "");
}

/// Draws the instrument's samples.
static void draw_samples(void) {
    if (T->full)
        gfx_bevel(SMP_X, TOP_Y, SMP_W, TOP_H, true, C_FACE);
    xm_instrument_t *in = instrument(false);
    int n = in ? in->nsamples : 0;
    for (int i = 0; i < 8; ++i) {
        int y = TOP_Y + 6 + i * 16;
        bool on = i == I.smp;
        uint8_t bg = on ? C_CURROW : C_FIELD;
        char name[23] = "";
        if (i < n)
            for (int k = 0; k < 22 && in->samples[i].name[k]; ++k)
                name[k] = in->samples[i].name[k] < 0x20 || in->samples[i].name[k] > 0x7e ? '.' : in->samples[i].name[k];
        gfx_printf(SMP_X + 4, y, 2, C_YELLOW, bg, "%X", i);
        gfx_text(SMP_X + 20, y, i < n ? name : on ? "(new: Load)" : "", 21, i < n ? C_TEXT : C_DIM, bg);
        hit_add(SMP_X + 4, y, SMP_W - 8, 16, IH_SAMPLE, i);
    }
    int y = TOP_Y + 6 + 8 * 16 + 2;
    gfx_button(SMP_X + 4, y, 60, 18, "Load", IH_TOOL, IT_LOAD, false);
    gfx_button(SMP_X + 66, y, 60, 18, "Delete", IH_TOOL, IT_DEL, false);
    gfx_button(SMP_X + 128, y, 60, 18, "Patt F2", IH_TOOL, IT_PATTERNS, false);
}

/// Draws the waveform, its loop and the selection.
static void draw_wave(void) {
    xm_sample_t *sm = sample();
    int top = WAVE_Y + 1, h = WAVE_H - 2, mid = top + h / 2;
    if (T->full)
        gfx_bevel(0, WAVE_Y, DVI_WIDTH, WAVE_H, false, C_BG);
    int w = DVI_WIDTH - 2;
    uint32_t len = sm ? sm->length : 0;
    int lx0 = -1, lx1 = -1, sx0 = -1, sx1 = -1;
    uint32_t s0 = I.sel0 < I.sel1 ? I.sel0 : I.sel1, s1 = I.sel0 < I.sel1 ? I.sel1 : I.sel0;
    if (len && s1 > s0 && s1 <= len) {
        sx0 = (int)((uint64_t)s0 * w / len);
        sx1 = (int)((uint64_t)s1 * w / len);
        sx1 = sx1 > sx0 ? sx1 : sx0 + 1;
    }
    if (sm && sm->loop && len) {
        lx0 = (int)((uint64_t)sm->loop_start * w / len);
        lx1 = (int)((uint64_t)(sm->loop_start + sm->loop_len) * w / len);
    }
    int prev_lo = 0, prev_hi = 0;
    for (int x = 0; x < w; ++x) {
        int lo = 0, hi = 0;
        if (len) {
            uint32_t a = (uint64_t)x * len / w, b = (uint64_t)(x + 1) * len / w;
            if (b <= a)
                b = a + 1;
            lo = 32767, hi = -32768;
            // Every sample in long ones would be slow: up to 64 a column.
            uint32_t step = (b - a) / 64 + 1;
            for (uint32_t i = a; i < b && i < len; i += step) {
                int v = sm->data[i];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
            if (x) {
                hi = hi > prev_lo ? hi : prev_lo;
                lo = lo < prev_hi ? lo : prev_hi;
            }
            prev_lo = lo;
            prev_hi = hi;
        }
        int y0 = mid - hi * (h / 2 - 2) / 32768, y1 = mid - lo * (h / 2 - 2) / 32768;
        bool in_loop = lx0 >= 0 && x >= lx0 && x < lx1, in_sel = x >= sx0 && x < sx1;
        uint8_t *d = dvi_framebuf + top * DVI_WIDTH + 1 + x;
        for (int y = top; y < top + h; ++y, d += DVI_WIDTH) {
            if (x == lx0 || x == lx1)
                *d = C_ORANGE;
            else if (len && y >= y0 && y <= y1)
                *d = in_sel ? C_YELLOW : C_GREEN;
            else
                *d = in_sel ? C_BLOCK : y == mid ? C_DARK : in_loop ? C_ROW4 : C_BG;
        }
    }
    if (!len)
        gfx_text(DVI_WIDTH / 2 - 60, mid - 8, "(no sample)", 11, C_DIM, C_BG);
    hit_add(1, top, w, h, IH_WAVE, 0);
}

/// Draws an envelope.
/// @param which 0 volume, 1 panning
static void draw_env(int which) {
    xm_instrument_t *in = instrument(false);
    xm_env_t *e = in ? (which ? &in->penv : &in->venv) : NULL;
    int x0 = which * ENV_W, top = ENV_Y + 1, h = ENV_H - 2, w = ENV_W - 2;
    if (T->full)
        gfx_bevel(x0, ENV_Y, ENV_W, ENV_H, false, C_BG);
    // Ticks across: enough for the last point, at least 128.
    int span = 128;
    if (e && e->count)
        span = e->points[e->count - 1].x + 16 > span ? e->points[e->count - 1].x + 16 : span;
    bool on = e && (e->flags & 1);
    int prev_vy = -1;
    for (int x = 0; x < w; ++x) {
        int tick = x * span / w, v = -1;
        if (e && e->count) {
            v = e->points[0].y;
            for (int i = 0; i + 1 < e->count; ++i)
                if (tick >= e->points[i].x && tick < e->points[i + 1].x) {
                    int dx = e->points[i + 1].x - e->points[i].x;
                    v = dx ? e->points[i].y + (e->points[i + 1].y - e->points[i].y) * (tick - e->points[i].x) / dx : e->points[i + 1].y;
                }
            if (tick >= e->points[e->count - 1].x)
                v = e->points[e->count - 1].y;
        }
        int vy = v < 0 ? -1 : top + h - 1 - v * (h - 1) / 64;
        // Joined to the column before, so steep parts are lines, not dots.
        int lo = vy, hi = vy + 1;
        if (vy >= 0 && prev_vy >= 0) {
            lo = vy < prev_vy ? vy : prev_vy;
            hi = vy < prev_vy ? prev_vy : vy + 1;
        }
        prev_vy = vy;
        bool in_loop = e && (e->flags & 4) && e->loop_start < e->count && e->loop_end < e->count &&
                       tick >= e->points[e->loop_start].x && tick <= e->points[e->loop_end].x;
        bool sus = e && (e->flags & 2) && e->sustain < e->count && tick == e->points[e->sustain].x;
        uint8_t *d = dvi_framebuf + top * DVI_WIDTH + x0 + 1 + x;
        for (int y = top; y < top + h; ++y, d += DVI_WIDTH) {
            uint8_t c = in_loop ? C_ROW4 : C_BG;
            if (sus)
                c = C_ORANGE;
            if (which && y == top + h / 2)
                c = C_DARK;
            if (vy >= 0 && y >= lo && y <= hi)
                c = on ? C_GREEN : C_DIM;
            *d = c;
        }
    }
    // The points, as small squares.
    for (int i = 0; e && i < e->count; ++i) {
        int px = x0 + 1 + e->points[i].x * w / span, py = top + h - 1 - e->points[i].y * (h - 1) / 64;
        gfx_fill(px - 2, py - 2, 5, 5, on ? C_YELLOW : C_DIM);
    }
    gfx_text(x0 + 4, top, which ? "Panning" : "Volume", 8, on ? C_YELLOW : C_DIM, C_BG);
    hit_add(x0 + 1, top, w, h, IH_ENV, which);
    // Its settings below it.
    int base = which ? F_PENV : F_VENV;
    char v[64], sus[8] = "-", loop[12] = "-";
    if (e && (e->flags & 2))
        snprintf(sus, sizeof sus, "%d", e->sustain);
    if (e && (e->flags & 4))
        snprintf(loop, sizeof loop, "%d-%d", e->loop_start, e->loop_end);
    if (e)
        snprintf(v, sizeof v, "%s  sus %s  loop %s  %d pts", on ? "on " : "off", sus, loop, e->count);
    else
        strcpy(v, "(no instrument)");
    bool focus = I.field >= base && I.field < base + 4;
    gfx_printf(x0 + 4, ENVBAR_Y + 2, 39, focus ? C_YELLOW : C_LABEL, C_FACE, "%s%s", focus ? ">" : " ", v);
    hit_add(x0, ENVBAR_Y, ENV_W, 18, IH_FIELD, base);
}

/// Draws the keymap: which sample each key plays.
static void draw_keys(void) {
    xm_instrument_t *in = instrument(false);
    gfx_fill(0, KEYS_Y - 2, DVI_WIDTH, 22, C_FACE);
    gfx_text(0, KEYS_Y, "Map", 3, C_LABEL, C_FACE);
    static const uint8_t colours[16] = {C_GREEN, C_CYAN, C_YELLOW, C_ORANGE, C_RED, 0x9b, 0x1f, 0xe3,
                                        0x7c, 0x53, 0xf9, 0x2e, 0x8e, 0xd7, 0x6b, 0xb3};
    for (int k = 0; k < 96; ++k) {
        int x = KEYS_X + k * KEY_W, n = k % 12;
        bool black = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
        int smp = in ? in->keymap[k] : 0;
        uint8_t c = in && in->nsamples ? colours[smp & 15] : C_DIM;
        gfx_fill(x, KEYS_Y, KEY_W - 1, 16, black ? C_DARK : C_LIGHT);
        gfx_fill(x, KEYS_Y + (black ? 2 : 10), KEY_W - 1, 6, c);
    }
    hit_add(KEYS_X, KEYS_Y, 96 * KEY_W, 16, IH_KEY, 0);
    gfx_printf(KEYS_X + 96 * KEY_W + 4, KEYS_Y, 4, C_DIM, C_FACE, "C-0");
}

/// Draws the status line.
static void draw_status(void) {
    char line[81];
    if (T->msg[0] && to_ms_since_boot(get_absolute_time()) < T->msg_until)
        snprintf(line, sizeof line, " %s", T->msg);
    else if (I.sel1 > I.sel0)
        snprintf(line, sizeof line, " Selected %lu-%lu (%lu samples)   right button: the tools",
                 (unsigned long)I.sel0, (unsigned long)I.sel1, (unsigned long)(I.sel1 - I.sel0));
    else
        snprintf(line, sizeof line, " Instrument %02X sample %X  F2 patterns  piano keys play  right button: tools",
                 T->sample, I.smp);
    gfx_text(0, STATUS_Y, line, 80, T->msg[0] ? C_YELLOW : C_LABEL, C_FACE);
}

void xmi_draw(void) {
    draw_list();
    draw_params();
    draw_samples();
    draw_wave();
    draw_env(0);
    draw_env(1);
    draw_keys();
    draw_status();
}

// ---- Keys and the mouse ----------------------------------------------------------------------------

bool xmi_key(int k) {
    switch (k) {
    case KEY_UP: I.field = (I.field + F_COUNT - 1) % F_COUNT; T->full = true; return true;
    case KEY_DOWN: I.field = (I.field + 1) % F_COUNT; T->full = true; return true;
    case KEY_LEFT: field_step(I.field, -1); return true;
    case KEY_RIGHT: field_step(I.field, 1); return true;
    case KEY_PGDN: field_step(I.field, -16); return true;
    case KEY_PGUP: field_step(I.field, 16); return true;
    case '\r': field_enter(I.field); return true;
    case '-': if (T->sample > 1) T->sample--, I.smp = 0; return true;
    case '=': case '+': if (T->sample < 128) T->sample++, I.smp = 0; return true;
    case '[': if (I.smp > 0) I.smp--; return true;
    case ']': if (I.smp < 15) I.smp++; return true;
    case CTRL('l'): load_sample(); return true;
    case KEY_ALT + 'l': xmi_tool(TOOL_SEL_ALL); return true;
    case KEY_ALT + 'u': I.sel0 = I.sel1 = 0; return true;
    case KEY_ALT + 'x': xmi_tool(TOOL_CUT); return true;
    case KEY_ALT + 'k': xmi_tool(TOOL_CROP); return true;
    }
    // Piano keys play the instrument.
    static const char lower[] = "zsxdcvgbhnjm,l.;/";
    static const char upper[] = "q2w3er5t6y7ui9o0p";
    int c = k >= 'A' && k <= 'Z' ? k + 32 : k;
    const char *p;
    int n = -1;
    if (k >= 0x20 && k < 0x7f && (p = strchr(lower, c)))
        n = p - lower;
    else if (k >= 0x20 && k < 0x7f && (p = strchr(upper, c)))
        n = 12 + (p - upper);
    if (n >= 0 && n + 48 + 1 <= 96 && instrument(false)) {
        xm_jam(S(), 0, T->sample, n + 48 + 1);
        return true;
    }
    return false;
}

/// Drags an envelope point (right button: adds or removes one).
/// @param which 0 volume, 1 panning
/// @param x the x
/// @param y the y
/// @param start the drag starts
/// @param right the right button
static void env_drag(int which, int x, int y, bool start, bool right) {
    xm_instrument_t *in = instrument(true);
    if (!in)
        return;
    xm_env_t *e = which ? &in->penv : &in->venv;
    int x0 = which * ENV_W, top = ENV_Y + 1, h = ENV_H - 2, w = ENV_W - 2;
    int span = 128;
    if (e->count)
        span = e->points[e->count - 1].x + 16 > span ? e->points[e->count - 1].x + 16 : span;
    int tick = clampi((x - x0 - 1) * span / w, 0, 65535), val = clampi((top + h - 1 - y) * 64 / (h - 1), 0, 64);
    if (start) {
        // Near a point: that one (the right button deletes it); else a new point.
        int best = -1, bd = 7 * 7;
        for (int i = 0; i < e->count; ++i) {
            int px = x0 + 1 + e->points[i].x * w / span, py = top + h - 1 - e->points[i].y * (h - 1) / 64;
            int d = (px - x) * (px - x) + (py - y) * (py - y);
            if (d <= bd)
                bd = d, best = i;
        }
        if (right) {
            if (best > 0 && e->count > 1) {
                memmove(&e->points[best], &e->points[best + 1], (e->count - best - 1) * sizeof *e->points);
                e->count--;
                if (e->sustain >= e->count) e->sustain = e->count - 1;
                if (e->loop_end >= e->count) e->loop_end = e->count - 1;
                if (e->loop_start > e->loop_end) e->loop_start = e->loop_end;
                T->dirty = true;
            }
            I.drag_env = -1;
            return;
        }
        if (best < 0) {
            if (e->count >= 12) {
                trk_message("An envelope has 12 points at most.");
                I.drag_env = -1;
                return;
            }
            int at = 0;
            while (at < e->count && e->points[at].x < tick)
                ++at;
            if (at == 0 && e->count)
                at = 1;  // the first point stays at tick 0
            memmove(&e->points[at + 1], &e->points[at], (e->count - at) * sizeof *e->points);
            e->points[at] = (xm_point_t){(uint16_t)tick, (uint16_t)val};
            e->count++;
            e->flags |= 1;
            best = at;
        }
        I.drag_env = which;
        I.drag_point = best;
    }
    if (I.drag_env != which || I.drag_point < 0 || I.drag_point >= e->count)
        return;
    int i = I.drag_point;
    // Between its neighbours; the first point stays at tick 0.
    int lo = i ? e->points[i - 1].x + 1 : 0, hi = i + 1 < e->count ? e->points[i + 1].x - 1 : 65535;
    e->points[i].x = i ? clampi(tick, lo, hi) : 0;
    e->points[i].y = val;
    T->dirty = true;
}

bool xmi_click(const hit_t *h, const mouse_event_t *e) {
    if (e->type == MOUSE_UP) {
        I.drag_env = -1;
        I.drag_loop = 0;
        if (I.selecting) {
            I.selecting = false;
            if (I.sel1 < I.sel0) {
                uint32_t t = I.sel0;
                I.sel0 = I.sel1;
                I.sel1 = t;
            }
            if (I.sel1 == I.sel0)
                I.sel0 = I.sel1 = 0;  // a click, not a drag
        }
        return false;
    }
    if (e->type == MOUSE_MOVE) {
        if (!(e->buttons & MOUSE_LEFT))
            return false;
        if (I.drag_env >= 0)
            env_drag(I.drag_env, e->x, e->y, false, false);
        else if (I.selecting) {
            xm_sample_t *sm = sample();
            if (sm && sm->length)
                I.sel1 = (uint64_t)clampi(e->x - 1, 0, DVI_WIDTH - 2) * sm->length / (DVI_WIDTH - 2);
        } else if (I.drag_loop) {
            xm_sample_t *sm = sample();
            if (sm && sm->length) {
                uint32_t p = (uint64_t)clampi(e->x - 1, 0, DVI_WIDTH - 2) * sm->length / (DVI_WIDTH - 2);
                uint32_t end = sm->loop_start + sm->loop_len;
                if (I.drag_loop == 1 && p < end)
                    sm->loop_start = p, sm->loop_len = end - p;
                else if (I.drag_loop == 2 && p > sm->loop_start)
                    sm->loop_len = p - sm->loop_start;
                T->dirty = true;
            }
        } else if (h && h->id == IH_KEY) {
            goto keymap;
        }
        return true;
    }
    if (!h)
        return false;
    if (e->type == MOUSE_WHEEL) {
        int d = -e->wheel;
        if (h->id == IH_INSTR)
            T->sample = clampi(T->sample + d, 1, 128), I.smp = 0;
        else if (h->id == IH_FIELD || h->id == IH_DEC || h->id == IH_INC)
            field_step(h->arg, -d);
        else
            return false;
        return true;
    }
    if (e->type != MOUSE_DOWN)
        return false;
    switch (h->id) {
    case IH_INSTR: T->sample = h->arg; I.smp = 0; I.sel0 = I.sel1 = 0; T->full = true; return true;
    case IH_SAMPLE: I.smp = h->arg; I.sel0 = I.sel1 = 0; if (I.field < F_SNAME || I.field >= F_VENV) I.field = F_SNAME; T->full = true; return true;
    case IH_FIELD: I.field = h->arg; T->full = true; if (h->arg != F_VENV && h->arg != F_PENV) field_enter(h->arg); return true;
    case IH_DEC: I.field = h->arg; field_step(h->arg, -1); return true;
    case IH_INC: I.field = h->arg; field_step(h->arg, 1); return true;
    case IH_ENV: env_drag(h->arg, e->x, e->y, true, e->button == MOUSE_RIGHT); return true;
    case IH_WAVE: {
        // Near a loop marker: moves it; else drags out a selection.
        xm_sample_t *sm = sample();
        if (!sm || !sm->length || e->button != MOUSE_LEFT)
            return e->button == MOUSE_LEFT;
        int w = DVI_WIDTH - 2;
        if (sm->loop) {
            int lx0 = (int)((uint64_t)sm->loop_start * w / sm->length), lx1 = (int)((uint64_t)(sm->loop_start + sm->loop_len) * w / sm->length);
            I.drag_loop = abs(e->x - 1 - lx0) <= 3 ? 1 : abs(e->x - 1 - lx1) <= 3 ? 2 : 0;
            if (I.drag_loop)
                return true;
        }
        I.selecting = true;
        I.sel0 = I.sel1 = (uint64_t)clampi(e->x - 1, 0, w) * sm->length / w;
        return true;
    }
    case IH_KEY:
    keymap: {
        xm_instrument_t *in = instrument(true);
        int k = (e->x - KEYS_X) / KEY_W;
        if (in && k >= 0 && k < 96 && I.smp < in->nsamples) {
            in->keymap[k] = I.smp;
            T->dirty = true;
        }
        return true;
    }
    case IH_TOOL:
        switch (h->arg) {
        case IT_LOAD: load_sample(); break;
        case IT_DEL: delete_sample(); break;
        case IT_PATTERNS: T->screen = SCREEN_PATTERN; T->full = true; break;
        }
        return true;
    }
    return false;
}

void xmi_reset(void) {
    memset(&I, 0, sizeof I);
    I.drag_env = -1;
}
