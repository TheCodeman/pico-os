/**
 * @file trk_sample.c
 * @brief The tracker's sample screen: the sample list, the sample's settings, its waveform with the loop and the selection, and the tools.
 *
 * Also loading samples (WAV, IFF 8SVX, raw) and saving them as WAV.
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
#include <strings.h>

#include "dvi.h"
#include "mc.h"
#include "psram_heap.h"
#include "trk.h"

#define MAX_LEN 131070u  ///< 65535 words: the most a MOD sample can be

#define LIST_X  0  ///< the sample list: left
#define LIST_W  292  ///< width
#define PROP_X  292  ///< the settings: left
#define PROP_W  348  ///< width
#define TOP_Y   16  ///< the panels' top
#define TOP_H   144  ///< their height
#define WAVE_Y  160  ///< the waveform's top
#define WAVE_H  256  ///< its height
#define BAR_Y   416  ///< the view bar's row
#define TOOL_Y  432  ///< the tools' row
#define STATUS_Y 464  ///< the status line's top
#define LIST_ROWS 8  ///< sample rows shown

/// The sample chosen.
/// @return it
static mod_sample_t *cur(void) {
    return &T->song.samples[T->sample - 1];
}

void smp_fix_loop(mod_sample_t *m) {
    m->loop_start &= ~1u;
    m->loop_len &= ~1u;
    if (m->loop_start >= m->length)
        m->loop_start = 0;
    if (m->loop_start + m->loop_len > m->length)
        m->loop_len = m->length - m->loop_start;
    if (m->loop_len <= 2) {
        m->loop_start = 0;
        m->loop_len = 2;  // "no loop", as ProTracker writes it
    }
}

/// Shows the whole sample.
static void view_all(void) {
    T->view0 = 0;
    T->view_len = cur()->length;
    T->wave_dirty = true;
}

void smp_set_data(int n, int8_t *data, uint32_t length) {
    mod_sample_t *m = &T->song.samples[n - 1];
    mod_free(m->data);
    m->data = data;
    m->length = data ? length & ~1u : 0;
    smp_fix_loop(m);
    if (n == T->sample) {
        if (T->sel1 > m->length)
            T->sel0 = T->sel1 = 0;
        view_all();
    }
    T->dirty = true;
}

/// Sound for a sample (zeroed, two bytes more).
/// @param len its length
/// @return it, or NULL (with a message)
static int8_t *new_data(uint32_t len) {
    // Two bytes more: the player reads one past the end when it interpolates.
    int8_t *d = mod_alloc(len + 2);
    if (d)
        memset(d, 0, len + 2);
    else
        trk_message("Not enough memory.");
    return d;
}

// ---- The tools ---------------------------------------------------------------------------------------------

/// The selection, or all of the sample.
/// @param a set to the first
/// @param b to the one after the last
static void range(uint32_t *a, uint32_t *b) {
    mod_sample_t *m = cur();
    if (T->sel1 > T->sel0 && T->sel1 <= m->length) {
        *a = T->sel0;
        *b = T->sel1;
    } else {
        *a = 0;
        *b = m->length;
    }
}

/// Whether the sample has sound (else a message).
/// @return it has
static bool have_sample(void) {
    if (cur()->length && cur()->data)
        return true;
    trk_message("Sample %02d is empty.", T->sample);
    return false;
}

/// Whether part of it is selected (else a message).
/// @return it is
static bool have_selection(void) {
    if (T->sel1 > T->sel0)
        return true;
    trk_message("Select part of the sample first (drag on it, or the Sel fields).");
    return false;
}

/// A value clamped to 8 bits.
/// @param v the value
/// @return it
static int clamp8(int v) {
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

/// Amplifies the selection (or all).
/// @param percent by this much
static void amplify(int percent) {
    mod_sample_t *m = cur();
    uint32_t a, b;
    range(&a, &b);
    for (uint32_t i = a; i < b; ++i)
        m->data[i] = clamp8(m->data[i] * percent / 100);
}

/// Generate: a sine, square, saw, triangle or noise sample.
static void generate(void) {
    static char *const kinds[] = {"Sine", "Square", "Saw", "Triangle", "Noise"};
    int k = gui_list("Generate", kinds, 5, 0);
    if (k < 0)
        return;
    char buf[12] = "32";
    if (k == 4)
        strcpy(buf, "8192");
    if (!gui_input("Generate", k == 4 ? "Length in bytes:" : "One cycle, in bytes (32 = C-3 at C-3):",
                   buf, sizeof buf))
        return;
    uint32_t len = strtoul(buf, NULL, 10) & ~1u;
    if (len < 2 || len > MAX_LEN) {
        trk_message("The length must be 2 to %u.", MAX_LEN);
        return;
    }
    int8_t *d = new_data(len);
    if (!d)
        return;
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < len; ++i) {
        float x = (float)i / len;
        float v;
        switch (k) {
        case 0: v = sinf(x * 6.2831853f); break;
        case 1: v = x < 0.5f ? 1 : -1; break;
        case 2: v = 2 * x - 1; break;
        case 3: v = x < 0.5f ? 4 * x - 1 : 3 - 4 * x; break;
        default: seed = seed * 1103515245 + 12345; v = (int)(seed >> 16 & 255) / 128.0f - 1; break;
        }
        d[i] = clamp8((int)lrintf(v * 100));
    }
    trk_stop();
    undo_keep_mod_sample(T->sample);
    smp_set_data(T->sample, d, len);
    mod_sample_t *m = cur();
    if (!m->name[0])
        snprintf(m->name, sizeof m->name, "%s", kinds[k]);
    if (!m->volume)
        m->volume = 64;
    if (k != 4) {
        m->loop_start = 0;
        m->loop_len = len;
    }
    smp_fix_loop(m);
}

/// Cuts the selection, or crops to it.
/// @param crop keep only the selection
static void cut_or_crop(bool crop) {
    mod_sample_t *m = cur();
    uint32_t a = T->sel0, b = T->sel1;
    uint32_t len = crop ? b - a : m->length - (b - a);
    if (len < 2) {
        trk_message("That would leave nothing.");
        return;
    }
    int8_t *d = new_data(len);
    if (!d)
        return;
    if (crop) {
        memcpy(d, m->data + a, len);
    } else {
        memcpy(d, m->data, a);
        memcpy(d + a, m->data + b, m->length - b);
    }
    uint32_t ls = m->loop_start, ll = m->loop_len;
    undo_keep_mod_sample(T->sample);
    smp_set_data(T->sample, d, len);
    // The loop moves with what it was on, if it can.
    if (ll > 2) {
        if (crop) {
            ls = ls > a ? ls - a : 0;
        } else if (ls >= b) {
            ls -= b - a;
        }
        m->loop_start = ls;
        m->loop_len = ll;
        smp_fix_loop(m);
    }
    T->sel0 = T->sel1 = 0;
}

/// Halves or doubles the rate.
/// @param halve halve (else double)
static void resample(bool halve) {
    mod_sample_t *m = cur();
    uint32_t len = halve ? m->length / 2 : m->length * 2;
    len &= ~1u;
    if (len < 2 || len > MAX_LEN) {
        trk_message(halve ? "Too short to halve." : "Too long to double (the most is %u bytes).", MAX_LEN);
        return;
    }
    int8_t *d = new_data(len);
    if (!d)
        return;
    for (uint32_t i = 0; i < len; ++i) {
        if (halve) {
            d[i] = (m->data[2 * i] + m->data[2 * i + 1]) / 2;
        } else {
            int a = m->data[i / 2], b = i / 2 + 1 < m->length ? m->data[i / 2 + 1] : a;
            d[i] = i & 1 ? (a + b) / 2 : a;
        }
    }
    uint32_t ls = m->loop_start, ll = m->loop_len;
    undo_keep_mod_sample(T->sample);
    smp_set_data(T->sample, d, len);
    if (ll > 2) {
        m->loop_start = halve ? ls / 2 : ls * 2;
        m->loop_len = halve ? ll / 2 : ll * 2;
        smp_fix_loop(m);
    }
    // The pitch stays the same if the notes go with it: say so.
    trk_message("%s: notes now play it an octave %s.", halve ? "Halved" : "Doubled",
                halve ? "higher" : "lower");
}

/// Copies the sample to another number.
static void copy_to(void) {
    char buf[8];
    snprintf(buf, sizeof buf, "%d", T->sample < 31 ? T->sample + 1 : 1);
    if (!gui_input("Copy sample", "Copy to sample number (1-31):", buf, sizeof buf))
        return;
    int n = atoi(buf);
    if (n < 1 || n > 31 || n == T->sample)
        return;
    mod_sample_t *m = cur(), *to = &T->song.samples[n - 1];
    int8_t *d = NULL;
    if (m->length && !(d = new_data(m->length)))
        return;
    if (d)
        memcpy(d, m->data, m->length);
    trk_stop();
    mod_free(to->data);
    *to = *m;
    to->data = d;
    T->dirty = true;
    trk_message("Sample %02d copied to %02d.", T->sample, n);
}

void smp_tool(int tool) {
    mod_sample_t *m = cur();
    uint32_t a, b;
    if (tool == TOOL_LOAD || tool == TOOL_SAVE || tool == TOOL_GENERATE || tool == TOOL_NAME ||
        tool == TOOL_COPY_TO || tool == TOOL_CLEAR || tool == TOOL_AMPLIFY)
        trk_dialog_begin();
    switch (tool) {
    case TOOL_LOAD: {
        char path[256];
        if (file_pick("Load a sample", ".WAV.IFF.8SV.8SVX.RAW.SMP.SAM.", false, path, sizeof path))
            smp_load(path);
        break;
    }
    case TOOL_SAVE: {
        if (!have_sample())
            break;
        char path[256];
        if (file_pick("Save the sample as WAV", ".WAV.", true, path, sizeof path))
            smp_save(path);
        break;
    }
    case TOOL_NAME: {
        char buf[23];
        snprintf(buf, sizeof buf, "%.22s", m->name);
        if (gui_input("Sample name", "Name (22 letters):", buf, 23)) {
            memset(m->name, 0, sizeof m->name);
            memcpy(m->name, buf, strlen(buf));
            T->dirty = true;
        }
        break;
    }
    case TOOL_COPY_TO: if (have_sample()) copy_to(); break;
    case TOOL_CLEAR:
        if (gui_message("Clear sample", "Clear this sample (its sound and name)?", "Clear|Cancel",
                        false) == 0) {
            trk_stop();
            undo_keep_mod_sample(T->sample);
            smp_set_data(T->sample, NULL, 0);
            memset(m->name, 0, sizeof m->name);
            m->volume = 64;
            m->finetune = 0;
        }
        break;
    case TOOL_GENERATE: generate(); break;
    case TOOL_AMPLIFY: {
        if (!have_sample())
            break;
        static char buf[8] = "150";
        if (gui_input("Amplify", "Percent (100 = as it is):", buf, sizeof buf) && atoi(buf) > 0) {
            undo_keep_mod_sample(T->sample);
            amplify(atoi(buf));
            T->dirty = T->wave_dirty = true;
        }
        break;
    }
    case TOOL_NORMALIZE: {
        if (!have_sample())
            break;
        range(&a, &b);
        int peak = 1;
        for (uint32_t i = a; i < b; ++i)
            peak = abs(m->data[i]) > peak ? abs(m->data[i]) : peak;
        undo_keep_mod_sample(T->sample);
        amplify(12700 / peak);
        break;
    }
    case TOOL_REVERSE:
        if (!have_sample())
            break;
        undo_keep_mod_sample(T->sample);
        range(&a, &b);
        for (uint32_t i = a, j = b - 1; i < j; ++i, --j) {
            int8_t t = m->data[i];
            m->data[i] = m->data[j];
            m->data[j] = t;
        }
        break;
    case TOOL_FADE_IN:
    case TOOL_FADE_OUT:
        if (!have_sample())
            break;
        undo_keep_mod_sample(T->sample);
        range(&a, &b);
        for (uint32_t i = a; i < b; ++i) {
            uint32_t k = tool == TOOL_FADE_IN ? i - a : b - 1 - i;
            m->data[i] = m->data[i] * (int)k / (int)(b - a);
        }
        break;
    case TOOL_CENTRE: {
        if (!have_sample())
            break;
        undo_keep_mod_sample(T->sample);
        range(&a, &b);
        int64_t sum = 0;
        for (uint32_t i = a; i < b; ++i)
            sum += m->data[i];
        int dc = (int)(sum / (int64_t)(b - a));
        for (uint32_t i = a; i < b; ++i)
            m->data[i] = clamp8(m->data[i] - dc);
        trk_message("Moved by %d.", -dc);
        break;
    }
    case TOOL_SILENCE:
        if (!have_sample() || !have_selection())
            break;
        undo_keep_mod_sample(T->sample);
        memset(m->data + T->sel0, 0, T->sel1 - T->sel0);
        break;
    case TOOL_CROP:
    case TOOL_CUT:
        if (!have_sample() || !have_selection())
            break;
        trk_stop();
        cut_or_crop(tool == TOOL_CROP);
        break;
    case TOOL_LOOP_SEL:
        if (!have_sample() || !have_selection())
            break;
        m->loop_start = T->sel0;
        m->loop_len = T->sel1 - T->sel0;
        smp_fix_loop(m);
        break;
    case TOOL_HALVE:
    case TOOL_DOUBLE:
        if (!have_sample())
            break;
        trk_stop();
        resample(tool == TOOL_HALVE);
        break;
    case TOOL_SEL_ALL:
        T->sel0 = 0;
        T->sel1 = m->length;
        break;
    case TOOL_ZOOM_SEL:
        if (have_selection()) {
            T->view0 = T->sel0;
            T->view_len = T->sel1 - T->sel0;
        }
        break;
    case TOOL_ZOOM_ALL: view_all(); break;
    }
    if (tool == TOOL_LOAD || tool == TOOL_SAVE || tool == TOOL_GENERATE || tool == TOOL_NAME ||
        tool == TOOL_COPY_TO || tool == TOOL_CLEAR || tool == TOOL_AMPLIFY)
        trk_dialog_end();
    if (tool != TOOL_SEL_ALL && tool != TOOL_ZOOM_SEL && tool != TOOL_ZOOM_ALL && tool != TOOL_SAVE &&
        tool != TOOL_NAME)
        T->dirty = true;
    T->wave_dirty = true;
}

// ---- Sample files ------------------------------------------------------------------------------------------

/// Reads 32 bits little-endian.
/// @param p where
/// @return the value
static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
/// Reads 16 bits little-endian.
/// @param p where
/// @return the value
static uint16_t le16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}
/// Reads 32 bits big-endian.
/// @param p where
/// @return the value
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/// A sound file loaded as 8 bits.
typedef struct {
    int8_t *data;  ///< the sound
    /// Its length, loop and rate.
    ///@{
    uint32_t len, loop_start, loop_len, rate;
    ///@}
    bool cut;  ///< longer than a MOD sample can be
} loaded_t;

/// Loads a WAV (8 or 16 bits, mono or stereo, its loop from smpl).
/// @param f the file
/// @param size its size
/// @param out set
/// @return NULL, or an error
static const char *load_wav(const uint8_t *f, uint32_t size, loaded_t *out) {
    if (size < 12 || memcmp(f + 8, "WAVE", 4))
        return "not a WAV file";
    int format = 0, channels = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0, loop_a = 0, loop_b = 0;
    bool loop = false;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t n = le32(f + off + 4);
        const uint8_t *c = f + off + 8;
        if (n > size - off - 8)
            n = size - off - 8;
        if (!memcmp(f + off, "fmt ", 4) && n >= 16) {
            format = le16(c);
            channels = le16(c + 2);
            out->rate = le32(c + 4);
            bits = le16(c + 14);
            if (format == 0xfffe && n >= 26)
                format = le16(c + 24);  // extensible: the sub-format
        } else if (!memcmp(f + off, "data", 4)) {
            data = c;
            data_len = n;
        } else if (!memcmp(f + off, "smpl", 4) && n >= 60 && le32(c + 28)) {
            loop = true;
            loop_a = le32(c + 36 + 8);
            loop_b = le32(c + 36 + 12) + 1;
        }
        off += 8 + n + (n & 1);
    }
    if (!data || !channels || !bits)
        return "a WAV file without sound";
    if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) &&
        !(format == 3 && bits == 32))
        return "only PCM or 32-bit float WAV files";
    int bytes = bits / 8;
    uint32_t frames = data_len / (bytes * channels);
    if (frames > MAX_LEN) {
        frames = MAX_LEN;
        out->cut = true;
    }
    out->len = frames & ~1u;
    out->data = new_data(out->len);
    if (!out->data)
        return "not enough memory";
    for (uint32_t i = 0; i < out->len; ++i) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t *p = data + ((size_t)i * channels + c) * bytes;
            float v;
            switch (bits) {
            case 8: v = (p[0] - 128) / 128.0f; break;
            case 16: v = (int16_t)le16(p) / 32768.0f; break;
            case 24: v = (int32_t)((uint32_t)p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f; break;
            default: {
                uint32_t u = le32(p);
                if (format == 3) {
                    memcpy(&v, &u, 4);
                } else {
                    v = (int32_t)u / 2147483648.0f;
                }
            }
            }
            sum += v;
        }
        out->data[i] = clamp8((int)lrintf(sum / channels * 128));
    }
    if (loop && loop_b > loop_a && loop_a < out->len) {
        out->loop_start = loop_a;
        out->loop_len = (loop_b < out->len ? loop_b : out->len) - loop_a;
    }
    return NULL;
}

/// Loads an IFF 8SVX.
/// @param f the file
/// @param size its size
/// @param out set
/// @return NULL, or an error
static const char *load_8svx(const uint8_t *f, uint32_t size, loaded_t *out) {
    uint32_t one_shot = 0, repeat = 0;
    const uint8_t *body = NULL;
    uint32_t body_len = 0;
    for (uint32_t off = 12; off + 8 <= size;) {
        uint32_t n = be32(f + off + 4);
        const uint8_t *c = f + off + 8;
        if (n > size - off - 8)
            n = size - off - 8;
        if (!memcmp(f + off, "VHDR", 4) && n >= 20) {
            one_shot = be32(c);
            repeat = be32(c + 4);
            out->rate = c[12] << 8 | c[13];
            if (c[15])
                return "a packed (Fibonacci) 8SVX file";
        } else if (!memcmp(f + off, "BODY", 4)) {
            body = c;
            body_len = n;
        }
        off += 8 + n + (n & 1);
    }
    if (!body)
        return "an IFF file without a BODY";
    if (body_len > MAX_LEN) {
        body_len = MAX_LEN;
        out->cut = true;
    }
    out->len = body_len & ~1u;
    out->data = new_data(out->len);
    if (!out->data)
        return "not enough memory";
    memcpy(out->data, body, out->len);
    if (repeat > 2 && one_shot < out->len) {
        out->loop_start = one_shot;
        out->loop_len = repeat;
    }
    return NULL;
}

void smp_load(const char *path) {
    uint32_t size;
    uint8_t *f = file_read_all(path, &size, 8u << 20);
    if (!f)
        return;
    loaded_t s = {0};
    const char *err;
    const char *kind;
    if (size >= 12 && !memcmp(f, "RIFF", 4)) {
        kind = "WAV";
        err = load_wav(f, size, &s);
    } else if (size >= 12 && !memcmp(f, "FORM", 4) && !memcmp(f + 8, "8SVX", 4)) {
        kind = "IFF";
        err = load_8svx(f, size, &s);
    } else {
        // Anything else: raw signed 8-bit.
        kind = "raw";
        err = NULL;
        s.len = (size > MAX_LEN ? MAX_LEN : size) & ~1u;
        s.cut = size > MAX_LEN;
        s.data = new_data(s.len);
        if (s.data)
            memcpy(s.data, f, s.len);
        else
            err = "not enough memory";
    }
    psram_free(f);
    if (err) {
        trk_message("%s: %s.", path, err);
        return;
    }
    if (s.len < 2) {
        mod_free(s.data);
        trk_message("%s: no sound in it.", path);
        return;
    }
    trk_stop();
    undo_keep_mod_sample(T->sample);
    smp_set_data(T->sample, s.data, s.len);
    mod_sample_t *m = cur();
    const char *base = strrchr(path, '/');
    memset(m->name, 0, sizeof m->name);
    const char *name = base ? base + 1 : path;
    memcpy(m->name, name, strnlen(name, 22));
    m->volume = 64;
    m->finetune = 0;
    m->loop_start = s.loop_start;
    m->loop_len = s.loop_len;
    smp_fix_loop(m);
    T->sel0 = T->sel1 = 0;
    view_all();
    if (s.rate && s.rate > 9000)
        trk_message("%s %s, %lu Hz%s: C-3 plays it at 16574 Hz (Halve rate if it is high).", kind,
                    s.cut ? "cut to 131070 bytes" : "loaded", (unsigned long)s.rate, "");
    else
        trk_message("%s sample loaded: %lu bytes%s.", kind, (unsigned long)s.len,
                    s.cut ? " (cut: a MOD sample holds 131070)" : "");
}

/// Stores 32 bits little-endian.
/// @param p where
/// @param v the value
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

void smp_save(const char *path) {
    // 8-bit mono WAV at 8363 Hz (C-2 on an Amiga), with the loop.
    mod_sample_t *m = cur();
    bool loop = m->loop_len > 2;
    uint32_t smpl = loop ? 8 + 60 : 0;
    uint32_t len = m->length, size = 44 + len + (len & 1) + smpl;
    uint8_t *f = psram_malloc(size);
    if (!f) {
        trk_message("Not enough memory.");
        return;
    }
    memset(f, 0, size);
    memcpy(f, "RIFF", 4);
    put32(f + 4, size - 8);
    memcpy(f + 8, "WAVEfmt ", 8);
    put32(f + 16, 16);
    f[20] = 1;   // PCM
    f[22] = 1;   // mono
    put32(f + 24, 8363);
    put32(f + 28, 8363);
    f[32] = 1;   // block align
    f[34] = 8;   // bits
    uint32_t off = 36;
    if (loop) {
        memcpy(f + off, "smpl", 4);
        put32(f + off + 4, 60);
        uint8_t *c = f + off + 8;
        put32(c + 8, 1000000000u / 8363);  // sample period, ns
        put32(c + 12, 48);                 // MIDI unity note: C-3 here (C-2 Amiga)
        put32(c + 28, 1);                  // one loop
        put32(c + 36 + 8, m->loop_start);
        put32(c + 36 + 12, m->loop_start + m->loop_len - 1);
        off += 8 + 60;
    }
    memcpy(f + off, "data", 4);
    put32(f + off + 4, len);
    for (uint32_t i = 0; i < len; ++i)
        f[off + 8 + i] = (uint8_t)(m->data[i] + 128);
    if (file_write_all(path, f, size))
        trk_message("Saved %s (%lu bytes).", path, (unsigned long)len);
    psram_free(f);
}

// ---- Drawing ---------------------------------------------------------------------------------------------

/// The settings' names.
static const char *const field_names[MF_COUNT] = {"Name", "Volume", "Finetune", "Loop",
                                                  "Loop at", "Loop len", "Sel from", "Sel to"};

/// A setting's value as text.
/// @param f the setting
/// @param v the text
/// @param size room in v
static void field_value(int f, char *v, int size) {
    mod_sample_t *m = cur();
    switch (f) {
    case MF_NAME: snprintf(v, size, "%.22s", m->name); break;
    case MF_VOLUME: snprintf(v, size, "%d", m->volume); break;
    case MF_FINETUNE: snprintf(v, size, "%+d", m->finetune); break;
    case MF_LOOP: snprintf(v, size, "%s", m->loop_len > 2 ? "on" : "off"); break;
    case MF_LOOP_START: snprintf(v, size, "%lu", (unsigned long)m->loop_start); break;
    case MF_LOOP_LEN: snprintf(v, size, "%lu", (unsigned long)(m->loop_len > 2 ? m->loop_len : 0)); break;
    case MF_SEL_START: snprintf(v, size, "%lu", (unsigned long)T->sel0); break;
    case MF_SEL_END: snprintf(v, size, "%lu", (unsigned long)T->sel1); break;
    }
}

/// Draws the sample list.
static void draw_list(void) {
    mod_song_t *s = &T->song;
    if (T->full)
        gfx_bevel(LIST_X, TOP_Y, LIST_W, TOP_H, true, C_FACE);
    if (T->sample < T->smp_top + 1)
        T->smp_top = T->sample - 1;
    if (T->sample > T->smp_top + LIST_ROWS)
        T->smp_top = T->sample - LIST_ROWS;
    for (int i = 0; i < LIST_ROWS; ++i) {
        int n = T->smp_top + 1 + i, y = TOP_Y + 8 + i * 16;
        mod_sample_t *m = &s->samples[n - 1];
        bool on = n == T->sample;
        uint8_t bg = on ? C_CURROW : C_FIELD;
        char name[23];
        memcpy(name, m->name, 22);
        name[22] = 0;
        for (int k = 0; k < 22; ++k)
            if (name[k] && (name[k] < 0x20 || name[k] > 0x7e))
                name[k] = '.';
        gfx_printf(LIST_X + 6, y, 3, C_YELLOW, bg, "%02d", n);
        gfx_text(LIST_X + 30, y, name, 22, m->length ? C_TEXT : C_DIM, bg);
        gfx_text(LIST_X + 206, y, "", 10, C_TEXT, bg);
        hit_add(LIST_X + 6, y, 280, 16, HIT_SAMPLE, n);
    }
}

/// Draws the settings.
static void draw_props(void) {
    mod_sample_t *m = cur();
    if (T->full)
        gfx_bevel(PROP_X, TOP_Y, PROP_W, TOP_H, true, C_FACE);
    for (int f = 0; f < MF_COUNT; ++f) {
        int y = TOP_Y + 8 + f * 16, x = PROP_X + 6;
        bool focus = T->smp_field == f;
        gfx_text(x, y, field_names[f], 9, focus ? C_YELLOW : C_LABEL, C_FACE);
        char v[32];
        field_value(f, v, sizeof v);
        uint8_t bg = focus ? C_FOCUS : C_FIELD;
        int w = f == MF_NAME ? 22 : 7;
        gfx_fill(x + 72, y, 2, 16, bg);
        int end = gfx_text(x + 74, y, v, w, C_TEXT, bg);
        gfx_fill(end, y, 2, 16, bg);
        hit_add(x + 72, y, w * 8 + 4, 16, HIT_SMP_FIELD, f);
        if (f == MF_NAME)
            continue;
        gfx_button(x + 136, y, 18, 16, "-", HIT_SMP_DEC, f, false);
        gfx_button(x + 156, y, 18, 16, "+", HIT_SMP_INC, f, false);
        char info[24] = "";
        if (f == MF_VOLUME)
            snprintf(info, sizeof info, "Length %lu", (unsigned long)m->length);
        else if (f == MF_FINETUNE)
            snprintf(info, sizeof info, "Sample %02d", T->sample);
        else if (f == MF_LOOP && m->loop_len > 2)
            snprintf(info, sizeof info, "to %lu", (unsigned long)(m->loop_start + m->loop_len));
        else if (f == MF_SEL_END && T->sel1 > T->sel0)
            snprintf(info, sizeof info, "%lu bytes", (unsigned long)(T->sel1 - T->sel0));
        else if (f == MF_LOOP_LEN)
            snprintf(info, sizeof info, "Ctrl-Z undo");
        gfx_text(x + 180, y, info, 20, C_DIM, C_FACE);
    }
}

/// Works out each column's lowest and highest value.
static void wave_columns(void) {
    mod_sample_t *m = cur();
    int iw = DVI_WIDTH - 2;
    if (!m->length || !m->data) {
        memset(T->wmin, 0, sizeof T->wmin);
        memset(T->wmax, 0, sizeof T->wmax);
        return;
    }
    if (!T->view_len || T->view0 + T->view_len > m->length) {
        T->view0 = 0;
        T->view_len = m->length;
    }
    for (int x = 0; x < iw; ++x) {
        uint32_t a = T->view0 + (uint64_t)x * T->view_len / iw;
        uint32_t b = T->view0 + (uint64_t)(x + 1) * T->view_len / iw;
        if (b <= a)
            b = a + 1;
        if (b > m->length)
            b = m->length;
        int lo = 127, hi = -128;
        for (uint32_t i = a; i < b; ++i) {
            int v = m->data[i];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        if (a >= m->length)
            lo = hi = 0;
        T->wmin[x] = lo;
        T->wmax[x] = hi;
    }
}

/// The column of a byte position, or -1 if it isn't shown.
/// @param p the position
/// @return the column
static int pos_x(uint32_t p) {
    if (!T->view_len || p < T->view0 || p > T->view0 + T->view_len)
        return -1;
    return (int)((uint64_t)(p - T->view0) * (DVI_WIDTH - 2) / T->view_len);
}

/// The position at a column.
/// @param x the column
/// @return the position
static uint32_t x_pos(int x) {
    if (x < 0)
        x = 0;
    if (x > DVI_WIDTH - 2)
        x = DVI_WIDTH - 2;
    uint32_t p = T->view0 + (uint64_t)x * T->view_len / (DVI_WIDTH - 2);
    uint32_t len = cur()->length;
    return p > len ? len : p;
}

/// Draws the waveform, the loop, the selection and the play position.
static void draw_wave(void) {
    mod_sample_t *m = cur();
    if (T->full)
        gfx_bevel(0, WAVE_Y, DVI_WIDTH, WAVE_H, false, C_BG);
    if (T->wave_dirty)
        wave_columns();
    mod_status_t st;
    mod_get_status(&st);
    int play_x[MOD_MAX_CH], nplay = 0;
    for (int c = 0; c < T->song.channels; ++c)
        if (st.sample[c] == T->sample && (play_x[nplay] = pos_x(st.pos[c])) >= 0)
            ++nplay;
    static int last_play[MOD_MAX_CH], last_nplay;
    bool moved = nplay != last_nplay;
    for (int i = 0; i < nplay && !moved; ++i)
        moved = play_x[i] != last_play[i];
    if (!T->full && !T->wave_dirty && !moved)
        return;
    memcpy(last_play, play_x, sizeof play_x);
    last_nplay = nplay;
    T->wave_dirty = false;

    bool loop = m->loop_len > 2 && m->length;
    int lx0 = loop ? pos_x(m->loop_start) : -1, lx1 = loop ? pos_x(m->loop_start + m->loop_len) : -1;
    uint32_t vl = T->view_len;
    int sx0 = -1, sx1 = -1;
    if (T->sel1 > T->sel0 && vl) {
        sx0 = T->sel0 <= T->view0 ? 0 : pos_x(T->sel0);
        sx1 = T->sel1 >= T->view0 + vl ? DVI_WIDTH - 2 : pos_x(T->sel1);
        if (T->sel1 <= T->view0 || T->sel0 >= T->view0 + vl)
            sx0 = sx1 = -1;
    }
    int top = WAVE_Y + 1, h = WAVE_H - 2, mid = top + h / 2;
    uint32_t loop_a = loop ? m->loop_start : 0, loop_b = loop ? m->loop_start + m->loop_len : 0;
    for (int x = 0; x < DVI_WIDTH - 2; ++x) {
        uint32_t p = x_pos(x);
        bool sel = sx0 >= 0 && x >= sx0 && x < sx1;
        bool in_loop = loop && p >= loop_a && p < loop_b;
        uint8_t bg = sel ? C_BLOCK : in_loop ? C_ROW4 : C_BG;
        uint8_t line = x == lx0 || x == lx1 ? C_ORANGE : 0;
        for (int i = 0; i < nplay; ++i)
            if (play_x[i] == x)
                line = C_TEXT;
        // Joined to the column before, so steep edges show.
        int hi_v = T->wmax[x], lo_v = T->wmin[x];
        if (x > 0) {
            hi_v = hi_v > T->wmin[x - 1] ? hi_v : T->wmin[x - 1];
            lo_v = lo_v < T->wmax[x - 1] ? lo_v : T->wmax[x - 1];
        }
        int y0 = mid - hi_v * (h / 2 - 2) / 128, y1 = mid - lo_v * (h / 2 - 2) / 128;
        uint8_t *d = dvi_framebuf + top * DVI_WIDTH + 1 + x;
        uint8_t wave = m->length && p < m->length ? C_GREEN : C_DIM;
        for (int y = top; y < top + h; ++y, d += DVI_WIDTH) {
            if (line)
                *d = line;
            else if (y >= y0 && y <= y1)
                *d = wave;
            else
                *d = y == mid ? C_DARK : bg;
        }
    }
    // The loop's ends, marked at the top.
    if (lx0 >= 0)
        gfx_text(1 + lx0 + 2 > DVI_WIDTH - 10 ? DVI_WIDTH - 10 : 1 + lx0 + 2, top, "S", 1, C_ORANGE, C_BG);
    if (lx1 >= 0)
        gfx_text(lx1 - 8 < 1 ? 1 : lx1 - 8, top, "E", 1, C_ORANGE, C_BG);
    if (!m->length)
        gfx_text(DVI_WIDTH / 2 - 15 * 4, mid - 24, "(empty sample)", 15, C_DIM, C_BG);
    hit_add(1, top, DVI_WIDTH - 2, h, HIT_WAVE, 0);
}

/// Draws the view bar.
static void draw_bar(void) {
    // Where the view is in the sample.
    mod_sample_t *m = cur();
    gfx_fill(0, BAR_Y, DVI_WIDTH, 16, C_DARK);
    if (m->length && T->view_len) {
        int a = (int)((uint64_t)T->view0 * DVI_WIDTH / m->length);
        int w = (int)((uint64_t)T->view_len * DVI_WIDTH / m->length);
        gfx_bevel(a, BAR_Y + 2, w < 6 ? 6 : w, 12, true, C_FACE);
    }
    hit_add(0, BAR_Y, DVI_WIDTH, 16, HIT_WAVE_SCROLL, 0);
}

/// Draws the tools.
static void draw_tools(void) {
    static const struct {
        const char *label;
        int tool;
    } b[20] = {
        {"Load", TOOL_LOAD},       {"Save", TOOL_SAVE},        {"Synth", TOOL_GENERATE},
        {"Normal", TOOL_NORMALIZE}, {"Amplify", TOOL_AMPLIFY}, {"Reverse", TOOL_REVERSE},
        {"Fade in", TOOL_FADE_IN}, {"FadeOut", TOOL_FADE_OUT}, {"No DC", TOOL_CENTRE},
        {"Clear", TOOL_CLEAR},     {"Sel all", TOOL_SEL_ALL},  {"ZoomSel", TOOL_ZOOM_SEL},
        {"ZoomAll", TOOL_ZOOM_ALL}, {"Crop", TOOL_CROP},       {"Cut", TOOL_CUT},
        {"Silence", TOOL_SILENCE}, {"LoopSel", TOOL_LOOP_SEL}, {"Halve", TOOL_HALVE},
        {"Double", TOOL_DOUBLE},   {"Patt F2", 0},
    };
    for (int i = 0; i < 20; ++i)
        gfx_button((i % 10) * 64, TOOL_Y + (i / 10) * 16, 64, 16, b[i].label,
                   b[i].tool ? HIT_TOOL : HIT_SCREEN_PATTERN, b[i].tool, false);
}

void smp_draw(void) {
    draw_list();
    draw_props();
    draw_wave();
    draw_bar();
    draw_tools();
}

// ---- Keys and the mouse ------------------------------------------------------------------------------

/// Changes a setting by a step.
/// @param f the setting
/// @param d the step
static void field_step(int f, int d) {
    mod_sample_t *m = cur();
    int big = d / 16;  // PgUp/PgDn: 16 times as far
    switch (f) {
    case MF_VOLUME:
        m->volume = m->volume + d < 0 ? 0 : m->volume + d > 64 ? 64 : m->volume + d;
        break;
    case MF_FINETUNE: {
        int ft = m->finetune + (d > 0 ? 1 : -1);
        m->finetune = ft < -8 ? -8 : ft > 7 ? 7 : ft;
        break;
    }
    case MF_LOOP:
        if (m->loop_len > 2) {
            m->loop_start = 0;
            m->loop_len = 2;
        } else if (m->length > 2) {
            // On: the selection if there is one, else the whole sample.
            m->loop_start = T->sel1 > T->sel0 ? T->sel0 : 0;
            m->loop_len = T->sel1 > T->sel0 ? T->sel1 - T->sel0 : m->length;
        }
        smp_fix_loop(m);
        break;
    default: {
        // Positions: by about a pixel of the view (big steps 16 of them).
        int32_t step = T->view_len / (DVI_WIDTH - 2);
        step = (step < 2 ? 2 : step) & ~1;
        int32_t by = (big ? big * 16 : d) * step;
        int32_t len = m->length;
        if (f == MF_LOOP_START) {
            int32_t end = m->loop_start + m->loop_len, v = m->loop_start + by;
            if (m->loop_len <= 2)
                end = len;
            v = v < 0 ? 0 : v > end - 2 ? end - 2 : v;
            m->loop_start = v;
            m->loop_len = end - v;
        } else if (f == MF_LOOP_LEN) {
            int32_t v = (m->loop_len > 2 ? m->loop_len : 0) + by;
            int32_t most = len - m->loop_start;
            m->loop_len = v < 2 ? 2 : v > most ? most : v;
        } else if (f == MF_SEL_START) {
            int32_t v = T->sel0 + by;
            T->sel0 = v < 0 ? 0 : v > len ? len : v;
            if (T->sel1 < T->sel0)
                T->sel1 = T->sel0;
        } else if (f == MF_SEL_END) {
            int32_t v = T->sel1 + by;
            T->sel1 = v < (int32_t)T->sel0 ? (int32_t)T->sel0 : v > len ? len : v;
        }
        if (f == MF_LOOP_START || f == MF_LOOP_LEN)
            smp_fix_loop(m);
    }
    }
    if (f != MF_SEL_START && f != MF_SEL_END)
        T->dirty = true;
    T->wave_dirty = true;
}

/// Edits a setting by typing.
/// @param f the setting
static void field_enter(int f) {
    mod_sample_t *m = cur();
    if (f == MF_NAME) {
        smp_tool(TOOL_NAME);
        return;
    }
    if (f == MF_LOOP) {
        field_step(f, 1);
        return;
    }
    char buf[12];
    field_value(f, buf, sizeof buf);
    trk_dialog_begin();
    bool ok = gui_input(field_names[f], "New value:", buf, sizeof buf);
    trk_dialog_end();
    if (!ok)
        return;
    long v = strtol(buf, NULL, 10);
    switch (f) {
    case MF_VOLUME: m->volume = v < 0 ? 0 : v > 64 ? 64 : v; break;
    case MF_FINETUNE: m->finetune = v < -8 ? -8 : v > 7 ? 7 : v; break;
    case MF_LOOP_START: m->loop_start = v < 0 ? 0 : v; if (m->loop_len <= 2) m->loop_len = m->length - m->loop_start; break;
    case MF_LOOP_LEN: m->loop_len = v < 0 ? 0 : v; break;
    case MF_SEL_START: T->sel0 = v < 0 ? 0 : (uint32_t)v > m->length ? m->length : (uint32_t)v; break;
    case MF_SEL_END: T->sel1 = v < 0 ? 0 : (uint32_t)v > m->length ? m->length : (uint32_t)v; break;
    }
    if (T->sel1 < T->sel0)
        T->sel1 = T->sel0;
    smp_fix_loop(m);
    if (f != MF_SEL_START && f != MF_SEL_END)
        T->dirty = true;
    T->wave_dirty = true;
}

/// Zooms in or out about the middle.
/// @param d +1 in, -1 out
static void zoom(int d) {
    mod_sample_t *m = cur();
    if (!m->length)
        return;
    uint32_t centre = T->view0 + T->view_len / 2, len = T->view_len;
    len = d > 0 ? len / 2 : len * 2;
    if (len < 16)
        len = 16;
    if (len > m->length)
        len = m->length;
    T->view_len = len;
    T->view0 = centre > len / 2 ? centre - len / 2 : 0;
    if (T->view0 + len > m->length)
        T->view0 = m->length - len;
    T->wave_dirty = true;
}

/// Scrolls the view.
/// @param d eighths of the view
static void scroll(int d) {
    mod_sample_t *m = cur();
    int64_t v = (int64_t)T->view0 + (int64_t)d * (T->view_len / 8 ? T->view_len / 8 : 1);
    if (v + T->view_len > m->length)
        v = (int64_t)m->length - T->view_len;
    T->view0 = v < 0 ? 0 : (uint32_t)v;
    T->wave_dirty = true;
}

bool smp_key(int k) {
    switch (k) {
    case KEY_UP: T->smp_field = (T->smp_field + MF_COUNT - 1) % MF_COUNT; return true;
    case KEY_DOWN: T->smp_field = (T->smp_field + 1) % MF_COUNT; return true;
    case KEY_LEFT: field_step(T->smp_field, -1); return true;
    case KEY_RIGHT: field_step(T->smp_field, 1); return true;
    case KEY_PGDN: field_step(T->smp_field, -16); return true;
    case KEY_PGUP: field_step(T->smp_field, 16); return true;
    case '\r': field_enter(T->smp_field); return true;
    case KEY_ALT + 'l': smp_tool(TOOL_SEL_ALL); return true;
    case KEY_ALT + 'u': T->sel0 = T->sel1 = 0; T->wave_dirty = true; return true;
    case KEY_ALT + 'z': smp_tool(TOOL_ZOOM_SEL); return true;
    case KEY_ALT + 'o': smp_tool(TOOL_ZOOM_ALL); return true;
    case KEY_ALT + 'x': smp_tool(TOOL_CUT); return true;
    case KEY_ALT + 'k': smp_tool(TOOL_CROP); return true;
    case KEY_HOME: scroll(-(1 << 20)); return true;
    case KEY_END: scroll(1 << 20); return true;
    case KEY_ALT + '=':
    case KEY_ALT + '+': zoom(1); return true;
    case KEY_ALT + '-': zoom(-1); return true;
    case KEY_ALT + ',': scroll(-1); return true;
    case KEY_ALT + '.': scroll(1); return true;
    }
    int n = trk_note_key(k);
    if (n >= 0) {
        trk_jam(T->sample, n);
        return true;
    }
    return false;
}

void smp_wheel(const hit_t *h, int x, int d) {
    if (h && (h->id == HIT_SMP_FIELD || h->id == HIT_SMP_DEC || h->id == HIT_SMP_INC))
        field_step(h->arg, -d);
    else if (h && h->id == HIT_SAMPLE)
        T->sample = T->sample + d < 1 ? 1 : T->sample + d > 31 ? 31 : T->sample + d, view_all();
    else if (h && h->id == HIT_WAVE_SCROLL)
        scroll(d);
    else
        zoom(-d);
}

void smp_click(const hit_t *h, int x, int y, bool drag) {
    static int dragging;  // 0 selection, 1 loop start, 2 loop end, 3 the scroll bar
    static uint32_t anchor;
    mod_sample_t *m = cur();
    if (!drag) {
        if (!h)
            return;
        if (h->id == HIT_SMP_DEC || h->id == HIT_SMP_INC) {
            T->smp_field = h->arg;
            field_step(h->arg, h->id == HIT_SMP_INC ? 1 : -1);
            return;
        }
        if (h->id == HIT_SMP_FIELD) {
            T->smp_field = h->arg;
            field_enter(h->arg);
            return;
        }
        if (h->id == HIT_WAVE_SCROLL) {
            dragging = 3;
        } else {
            // Near a loop end: move it; else start a selection.
            int lx0 = m->loop_len > 2 ? pos_x(m->loop_start) : -100;
            int lx1 = m->loop_len > 2 ? pos_x(m->loop_start + m->loop_len) : -100;
            int px = x - 1;
            dragging = abs(px - lx0) <= 4 ? 1 : abs(px - lx1) <= 4 ? 2 : 0;
            if (!dragging) {
                anchor = x_pos(px);
                T->sel0 = T->sel1 = anchor;
            }
        }
    }
    if (dragging == 3) {
        if (m->length && T->view_len < m->length) {
            int64_t centre = (int64_t)x * m->length / DVI_WIDTH;
            int64_t v = centre - T->view_len / 2;
            if (v + T->view_len > m->length)
                v = m->length - T->view_len;
            T->view0 = v < 0 ? 0 : (uint32_t)v;
        }
    } else if (dragging == 0) {
        uint32_t p = x_pos(x - 1);
        T->sel0 = p < anchor ? p : anchor;
        T->sel1 = p < anchor ? anchor : p;
    } else {
        uint32_t p = x_pos(x - 1) & ~1u;
        uint32_t end = m->loop_start + m->loop_len;
        if (dragging == 1 && p + 2 <= end) {
            m->loop_start = p;
            m->loop_len = end - p;
        } else if (dragging == 2 && p >= m->loop_start + 2) {
            m->loop_len = p - m->loop_start;
        }
        smp_fix_loop(m);
        T->dirty = true;
    }
    T->wave_dirty = true;
}
