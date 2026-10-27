/**
 * @file trk_undo.c
 * @brief Undo and redo for the tracker (Ctrl-Z, Ctrl-Y), many steps back.
 *
 * Most edits are noticed rather than announced: after each key or click
 * undo_watch compares the pattern being edited, the order list and song
 * settings, and the current sample's (MOD) or instrument's (XM) settings
 * with copies taken before, and keeps the old copy if they changed. Tools
 * that change sample data (or add and remove an XM instrument's samples)
 * keep a full copy first with undo_keep_*. The copies live in PSRAM with a
 * checksum each, as the editor's buffers do: a damaged one isn't restored.
 * Undoing something keeps what it replaces for redo.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psram_heap.h"
#include "trk.h"

enum {
    K_MOD_PAT,    ///< a MOD pattern: channels, then its cells as the file has them
    K_XM_PAT,     ///< an XM pattern: rows (2 bytes), channels, then its cells
    K_SONG,       ///< the title, order list, restart, speed and tempo
    K_MOD_SHDR,   ///< a MOD sample's settings (mod_sample_t)
    K_XM_IHDR,    ///< an XM instrument's settings and its samples' (xm_instrument_t, xm_sample_t...)
    K_MOD_SDEEP,  ///< a MOD sample with its sound
    K_XM_IDEEP,   ///< an XM instrument with its samples' sound
};

typedef struct {
    uint8_t kind;
    int16_t index;
    ///@{
    uint32_t size, sum;
    ///@}
    uint8_t *data;
} rec_t;

#define MAX_RECS 64
#define BUDGET   (3u << 20)   ///< bytes of copies kept at most
#define PAT_MAX  (4 + 256 * XM_MAX_CH * sizeof(xm_cell_t))

static rec_t *undos, *redos;  ///< MAX_RECS each (PSRAM: SRAM is short)
static int nundo, nredo;
static uint32_t used;

typedef struct {
    ///@{
    int kind, index;
    ///@}
    uint32_t size;
    uint8_t *buf;
    bool valid;
} watch_t;
static watch_t w_pat, w_song, w_hdr;
static uint8_t *scratch;

typedef struct {
    char title[21];
    uint8_t orders[256];
    ///@{
    int length, restart, speed, bpm;
    ///@}
} song_state_t;

static uint32_t checksum(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

// ---- Copies of things ----------------------------------------------------------------------------

static uint32_t mod_pattern_bytes(void) {
    return MOD_ROWS * T->song.channels * 4;
}

/// What `kind` `index` is now, into out (if not NULL); its size, 0 if it
/// doesn't exist.
static uint32_t build(int kind, int index, uint8_t *out) {
    xm_song_t *x = T->xm;
    switch (kind) {
    case K_MOD_PAT: {
        if (x || index < 0 || index >= MOD_MAX_PATTERNS)
            return 0;
        uint32_t n = mod_pattern_bytes();
        if (out) {
            out[0] = T->song.channels;
            memcpy(out + 1, T->song.patterns + (size_t)index * n, n);
        }
        return 1 + n;
    }
    case K_XM_PAT: {
        if (!x || index < 0 || index >= XM_MAX_PATTERNS || !x->patterns[index].cells)
            return 0;
        const xm_pattern_t *p = &x->patterns[index];
        uint32_t n = (uint32_t)p->rows * x->channels * sizeof(xm_cell_t);
        if (out) {
            out[0] = p->rows & 0xff;
            out[1] = p->rows >> 8;
            out[2] = x->channels;
            memcpy(out + 3, p->cells, n);
        }
        return 3 + n;
    }
    case K_SONG: {
        if (out) {
            song_state_t s;
            memset(&s, 0, sizeof s);
            if (x) {
                memcpy(s.title, x->title, sizeof s.title);
                memcpy(s.orders, x->orders, 256);
                s.length = x->length, s.restart = x->restart, s.speed = x->speed, s.bpm = x->bpm;
            } else {
                memcpy(s.title, T->song.title, sizeof s.title);
                memcpy(s.orders, T->song.orders, 128);
                s.length = T->song.length, s.restart = T->song.restart;
            }
            memcpy(out, &s, sizeof s);
        }
        return sizeof(song_state_t);
    }
    case K_MOD_SHDR:
    case K_MOD_SDEEP: {
        if (x || index < 1 || index > MOD_MAX_SAMPLES)
            return 0;
        const mod_sample_t *m = &T->song.samples[index - 1];
        uint32_t data = kind == K_MOD_SDEEP && m->data ? m->length + 2 : 0;
        if (out) {
            memcpy(out, m, sizeof *m);
            if (data)
                memcpy(out + sizeof *m, m->data, data);
        }
        return sizeof *m + data;
    }
    case K_XM_IHDR:
    case K_XM_IDEEP: {
        if (!x || index < 1 || index > x->ninstruments)
            return 0;
        const xm_instrument_t *in = &x->instruments[index - 1];
        if (in->nsamples > 16 && kind == K_XM_IHDR)
            return 0;
        uint32_t n = sizeof *in + in->nsamples * sizeof(xm_sample_t);
        if (out) {
            memcpy(out, in, sizeof *in);
            memcpy(out + sizeof *in, in->samples, in->nsamples * sizeof(xm_sample_t));
        }
        if (kind == K_XM_IDEEP)
            for (int i = 0; i < in->nsamples; ++i) {
                const xm_sample_t *sm = &in->samples[i];
                uint32_t d = sm->data ? (sm->length + 1) * sizeof(int16_t) : 0;
                if (out && d)
                    memcpy(out + n, sm->data, d);
                n += d;
            }
        return n;
    }
    }
    return 0;
}

/// Whether a settings copy still fits what is there now (the same channels,
/// the same sound data): only then can it go back, or be worth keeping.
static bool fits(int kind, int index, const uint8_t *blob) {
    xm_song_t *x = T->xm;
    switch (kind) {
    case K_MOD_PAT: return !x && blob[0] == T->song.channels;
    case K_XM_PAT: return x && blob[2] == x->channels;
    case K_MOD_SHDR: {
        mod_sample_t m;
        memcpy(&m, blob, sizeof m);
        const mod_sample_t *now = &T->song.samples[index - 1];
        return !x && m.data == now->data && m.length == now->length;
    }
    case K_XM_IHDR: {
        if (!x || index > x->ninstruments)
            return false;
        xm_instrument_t in;
        memcpy(&in, blob, sizeof in);
        const xm_instrument_t *now = &x->instruments[index - 1];
        if (in.samples != now->samples || in.nsamples != now->nsamples)
            return false;
        for (int i = 0; i < in.nsamples; ++i) {
            xm_sample_t sm;
            memcpy(&sm, blob + sizeof in + i * sizeof sm, sizeof sm);
            if (sm.data != now->samples[i].data || sm.length != now->samples[i].length)
                return false;
        }
        return true;
    }
    }
    return true;
}

/// Puts a copy back. False (with a message) if it can't.
static bool apply(int kind, int index, const uint8_t *blob, uint32_t size) {
    xm_song_t *x = T->xm;
    if (!fits(kind, index, blob)) {
        trk_message("Can't undo past a change of channels or samples.");
        return false;
    }
    switch (kind) {
    case K_MOD_PAT:
        memcpy(T->song.patterns + (size_t)index * mod_pattern_bytes(), blob + 1, size - 1);
        if (index >= T->song.npatterns)
            T->song.npatterns = index + 1;
        return true;
    case K_XM_PAT: {
        xm_pattern_t *p = &x->patterns[index];
        int rows = blob[0] | blob[1] << 8;
        if (rows != p->rows || !p->cells) {
            xm_cell_t *c = xm_alloc(size - 3);
            if (!c) {
                trk_message("Not enough memory.");
                return false;
            }
            xm_stop();
            xm_free(p->cells);
            p->cells = c;
            p->rows = rows;
        }
        memcpy(p->cells, blob + 3, size - 3);
        return true;
    }
    case K_SONG: {
        song_state_t s;
        memcpy(&s, blob, sizeof s);
        if (x) {
            memcpy(x->title, s.title, sizeof s.title);
            memcpy(x->orders, s.orders, 256);
            x->length = s.length, x->restart = s.restart, x->speed = s.speed, x->bpm = s.bpm;
        } else {
            memcpy(T->song.title, s.title, sizeof s.title);
            memcpy(T->song.orders, s.orders, 128);
            T->song.length = s.length, T->song.restart = s.restart;
        }
        return true;
    }
    case K_MOD_SHDR:
        memcpy(&T->song.samples[index - 1], blob, sizeof(mod_sample_t));
        return true;
    case K_XM_IHDR: {
        xm_instrument_t *in = &x->instruments[index - 1];
        memcpy(in, blob, sizeof *in);
        memcpy(in->samples, blob + sizeof *in, in->nsamples * sizeof(xm_sample_t));
        return true;
    }
    case K_MOD_SDEEP: {
        mod_sample_t m;
        memcpy(&m, blob, sizeof m);
        int8_t *d = NULL;
        if (m.data && m.length) {
            if (!(d = mod_alloc(m.length + 2))) {
                trk_message("Not enough memory.");
                return false;
            }
            memcpy(d, blob + sizeof m, m.length + 2);
        }
        trk_stop();
        smp_set_data(index, d, d ? m.length : 0);
        mod_sample_t *now = &T->song.samples[index - 1];
        m.data = now->data;
        m.length = now->length;
        *now = m;
        smp_fix_loop(now);
        return true;
    }
    case K_XM_IDEEP: {
        xm_instrument_t in;
        memcpy(&in, blob, sizeof in);
        xm_sample_t *list = NULL;
        if (in.nsamples && !(list = xm_alloc(in.nsamples * sizeof *list))) {
            trk_message("Not enough memory.");
            return false;
        }
        const uint8_t *p = blob + sizeof in + in.nsamples * sizeof *list;
        memcpy(list, blob + sizeof in, in.nsamples * sizeof *list);
        bool ok = true;
        for (int i = 0; i < in.nsamples; ++i) {
            xm_sample_t *sm = &list[i];
            if (!sm->data)
                continue;
            uint32_t d = (sm->length + 1) * sizeof(int16_t);
            if (!ok || !(sm->data = xm_alloc(d))) {
                sm->data = NULL;
                ok = false;
                continue;
            }
            memcpy(sm->data, p, d);
            p += d;
        }
        if (!ok) {
            for (int i = 0; i < in.nsamples; ++i)
                xm_free(list[i].data);
            xm_free(list);
            trk_message("Not enough memory.");
            return false;
        }
        xm_stop();
        xm_instrument_t *now = &x->instruments[index - 1];
        for (int i = 0; i < now->nsamples; ++i)
            xm_free(now->samples[i].data);
        xm_free(now->samples);
        in.samples = list;
        *now = in;
        return true;
    }
    }
    return false;
}

// ---- The lists -------------------------------------------------------------------------------------

static void drop(rec_t *r) {
    used -= r->size;
    psram_free(r->data);
    r->data = NULL;
}

static void drop_all(rec_t *list, int *n) {
    while (*n)
        drop(&list[--*n]);
}

/// Keeps a copy (taking `data`, from psram_malloc), the oldest going if
/// there are too many.
static void push(rec_t *list, int *n, int kind, int index, uint8_t *data, uint32_t size) {
    rec_t *other = list == undos ? redos : undos;
    int *no = list == undos ? &nredo : &nundo;
    while (*n && (*n >= MAX_RECS || used + size > BUDGET)) {
        drop(&list[0]);
        memmove(list, list + 1, (--*n) * sizeof *list);
    }
    // Still too much: the other list's oldest go too.
    while (*no && used + size > BUDGET) {
        drop(&other[0]);
        memmove(other, other + 1, (--*no) * sizeof *other);
    }
    if (used + size > BUDGET) {
        psram_free(data);
        trk_message("Too big to undo.");
        return;
    }
    list[*n] = (rec_t){(uint8_t)kind, (int16_t)index, size, checksum(data, size), data};
    ++*n;
    used += size;
}

static uint8_t *copy_of(int kind, int index, uint32_t *size) {
    *size = build(kind, index, NULL);
    if (!*size)
        return NULL;
    uint8_t *d = psram_malloc(*size);
    if (d)
        build(kind, index, d);
    return d;
}

// ---- Watching -----------------------------------------------------------------------------------

static void targets(int *pk, int *pi, int *hk, int *hi) {
    if (T->xm) {
        *pk = K_XM_PAT, *pi = xed_pattern();
        *hk = K_XM_IHDR, *hi = T->sample;
    } else {
        *pk = K_MOD_PAT, *pi = T->pattern;
        *hk = K_MOD_SHDR, *hi = T->sample;
    }
}

static void take(watch_t *w, int kind, int index) {
    w->kind = kind;
    w->index = index;
    w->size = w->buf ? build(kind, index, w->buf) : 0;
    w->valid = w->size > 0;
}

static void check(watch_t *w, int kind, int index) {
    if (w->valid) {
        uint32_t n = build(w->kind, w->index, scratch);
        if (n && (n != w->size || memcmp(scratch, w->buf, n)) && fits(w->kind, w->index, w->buf)) {
            uint8_t *d = psram_malloc(w->size);
            if (d) {
                memcpy(d, w->buf, w->size);
                push(undos, &nundo, w->kind, w->index, d, w->size);
                drop_all(redos, &nredo);
            }
        }
    }
    take(w, kind, index);
}

void undo_watch(void) {
    if (!undos) {
        undos = psram_malloc(2 * MAX_RECS * sizeof *undos);
        if (!undos)
            return;
        redos = undos + MAX_RECS;
    }
    if (!scratch && !(scratch = psram_malloc(PAT_MAX)))
        return;
    if (!w_pat.buf) {
        w_pat.buf = psram_malloc(PAT_MAX);
        w_song.buf = psram_malloc(sizeof(song_state_t));
        w_hdr.buf = psram_malloc(sizeof(xm_instrument_t) + 16 * sizeof(xm_sample_t) + sizeof(mod_sample_t));
    }
    int pk, pi, hk, hi;
    targets(&pk, &pi, &hk, &hi);
    check(&w_pat, pk, pi);
    check(&w_song, K_SONG, 0);
    check(&w_hdr, hk, hi);
}

/// After an undo (or a new song): what is there now is the starting point.
static void rewatch(void) {
    int pk, pi, hk, hi;
    targets(&pk, &pi, &hk, &hi);
    take(&w_pat, pk, pi);
    take(&w_song, K_SONG, 0);
    take(&w_hdr, hk, hi);
}

// ---- The tracker's side --------------------------------------------------------------------------

static void keep_deep(int kind, int index) {
    undo_watch();  // settings changed just before go in first
    uint32_t size;
    uint8_t *d = copy_of(kind, index, &size);
    if (!d) {
        if (size)
            trk_message("Not enough memory to keep an undo copy.");
        return;
    }
    push(undos, &nundo, kind, index, d, size);
    drop_all(redos, &nredo);
}

void undo_keep_mod_sample(int n) {
    keep_deep(K_MOD_SDEEP, n);
}

void undo_keep_xm_instrument(int n) {
    keep_deep(K_XM_IDEEP, n);
}

bool undo_can(bool redo) {
    return redo ? nredo > 0 : nundo > 0;
}

bool undo_step(bool redo) {
    undo_watch();  // an edit not yet noticed is the newest
    if (!undos)
        return false;
    rec_t *list = redo ? redos : undos;
    int *n = redo ? &nredo : &nundo;
    if (!*n) {
        trk_message(redo ? "Nothing to redo." : "Nothing to undo.");
        return false;
    }
    rec_t r = list[--*n];
    used -= r.size;
    if (checksum(r.data, r.size) != r.sum) {
        psram_free(r.data);
        trk_message("That undo copy was damaged in memory; it is dropped.");
        return false;
    }
    uint32_t size;
    uint8_t *now = copy_of(r.kind, r.index, &size);
    bool ok = apply(r.kind, r.index, r.data, r.size);
    psram_free(r.data);
    if (ok && now)
        push(redo ? undos : redos, redo ? &nundo : &nredo, r.kind, r.index, now, size);
    else
        psram_free(now);
    rewatch();
    if (ok) {
        static const char *const what[] = {"pattern", "pattern", "song", "sample settings",
                                           "instrument settings", "sample", "instrument"};
        T->dirty = true;
        T->full = true;
        T->wave_dirty = true;
        trk_message("%s %s %d. (%d more to undo, %d to redo)", redo ? "Redone:" : "Undone:",
                    what[r.kind], r.index, nundo, nredo);
    }
    return ok;
}

void undo_reset(void) {
    if (!undos)
        return;
    drop_all(undos, &nundo);
    drop_all(redos, &nredo);
    if (w_pat.buf)
        rewatch();
}

void undo_free(void) {
    undo_reset();
    psram_free(scratch);
    psram_free(w_pat.buf);
    psram_free(w_song.buf);
    psram_free(w_hdr.buf);
    psram_free(undos);
    undos = redos = NULL;
    scratch = w_pat.buf = w_song.buf = w_hdr.buf = NULL;
    w_pat.valid = w_song.valid = w_hdr.valid = false;
}
