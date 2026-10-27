/**
 * @file audio.c
 * @brief Implementation of audio.h: sound over HDMI.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "audio.h"

#include <math.h>
#include <stdlib.h>

#include "ay38910.h"
#include "chime.h"
#include "c64_sid.h"
#include "hdmi_audio.h"
#include "psram_heap.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "pico/util/queue.h"

#define SINE_LEN  256
#define AMPLITUDE 8000  ///< about -12 dBFS
#define TWO_PI    6.28318531f

static int16_t sine[SINE_LEN];
static volatile uint32_t beep_step;  ///< phase increment per frame, 16.16 fixed point
static volatile uint32_t beep_frames_left;
static volatile bool beep_silent;  ///< a rest from the note queue

/// Notes for audio_play: a tone (or a rest, step 0) and its length in frames.
typedef struct {
    ///@{
    uint32_t step, frames;
    ///@}
} note_t;
static queue_t notes;
static volatile bool note_playing;  ///< the current sound came from the queue

/// Speaker clicks: times (low 32 bits of time_us_64) written by core 0 and
/// played on core 1 SPK_DELAY_US later. A DC-blocking filter turns the
/// cone's two positions into sound and silence when it stops moving.
#define SPK_QUEUE    512
#define SPK_DELAY_US 40000
#define SPK_LEVEL    6000
static volatile uint32_t *volatile spk_times;  ///< SPK_QUEUE, made on the first click (SRAM is short)
static volatile uint32_t spk_head, spk_tail;
static uint64_t spk_clock;  ///< the time of the frame being made, in 1/65536 us
static int32_t spk_in, spk_prev_in, spk_out;
static bool spk_up;

void audio_speaker_click(uint64_t when_us) {
    if (!spk_times) {
        uint32_t *t = calloc(SPK_QUEUE, sizeof *t);
        if (!t)
            return;
        spk_times = t;
        __dmb();
    }
    uint32_t next = (spk_head + 1) % SPK_QUEUE;
    if (next == spk_tail)
        return;  // full: drop it
    spk_times[spk_head] = (uint32_t)when_us;
    __dmb();
    spk_head = next;
}

/// Another sound source mixed in on core 1 (the MOD player): a function
/// giving the next stereo frame, or NULL.
static bool (*volatile source)(int32_t *left, int32_t *right);

static volatile uint32_t source_busy_us, source_total_us;

void audio_set_source(bool (*fn)(int32_t *left, int32_t *right)) {
    source = fn;
}

bool (*audio_get_source(void))(int32_t *left, int32_t *right) {
    return source;
}

unsigned audio_source_load(void) {
    // Since the last call: core 1's time in the source over the time passed.
    static uint32_t last_busy, last_total;
    uint32_t busy = source_busy_us - last_busy, total = source_total_us - last_total;
    last_busy += busy;
    last_total += total;
    return total ? (unsigned)((uint64_t)busy * 1000 / total) : 0;
}

static uint32_t sample_time;  ///< the frame being made, in time_us_64() terms (low bits)

/// The AY chips: register writes from core 0, applied on core 1 at their
/// times (SPK_DELAY_US later, as the speaker's clicks).
#define AY_QUEUE 512
typedef struct {
    uint32_t when;
    ///@{
    uint8_t chip, reg, value;
    ///@}
} ay_event_t;
static ay_event_t *volatile ay_queue;  ///< AY_QUEUE, made by the first audio_ay_start
static volatile uint32_t ay_head, ay_tail;
static volatile bool ay_on, ay_reset_wanted;
static volatile uint32_t ay_clock_hz;
static volatile unsigned ay_gain_q8 = 512;
static ay38910 ay[2];

void audio_ay_start(uint32_t clock_hz) {
    if (!ay_queue) {
        ay_event_t *q = calloc(AY_QUEUE, sizeof *q);
        if (!q)
            return;  // no memory: the chips stay silent
        ay_queue = q;
    }
    ay_clock_hz = clock_hz;
    ay_reset_wanted = true;
    __dmb();
    ay_on = true;
}

void audio_trim(void) {
    // Core 1 checks the pointer each time round: once it is NULL and core 1
    // has been round once more, the queue is no longer read.
    void *ay = NULL, *spk = NULL;
    if (ay_queue && !ay_on) {
        ay = ay_queue;
        ay_queue = NULL;
    }
    if (spk_times && spk_head == spk_tail) {
        spk = (void *)spk_times;
        spk_times = NULL;
        spk_head = spk_tail = 0;
    }
    if (!ay && !spk)
        return;
    __dmb();
    sleep_ms(3);
    if (ay)
        ay_head = ay_tail = 0;  // (a new queue starts empty)
    free(ay);
    free(spk);
}

void audio_ay_gain(unsigned q8) {
    ay_gain_q8 = q8;
}

void audio_ay_stop(void) {
    ay_on = false;
}

void audio_ay_write(unsigned chip, uint8_t reg, uint8_t value, uint64_t when_us) {
    if (!ay_queue)
        return;
    uint32_t next = (ay_head + 1) % AY_QUEUE;
    if (next == ay_tail)
        return;  // full: drop it
    ay_queue[ay_head].when = (uint32_t)when_us;
    ay_queue[ay_head].chip = (uint8_t)chip;
    ay_queue[ay_head].reg = reg;
    ay_queue[ay_head].value = value;
    __dmb();
    ay_head = next;
}

/// The chips' next stereo sample (core 1), after any writes now due.
static void ay_samples(int32_t *left, int32_t *right) {
    if (ay_reset_wanted) {
        ay_reset_wanted = false;
        ay_tail = ay_head;
        ay_init(&ay[0], ay_clock_hz, HDMI_AUDIO_RATE);
        ay_init(&ay[1], ay_clock_hz, HDMI_AUDIO_RATE);
    }
    while (ay_queue && ay_tail != ay_head && (int32_t)(ay_queue[ay_tail].when - sample_time) <= 0) {
        ay_write(&ay[ay_queue[ay_tail].chip & 1], ay_queue[ay_tail].reg, ay_queue[ay_tail].value);
        ay_tail = (ay_tail + 1) % AY_QUEUE;
    }
    // By default twice the chips' level: about as loud as the speaker at the
    // volumes music uses.
    *left = ay_sample(&ay[0]) * (int32_t)ay_gain_q8 / 256;
    *right = ay_sample(&ay[1]) * (int32_t)ay_gain_q8 / 256;
}

/// The SID: register writes from core 0, applied on core 1 at their times,
/// as the AY chips'.
#define SID_QUEUE 512  ///< 40 ms of writes: a tune's 50 a second, or a digi's 8000
/// In PSRAM, allocated the first time and kept (core 1 may be reading it).
static struct sid_write {
    uint32_t when;
    ///@{
    uint8_t reg, value;
    ///@}
} *sid_queue;
static volatile uint32_t sid_head, sid_tail;
static volatile bool sid_on, sid_reset_wanted;
static volatile uint32_t sid_clock_hz;
static c64_sid sid;
static volatile uint64_t sid_busy_us, sid_since_us;
static volatile unsigned sid_gain_q8 = 256;

void audio_sid_gain(unsigned q8) {
    sid_gain_q8 = q8;
}

void audio_sid_start(uint32_t clock_hz) {
    if (!sid_queue && !(sid_queue = psram_malloc(SID_QUEUE * sizeof *sid_queue)))
        return;  // no PSRAM: no sound
    sid_clock_hz = clock_hz;
    sid_reset_wanted = true;
    __dmb();
    sid_on = true;
}

void audio_sid_stop(void) {
    sid_on = false;
}

void audio_sid_write(uint8_t reg, uint8_t value, uint64_t when_us) {
    if (!sid_on)
        return;
    uint32_t next = (sid_head + 1) % SID_QUEUE;
    if (next == sid_tail)
        return;  // full: drop it
    sid_queue[sid_head].when = (uint32_t)when_us;
    sid_queue[sid_head].reg = reg;
    sid_queue[sid_head].value = value;
    __dmb();
    sid_head = next;
}

unsigned audio_sid_load(void) {
    uint64_t span = time_us_64() - sid_since_us;
    return span ? (unsigned)(sid_busy_us * 1000 / span) : 0;
}

static int32_t sid_sample(void) {
    if (sid_reset_wanted) {
        sid_reset_wanted = false;
        sid_tail = sid_head;
        c64_sid_init(&sid, sid_clock_hz, HDMI_AUDIO_RATE);
        sid_busy_us = 0;
        sid_since_us = time_us_64();
    }
    while (sid_tail != sid_head && (int32_t)(sid_queue[sid_tail].when - sample_time) <= 0) {
        c64_sid_write(&sid, sid_queue[sid_tail].reg, sid_queue[sid_tail].value);
        sid_tail = (sid_tail + 1) % SID_QUEUE;
    }
    return c64_sid_sample(&sid) * (int32_t)sid_gain_q8 / 256;
}

/// The speaker's next sample (core 1).
static int32_t speaker_sample(void) {
    uint64_t now = (time_us_64() - SPK_DELAY_US) << 16;
    int64_t drift = (int64_t)(now - spk_clock);
    if (drift > (20000ll << 16) || drift < -(20000ll << 16))
        spk_clock = now;  // first use, or fell behind: catch up
    uint32_t t = (uint32_t)(spk_clock >> 16);
    sample_time = t;
    spk_clock += 1365333;  // 1/48000 s
    while (spk_times && spk_tail != spk_head && (int32_t)(spk_times[spk_tail] - t) <= 0) {
        spk_up = !spk_up;
        spk_tail = (spk_tail + 1) % SPK_QUEUE;
    }
    spk_in = spk_up ? SPK_LEVEL : -SPK_LEVEL;
    // y = x - x[-1] + 0.996 y[-1]
    spk_out = spk_in - spk_prev_in + (spk_out * 255) / 256;
    spk_prev_in = spk_in;
    return spk_out;
}

static volatile bool chime_wanted, chime_on;
static uint32_t chime_underruns0;
volatile uint32_t chime_underruns;  ///< sound missed while it played (read over SWD)

void audio_chime(void) {
    chime_wanted = true;
}

bool audio_chime_playing(void) {
    return chime_wanted || chime_on;
}

void audio_beep(unsigned freq_hz, unsigned ms) {
    beep_frames_left = 0;
    beep_silent = false;
    note_playing = false;
    beep_step = (uint32_t)(((uint64_t)freq_hz * SINE_LEN << 16) / HDMI_AUDIO_RATE);
    beep_frames_left = ms * (HDMI_AUDIO_RATE / 1000);
}

/// Above 24000 the level rises a quarter as fast, so loud music flattens
/// gently instead of clipping hard.
static int16_t soft_limit(int32_t x) {
    if (x > 24000)
        x = 24000 + (x - 24000) / 4;
    else if (x < -24000)
        x = -24000 + (x + 24000) / 4;
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/// Keeps the packet queue full. When nothing is playing it sends silence, so
/// the sink always has a steady stream and doesn't mute and unmute. Each
/// packet is generated only once there is room for it, so a beep starts
/// within one queue length (about 5 ms) of being asked for.
void __not_in_flash_func(audio_task)(void) {
    static uint32_t phase;
    int16_t frames[4][2];
    static uint32_t last_us;
    uint32_t now_us = time_us_32();
    source_total_us += now_us - last_us;  // timing to 1 us averages out over many frames
    last_us = now_us;
    while (hdmi_audio_has_room()) {
        bool timing = sid_on;  // read once: core 0 may switch it on meanwhile
        uint32_t t0 = timing ? time_us_32() : 0;
        for (int i = 0; i < 4; ++i) {
            int16_t s = 0;
            uint32_t left = beep_frames_left;
            if (!left) {
                note_t n;
                note_playing = false;
                // The lock only when something is queued (it may be shared).
                if (queue_get_level_unsafe(&notes) && queue_try_remove(&notes, &n)) {
                    beep_step = n.step;
                    beep_silent = n.step == 0;
                    left = beep_frames_left = n.frames;
                    note_playing = true;
                }
            }
            if (left) {
                if (!beep_silent) {
                    s = sine[(phase >> 16) % SINE_LEN];
                    phase += beep_step;
                }
                beep_frames_left = left - 1;
            }
            int32_t mix = s + speaker_sample(), l = 0, r = 0;
            if (ay_on)
                ay_samples(&l, &r);
            if (sid_on)
                mix += sid_sample();
            l += mix;
            r += mix;
            if (chime_wanted) {
                chime_wanted = false;
                chime_begin();
                chime_on = true;
                chime_underruns0 = hdmi_audio_underruns;
            }
            if (chime_on) {
                chime_on = chime_sample(&l, &r);
                chime_underruns = hdmi_audio_underruns - chime_underruns0;
            }
            bool (*src)(int32_t *, int32_t *) = source;
            if (src) {
                int32_t sl, sr;
                uint32_t s0 = time_us_32();
                src(&sl, &sr);
                source_busy_us += time_us_32() - s0;
                l += sl / 2;  // two full channels on a side stay under the limiter
                r += sr / 2;
            }
            frames[i][0] = soft_limit(l);
            frames[i][1] = soft_limit(r);
        }
        if (timing)
            sid_busy_us += time_us_32() - t0;  // with the rest of the mixing, which is small
        hdmi_audio_push(frames);
    }
}

/// Core 1: audio_task from a 1 ms timer interrupt on core 1, so the queue
/// stays full whatever core 1's loop is doing (a USB restart sleeps 100 ms;
/// the queue holds 5 ms).
static repeating_timer_t refill_timer;

static bool refill(repeating_timer_t *t) {
    (void)t;
    audio_task();
    return true;
}

bool audio_start_refill(void) {
    alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(2);
    return pool && alarm_pool_add_repeating_timer_us(pool, -1000, refill, NULL, &refill_timer);
}

bool audio_play(unsigned freq_hz, unsigned ms) {
    note_t n = {freq_hz ? (uint32_t)(((uint64_t)freq_hz * SINE_LEN << 16) / HDMI_AUDIO_RATE) : 0,
                ms * (HDMI_AUDIO_RATE / 1000)};
    return n.frames == 0 || queue_try_add(&notes, &n);
}

int audio_play_pending(void) {
    return queue_get_level(&notes) + (note_playing ? 1 : 0);
}

void audio_play_stop(void) {
    note_t n;
    while (queue_try_remove(&notes, &n))
        ;
    if (note_playing)
        beep_frames_left = 0;
}

void audio_init(void) {
    queue_init(&notes, sizeof(note_t), AUDIO_QUEUE_LEN);
    for (int i = 0; i < SINE_LEN; ++i)
        sine[i] = (int16_t)(AMPLITUDE * sinf(TWO_PI * i / SINE_LEN));
}
