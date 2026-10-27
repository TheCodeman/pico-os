/**
 * @file radio.c
 * @brief Internet radio: the stream (HTTP/HTTPS, ICY song titles, redirects and playlists), its buffer, and the MP3 or AAC decoder feeding the HDMI sound.
 *
 * See radio.h.
 *
 * Who does what:
 *   core 0, foreground: connecting (radio_play), the browser.
 *   core 0, lwIP's low-priority interrupt: a worker every few milliseconds
 *     reads what has arrived (decrypting it if HTTPS), takes out the ICY
 *     metadata and puts the MP3 or AAC data in a ring buffer in PSRAM.
 *   core 1, a job in its main loop: decodes a frame (Helix MP3 or AAC, fixed
 *     point) whenever the PCM ring has room; the audio refill resamples that
 *     to 48 kHz.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

// Text is often cut to fit on purpose.
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "radio.h"

#include <ctype.h>
#include <limits.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_overlay.h"
#include "audio.h"
#include "core1_job.h"
#include "hardware/sync.h"
#include "aac/aacdec.h"
#include "mp3dec.h"
#include "net.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

#define RING_SIZE      (256 * 1024)  ///< about 16 s at 128 kbit/s
#define PREBUFFER      (40 * 1024)   ///< before playing starts (or again after running dry)
#define INBUF_SIZE     4096          ///< the decoder's window on the stream
/// The longest reply header.
#define HEADER_MAX     4096
/// Redirects followed.
#define MAX_REDIRECTS  5
/// How often the stream is read (ms).
#define PUMP_MS        4
#define IDLE_MS        15000         ///< nothing arriving for this long: the stream has gone

// ---- URLs and connections (as wget's) ------------------------------------------------------

typedef struct {
    bool https;
    char host[128];
    uint16_t port;
    char path[512];
} url_t;

typedef struct {
    net_conn_t *conn;
    net_tls_t *tls;
} stream_t;

/// Reads.
/// @param s the stream
/// @param buf where
/// @param max how many
/// @return what net_tls_read or net_conn_read returns
static int s_read(stream_t *s, void *buf, size_t max) {
    return s->tls ? net_tls_read(s->tls, buf, max) : net_conn_read(s->conn, buf, max);
}

/// Writes.
/// @param s the stream
/// @param data the bytes
/// @param len how many
/// @return written
static bool s_write(stream_t *s, const void *data, size_t len) {
    return s->tls ? net_tls_write(s->tls, data, len) : net_conn_write(s->conn, data, len);
}

/// Closes (and forgets) the connection.
/// @param s the stream
static void s_close(stream_t *s) {
    if (s->tls)
        net_tls_end(s->tls);
    if (s->conn)
        net_conn_close(s->conn);
    s->tls = NULL;
    s->conn = NULL;
}

/// Connects, with TLS for https (certificates not checked).
/// @param s the stream
/// @param u the URL
/// @return connected
static bool s_open(stream_t *s, const url_t *u) {
    s->tls = NULL;
    // Radio streams: no certificate check (most stations' certificates are
    // fine, but CACERT.PEM may be missing and it would be no help here).
    s->conn = net_conn_open(u->host, u->port);
    if (!s->conn)
        return false;
    if (u->https && !(s->tls = net_tls_start(s->conn, u->host, false))) {
        net_conn_close(s->conn);
        s->conn = NULL;
        return false;
    }
    return true;
}

/// [http[s]://]host[:port][/path]; a redirect may give "/path" or "//host/path".
/// @param s the text
/// @param u the parts
/// @param base the URL redirected from, or NULL
/// @return parsed
static bool parse_url(const char *s, url_t *u, const url_t *base) {
    while (*s == ' ')
        ++s;
    if (base && s[0] == '/' && s[1] != '/') {
        *u = *base;
        snprintf(u->path, sizeof u->path, "%s", s);
        return true;
    }
    u->https = base ? base->https : false;
    if (!strncasecmp(s, "https://", 8)) {
        u->https = true;
        s += 8;
    } else if (!strncasecmp(s, "http://", 7)) {
        u->https = false;
        s += 7;
    } else if (s[0] == '/' && s[1] == '/') {
        s += 2;
    } else if (strstr(s, "://")) {
        return false;
    }
    size_t n = strcspn(s, ":/?");
    if (!n || n >= sizeof u->host)
        return false;
    memcpy(u->host, s, n);
    u->host[n] = 0;
    s += n;
    u->port = u->https ? 443 : 80;
    if (*s == ':') {
        u->port = (uint16_t)atoi(s + 1);
        s += strcspn(s, "/?");
    }
    snprintf(u->path, sizeof u->path, "%s%s", *s == '?' ? "/" : "", *s ? s : "/");
    // Spaces aren't allowed in a request line.
    for (char *c = u->path; *c; ++c)
        if (*c == ' ' || *c == '\r' || *c == '\n')
            *c = '+';
    return true;
}

/// Finds header `name` (case-insensitive) in hdr and copies its value.
/// @param hdr the header
/// @param name the field
/// @param out its value
/// @param size room in out
/// @return found
static bool header(const char *hdr, const char *name, char *out, size_t size) {
    size_t n = strlen(name);
    for (const char *p = strchr(hdr, '\n'); p; p = strchr(p, '\n')) {
        ++p;
        if (!strncasecmp(p, name, n) && p[n] == ':') {
            p += n + 1;
            while (*p == ' ')
                ++p;
            size_t len = strcspn(p, "\r\n");
            len = len < size ? len : size - 1;
            memcpy(out, p, len);
            out[len] = 0;
            return true;
        }
    }
    return false;
}

static const char *fail;  ///< why the last request failed

/// Sends a GET and reads the reply's header into hdr (0-terminated). The body
/// bytes read past it go to body (at most body_max); their count, or -1.
/// @param s the stream
/// @param u the URL
/// @param icy ask for song titles (Icy-MetaData)
/// @param hdr the header
/// @param body the body's first bytes
/// @param body_max room in body
/// @return the body bytes, or -1
static int request(stream_t *s, const url_t *u, bool icy, char *hdr, char *body, int body_max) {
    char req[768];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pico-os-radio/1.0\r\nAccept: */*\r\n%s"
             "Connection: close\r\n\r\n",
             u->path, u->host, icy ? "Icy-MetaData: 1\r\n" : "");
    if (!s_write(s, req, strlen(req))) {
        fail = "couldn't send the request";
        return -1;
    }
    int len = 0;
    absolute_time_t idle = make_timeout_time_ms(IDLE_MS);
    for (;;) {
        if (net_stop_key()) {
            fail = "stopped";
            return -1;
        }
        int n = s_read(s, hdr + len, HEADER_MAX - 1 - len);
        if (n < 0) {
            fail = len ? "the reply was cut short" : "the server closed the connection";
            return -1;
        }
        if (!n) {
            if (time_reached(idle)) {
                fail = "no reply";
                return -1;
            }
            sleep_ms(2);
            continue;
        }
        len += n;
        hdr[len] = 0;
        char *end = strstr(hdr, "\r\n\r\n");
        int skip = 4;
        if (!end && (end = strstr(hdr, "\n\n")))
            skip = 2;
        if (end) {
            int hl = end + skip - hdr, extra = len - hl;
            extra = extra < body_max ? extra : body_max;
            memcpy(body, hdr + hl, extra);
            *end = 0;
            return extra;
        }
        if (len >= HEADER_MAX - 1) {
            fail = "the reply's header is too long";
            return -1;
        }
    }
}

/// The reply's status code.
/// @param hdr the header
/// @return the code, or 0
static int status_code(const char *hdr) {
    // "HTTP/1.x 200 OK" or Shoutcast's "ICY 200 OK".
    const char *sp = strchr(hdr, ' ');
    return sp ? atoi(sp + 1) : 0;
}

/// The first stream URL in a .pls or .m3u playlist.
/// @param text the playlist
/// @param out the URL
/// @param size room in out
/// @return found
static bool playlist_url(const char *text, char *out, size_t size) {
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\r\n");
        const char *line = p;
        p += n;
        while (*p == '\r' || *p == '\n')
            ++p;
        const char *h = NULL;
        if (!strncasecmp(line, "File", 4) && memchr(line, '=', n))
            h = memchr(line, '=', n) + 1;
        else if (!strncasecmp(line, "http", 4))
            h = line;
        if (h && (size_t)(line + n - h) < size) {
            memcpy(out, h, line + n - h);
            out[line + n - h] = 0;
            return true;
        }
    }
    return false;
}

/// Whether a reply is a playlist, by its type or the path's extension.
/// @param type the Content-Type
/// @param path the path
/// @return it is
static bool is_playlist(const char *type, const char *path) {
    const char *q = path + strcspn(path, "?#");
    int n = q - path;
    bool ext = (n > 4 && !strncasecmp(q - 4, ".pls", 4)) || (n > 4 && !strncasecmp(q - 4, ".m3u", 4)) ||
               (n > 5 && !strncasecmp(q - 5, ".m3u8", 5));
    return ext || strstr(type, "scpls") || strstr(type, "mpegurl") || strstr(type, "x-scpls");
}

// ---- State ---------------------------------------------------------------------------------------

/// RADIO_OFF and the like.
static volatile int state;
static radio_status_t info;      ///< the strings and numbers radio_get_status copies
/// The connection.
static stream_t conn;
static uint8_t *ring;            ///< PSRAM
/// Bytes ever put in and taken out.
///@{
static volatile uint32_t ring_in, ring_out;  ///< bytes ever put in and taken out
///@}
static volatile bool ended;      ///< the stream closed (what's in the ring still plays)
/// The worker that reads the stream.
static async_at_time_worker_t pump_worker;
/// It is scheduled.
static bool pump_on;
/// When data last arrived.
static uint32_t last_data_ms;
/// The volume (0-100).
static int volume = 80;
/// Muted.
static volatile bool muted;
/// ICY metadata: every `metaint` bytes of audio, a length byte (x16) and that
/// much text ("StreamTitle='...';").
/// Where the metadata comes, the audio until then, the metadata left and its length.
///@{
static int metaint, audio_left, meta_left, meta_len;
///@}
static char *meta;  ///< 512 bytes after the ring and its read buffer

/// Bytes in the ring.
/// @return the count
static uint32_t ring_count(void) {
    return ring_in - ring_out;
}

/// Puts bytes into the ring.
/// @param p them
/// @param n how many
static void ring_put(const uint8_t *p, int n) {
    uint32_t at = ring_in % RING_SIZE;
    int first = n < (int)(RING_SIZE - at) ? n : (int)(RING_SIZE - at);
    memcpy(ring + at, p, first);
    memcpy(ring, p + first, n - first);
    __dmb();
    ring_in += n;
}

/// Takes the song title out of the metadata.
/// @param m the metadata text
static void set_title(const char *m) {
    const char *t = strstr(m, "StreamTitle='");
    if (!t)
        return;
    t += 13;
    const char *e = strstr(t, "';");
    int n = e ? e - t : (int)strlen(t);
    char title[sizeof info.title];
    n = n < (int)sizeof title - 1 ? n : (int)sizeof title - 1;
    memcpy(title, t, n);
    title[n] = 0;
    for (char *c = title; *c; ++c)
        if ((uint8_t)*c < 0x20)
            *c = ' ';
    memcpy(info.title, title, sizeof title);
}

/// Data from the stream: audio into the ring, metadata taken out.
/// @param p the bytes
/// @param n their count
static void feed(const uint8_t *p, int n) {
    while (n > 0) {
        if (!metaint) {
            ring_put(p, n);
            return;
        }
        if (audio_left) {
            int k = n < audio_left ? n : audio_left;
            ring_put(p, k);
            audio_left -= k;
            p += k;
            n -= k;
        } else if (!meta_left && meta_len < 0) {
            meta_left = *p++ * 16;  // the length byte
            --n;
            meta_len = 0;
            if (!meta_left) {
                audio_left = metaint;
                meta_len = -1;
            }
        } else {
            int k = n < meta_left ? n : meta_left;
            for (int i = 0; i < k; ++i)
                if (meta_len < 511)
                    meta[meta_len++] = p[i];
            meta_left -= k;
            p += k;
            n -= k;
            if (!meta_left) {
                meta[meta_len] = 0;
                set_title(meta);
                audio_left = metaint;
                meta_len = -1;
            }
        }
    }
}

/// lwIP's context, every PUMP_MS: what has arrived, into the ring.
/// @param ctx lwIP's context
/// @param w the worker
static void pump(async_context_t *ctx, async_at_time_worker_t *w) {
    if (!pump_on)
        return;
    uint8_t *buf = ring + RING_SIZE;  // 2 KB after the ring (SRAM is short)
    const int bufsize = 2048;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int rounds = 0; rounds < 8 && conn.conn && !ended; ++rounds) {
        // Room for a whole read (metadata bytes needn't fit).
        if (RING_SIZE - ring_count() < (uint32_t)bufsize)
            break;
        int n = s_read(&conn, buf, bufsize);
        if (n < 0) {
            ended = true;
            snprintf(info.error, sizeof info.error, "the station ended the stream");
            break;
        }
        if (!n)
            break;
        last_data_ms = now;
        feed(buf, n);
    }
    if (!ended && conn.conn && now - last_data_ms > IDLE_MS && RING_SIZE - ring_count() >= (uint32_t)bufsize) {
        ended = true;
        snprintf(info.error, sizeof info.error, "nothing has come for %d s", IDLE_MS / 1000);
    }
    async_context_add_at_time_worker_in_ms(ctx, w, PUMP_MS);
}

// ---- Decoding (core 1) -------------------------------------------------------------------------------
// Core 1's main loop decodes a frame at a time into a PCM ring (PSRAM), as a
// core 1 job; the audio refill (a 1 ms interrupt whose HDMI queue holds only
// 5 ms) just resamples from that ring, so a 3 ms decode can't starve it.

#define PCM_FRAMES 32768         ///< stereo frames at the stream's rate, about 0.7 s
                                 // (core 1 may be busy with the drive a while)
#define PCM_START  4096          ///< (re)starting playing once it has this many
#define FRAME_MAX  1152          ///< the most a frame decodes to (MP3; AAC 1024)

/// The codecs.
enum { CODEC_MP3, CODEC_AAC };
/// The stream's codec (CODEC_MP3 or CODEC_AAC).
static int codec;
static void *decoder;            ///< HMP3Decoder or HAACDecoder
static uint8_t *inbuf;           ///< the stream's next bytes for the decoder
static int in_len;               ///< valid bytes from in_at
/// Where they start in inbuf.
static int in_at;
static int16_t *pcm;             ///< one decoded frame (FRAME_MAX x 2)
static int16_t *pcm_ring;        ///< PCM_FRAMES x 2 (PSRAM)
/// Frames ever written and played.
///@{
static volatile uint32_t pcm_in, pcm_out;  ///< frames ever written and played
///@}
static uint32_t pos;             ///< the next 48 kHz frame's place after pcm_out, 16.16
static volatile int32_t step;    ///< the stream's rate / 48 kHz, 16.16
/// The decoder's buffers are in SRAM; the decoding job is on core 1.
///@{
static bool decoder_sram, job_on;
///@}
/// Microseconds spent decoding; times the PCM ran dry.
///@{
static volatile uint32_t busy_us, underruns;
///@}
/// The left and right levels for the meters.
///@{
static volatile uint8_t level_l, level_r;
///@}

/// The decoder's own buffers (24 KB): in the apps' SRAM area (app_overlay.h)
/// while the radio plays (the decoder is several times slower from PSRAM).
/// AAC's (about 21 KB) too. (Not HE-AAC's SBR, whose 50 KB state would have
/// to be in PSRAM, too slow: an HE-AAC stream plays its AAC core, at half
/// the sample rate.)
static uint8_t *helix_pool;
/// Used of the pool, and its size.
///@{
static size_t helix_used, helix_size;
///@}

/// Helix's malloc: from the pool.
/// @param n the size
/// @return the memory, or NULL
void *radio_helix_alloc(size_t n) {
    n = (n + 7) & ~7u;
    if (helix_used + n > helix_size)
        return NULL;
    void *p = helix_pool + helix_used;
    helix_used += n;
    return p;
}

/// Helix's free: nothing (the pool goes as a whole).
/// @param p the memory
void radio_helix_free(void *p) {
    (void)p;  // the pool goes as a whole
}


/// MPEG audio or ADTS (AAC) frame headers at p: which, from the first one
/// found in n bytes that the next agrees with; -1 if neither turns up.
/// @param p the bytes
/// @param n their count
/// @return CODEC_MP3, CODEC_AAC or -1
static int sniff_codec(const uint8_t *p, int n) {
    for (int i = 0; i + 7 < n; ++i) {
        if (p[i] != 0xff || (p[i + 1] & 0xe0) != 0xe0)
            continue;
        if ((p[i + 1] & 0xf6) == 0xf0) {  // ADTS: 12 sync bits, layer 0
            int len = (p[i + 3] & 3) << 11 | p[i + 4] << 3 | p[i + 5] >> 5;
            if (len >= 7 && (i + len + 1 >= n || (p[i + len] == 0xff && (p[i + len + 1] & 0xf6) == 0xf0)))
                return CODEC_AAC;
        } else if (p[i + 1] & 6) {  // MPEG audio: a layer
            return CODEC_MP3;
        }
    }
    return -1;
}


/// Fills the decoder's window from the ring.
static void refill(void) {
    if (in_at && in_len)
        memmove(inbuf, inbuf + in_at, in_len);
    in_at = 0;
    uint32_t have = ring_count();
    int want = INBUF_SIZE - in_len;
    int n = have < (uint32_t)want ? (int)have : want;
    uint32_t at = ring_out % RING_SIZE;
    int first = n < (int)(RING_SIZE - at) ? n : (int)(RING_SIZE - at);
    memcpy(inbuf + in_len, ring + at, first);
    memcpy(inbuf + in_len + first, ring, n - first);
    in_len += n;
    __dmb();
    ring_out += n;
}

/// The next frame into pcm; its frame count, or 0 if there isn't enough data.
/// @return the frame count
static int decode_frame(void) {
    for (int tries = 0; tries < 16; ++tries) {
        if (in_len < INBUF_SIZE / 2)
            refill();
        if (in_len < 4)
            return 0;
        int off = codec == CODEC_AAC ? AACFindSyncWord(inbuf + in_at, in_len)
                                     : MP3FindSyncWord(inbuf + in_at, in_len);
        if (off < 0) {
            in_at += in_len - 3;  // keep the last bytes: a sync word may start there
            in_len = 3;
            continue;
        }
        in_at += off;
        in_len -= off;
        unsigned char *p = inbuf + in_at;
        int left = in_len;
        int r;
        if (codec == CODEC_AAC) {
            // The AAC decoder doesn't check it has the whole frame: the
            // ADTS header says how long it is.
            int len = in_len < 7 ? INT_MAX : (p[3] & 3) << 11 | p[4] << 3 | p[5] >> 5;
            if (len < 7) {
                ++in_at;  // a false sync
                --in_len;
                continue;
            }
            r = len > in_len ? ERR_AAC_INDATA_UNDERFLOW : AACDecode(decoder, &p, &left, pcm);
        } else {
            r = MP3Decode(decoder, &p, &left, pcm, 0);
        }
        if (r == (codec == CODEC_AAC ? ERR_AAC_INDATA_UNDERFLOW : ERR_MP3_INDATA_UNDERFLOW)) {
            if (in_len >= INBUF_SIZE - 16) {  // can't be a frame: a false sync
                ++in_at;
                --in_len;
                continue;
            }
            if (!ring_count())
                return 0;  // wait for more
            refill();
            continue;
        }
        in_at += in_len - left;
        in_len = left;
        if (r == 0) {  // ERR_MP3_NONE, ERR_AAC_NONE
            int rate, chans, samps;
            if (codec == CODEC_AAC) {
                AACFrameInfo fi;
                AACGetLastFrameInfo(decoder, &fi);
                rate = fi.sampRateOut;
                chans = fi.nChans;
                samps = fi.outputSamps;
                if (fi.bitRate)
                    info.bitrate = fi.bitRate / 1000;
                // (24 kHz or less: HE-AAC, its SBR left out.)
                strcpy(info.codec, fi.sampRateCore <= 24000 ? "HE-AAC" : "AAC");
            } else {
                MP3FrameInfo fi;
                MP3GetLastFrameInfo(decoder, &fi);
                rate = fi.samprate;
                chans = fi.nChans;
                samps = fi.outputSamps;
                info.bitrate = fi.bitrate / 1000;
            }
            info.samplerate = rate;
            info.channels = chans;
            int frames = chans == 2 ? samps / 2 : samps;
            if (frames > FRAME_MAX || chans < 1 || chans > 2)
                continue;  // (can't be: more than the decoders make)
            if (chans == 1)  // mono: spread to both sides, from the end back
                for (int i = frames - 1; i >= 0; --i)
                    pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
            if (rate > 0)
                step = (int32_t)(((int64_t)rate << 16) / 48000);
            int pl = 0, pr = 0;
            for (int i = 0; i < frames; i += 4) {
                int a = abs(pcm[2 * i]), b = abs(pcm[2 * i + 1]);
                pl = a > pl ? a : pl;
                pr = b > pr ? b : pr;
            }
            level_l = (uint8_t)(pl * volume / 100 >> 9);
            level_r = (uint8_t)(pr * volume / 100 >> 9);
            return frames;
        }
        if ((codec == CODEC_AAC || r != ERR_MP3_MAINDATA_UNDERFLOW) && left == in_len) {
            ++in_at;  // nothing taken: step past the false sync
            --in_len;
        }
    }
    return 0;
}

/// Core 1's loop: one frame decoded whenever the PCM ring has room for it.
static void radio_decode_job(void) {
    int s = state;
    bool want = s == RADIO_PLAYING || (s == RADIO_BUFFERING && (ring_count() >= PREBUFFER || ended));
    if (!want || PCM_FRAMES - (pcm_in - pcm_out) < FRAME_MAX)
        return;
    uint32_t t0 = time_us_32();
    int n = decode_frame();
    for (int i = 0; i < n; ++i) {
        uint32_t at = (pcm_in + i) % PCM_FRAMES;
        pcm_ring[2 * at] = pcm[2 * i];
        pcm_ring[2 * at + 1] = pcm[2 * i + 1];
    }
    __dmb();
    pcm_in += n;
    busy_us += time_us_32() - t0;
}

/// The audio source (core 1's refill interrupt): one 48 kHz stereo frame.
/// @param l set to the left sample
/// @param r set to the right
/// @return true
static bool radio_source(int32_t *l, int32_t *r) {
    uint32_t have = pcm_in - pcm_out;
    int s = state;
    if (s == RADIO_BUFFERING && (have >= PCM_START || (ended && have >= 2)))
        state = s = RADIO_PLAYING;
    if (s != RADIO_PLAYING) {
        *l = *r = 0;
        level_l = level_r = 0;
        return true;
    }
    uint32_t i = pos >> 16;
    if (i + 1 >= have) {
        // Dry: wait for the buffer again (or it's the end).
        if (ended && ring_count() < 4 && !in_len)
            state = RADIO_ENDED;
        else {
            state = RADIO_BUFFERING;
            ++underruns;
        }
        *l = *r = 0;
        return true;
    }
    uint32_t a = (pcm_out + i) % PCM_FRAMES, b = (pcm_out + i + 1) % PCM_FRAMES;
    int f = pos & 0xffff;
    int al = pcm_ring[2 * a], ar = pcm_ring[2 * a + 1], bl = pcm_ring[2 * b], br = pcm_ring[2 * b + 1];
    int vl = al + (((bl - al) * f) >> 16), vr = ar + (((br - ar) * f) >> 16);
    pos += step;
    if (pos >> 16) {
        pcm_out += pos >> 16;
        pos &= 0xffff;
    }
    // The mixer halves sources: twice the sample, times the volume.
    int v = muted ? 0 : volume;
    *l = vl * v / 50;
    *r = vr * v / 50;
    return true;
}

// ---- Starting and stopping ---------------------------------------------------------------------------

/// Frees the decoder and its buffers.
static void free_decoder(void) {
    if (helix_pool && !decoder_sram)
        psram_free(helix_pool);
    helix_pool = NULL;
    decoder = NULL;
    psram_free(inbuf);
    inbuf = NULL;
    psram_free(pcm);
    pcm = NULL;
    psram_free(pcm_ring);
    pcm_ring = NULL;
}

void radio_stop(void) {
    if (job_on) {
        if (core1_job_now() == radio_decode_job)
            core1_job_stop();
        job_on = false;
    }
    if (audio_get_source() == radio_source) {
        audio_set_source(NULL);
        sleep_ms(3);  // core 1 may be in the middle of a frame
    }
    if (pump_on) {
        cyw43_arch_lwip_begin();
        pump_on = false;
        async_context_remove_at_time_worker(cyw43_arch_async_context(), &pump_worker);
        cyw43_arch_lwip_end();
    }
    s_close(&conn);
    free_decoder();
    psram_free(ring);
    ring = NULL;
    state = RADIO_OFF;
    level_l = level_r = 0;
}

bool radio_active(void) {
    int s = state;
    return s == RADIO_CONNECTING || s == RADIO_BUFFERING || s == RADIO_PLAYING;
}

/// Stops with an error.
/// @param why the error
/// @return false
static bool give_up(const char *why) {
    s_close(&conn);
    free_decoder();
    psram_free(ring);
    ring = NULL;
    snprintf(info.error, sizeof info.error, "%s", why);
    state = RADIO_ERROR;
    return false;
}

bool radio_play(const char *url_text, const char *name) {
    radio_stop();
    memset(&info, 0, sizeof info);
    snprintf(info.url, sizeof info.url, "%s", url_text);
    snprintf(info.station, sizeof info.station, "%s", name && *name ? name : url_text);
    state = RADIO_CONNECTING;
    if (!net_need_link())
        return give_up("WiFi isn't connected");
    char *hdr = psram_malloc(HEADER_MAX + 2 * HEADER_MAX);
    if (!hdr)
        return give_up("not enough memory");
    char *body = hdr + HEADER_MAX;
    url_t u, next;
    if (!parse_url(url_text, &u, NULL)) {
        psram_free(hdr);
        return give_up("only http:// and https:// addresses work");
    }
    int body_len = 0;
    for (int hop = 0;; ++hop) {
        if (hop > MAX_REDIRECTS) {
            psram_free(hdr);
            return give_up("too many redirects");
        }
        if (!s_open(&conn, &u)) {
            psram_free(hdr);
            return give_up("couldn't connect");
        }
        body_len = request(&conn, &u, true, hdr, body, 2 * HEADER_MAX - 1);
        if (body_len < 0) {
            psram_free(hdr);
            return give_up(fail);
        }
        int code = status_code(hdr);
        char value[512] = "", type[64] = "";
        header(hdr, "Content-Type", type, sizeof type);
        for (char *c = type; *c; ++c)
            *c = tolower((unsigned char)*c);
        if (code >= 300 && code < 400 && header(hdr, "Location", value, sizeof value)) {
            s_close(&conn);
            if (!parse_url(value, &next, &u)) {
                psram_free(hdr);
                return give_up("a redirect to an address that isn't http(s)");
            }
            u = next;
            continue;
        }
        if (code != 200) {
            char why[64];
            snprintf(why, sizeof why, "the server said %d", code);
            psram_free(hdr);
            return give_up(why);
        }
        if (is_playlist(type, u.path)) {
            // A playlist: read it all, then go to its first stream.
            absolute_time_t until = make_timeout_time_ms(IDLE_MS);
            for (;;) {
                int n = s_read(&conn, body + body_len, 2 * HEADER_MAX - 1 - body_len);
                if (n < 0 || body_len >= 2 * HEADER_MAX - 1 || time_reached(until))
                    break;
                body_len += n;
                if (!n)
                    sleep_ms(2);
            }
            body[body_len] = 0;
            s_close(&conn);
            if (strstr(body, "#EXT-X-")) {
                psram_free(hdr);
                return give_up("an HLS stream: those don't play");
            }
            if (!playlist_url(body, value, sizeof value) || !parse_url(value, &next, &u)) {
                psram_free(hdr);
                return give_up("an empty playlist");
            }
            u = next;
            continue;
        }
        // MP3 or AAC (ADTS): by the type, or failing that by the data.
        if (strstr(type, "aac")) {  // audio/aac, audio/aacp, audio/x-aac
            codec = CODEC_AAC;
        } else if (strstr(type, "mpeg") || strstr(type, "mp3")) {
            codec = CODEC_MP3;
        } else if (strstr(type, "mp4") || strstr(type, "ogg") || strstr(type, "flac") ||
                   strstr(type, "opus")) {
            char why[96];
            snprintf(why, sizeof why, "it sends %s: only MP3 and AAC streams play", type);
            psram_free(hdr);
            return give_up(why);
        } else {
            absolute_time_t until = make_timeout_time_ms(5000);
            int c;
            while ((c = sniff_codec((const uint8_t *)body, body_len)) < 0 && body_len < 2 * HEADER_MAX - 1 &&
                   !time_reached(until)) {
                int n = s_read(&conn, body + body_len, 2 * HEADER_MAX - 1 - body_len);
                if (n < 0)
                    break;
                body_len += n;
                if (!n)
                    sleep_ms(2);
            }
            codec = c < 0 ? CODEC_MP3 : c;
        }
        strcpy(info.codec, codec == CODEC_AAC ? "AAC" : "MP3");
        if (header(hdr, "icy-name", value, sizeof value) && (!name || !*name))
            snprintf(info.station, sizeof info.station, "%s", value);
        if (header(hdr, "icy-br", value, sizeof value))
            info.bitrate = atoi(value);
        metaint = header(hdr, "icy-metaint", value, sizeof value) ? atoi(value) : 0;
        break;
    }
    // The buffer, the decoder and the worker.
    ring = psram_malloc(RING_SIZE + 2048 + 512);
    meta = ring ? (char *)ring + RING_SIZE + 2048 : NULL;
    inbuf = psram_malloc(INBUF_SIZE);
    pcm = psram_malloc(FRAME_MAX * 2 * sizeof *pcm);
    pcm_ring = psram_malloc(PCM_FRAMES * 2 * sizeof *pcm_ring);
    helix_pool = app_overlay_claim(APP_OV_RADIO, &helix_size, radio_stop);
    decoder_sram = helix_size >= 24 * 1024;
    if (!decoder_sram) {  // (if the area were ever smaller: PSRAM, slowly)
        helix_size = 25 * 1024;
        helix_pool = psram_malloc(helix_size);
    }
    helix_used = 0;
    if (ring && inbuf && pcm && pcm_ring && helix_pool)
        decoder = codec == CODEC_AAC ? AACInitDecoder() : MP3InitDecoder();
    if (!decoder) {
        psram_free(hdr);
        return give_up("not enough memory");
    }
    info.decoder_in_sram = decoder_sram;
    info.decoder_sram_kb = decoder_sram ? 24 : 0;
    ring_in = ring_out = 0;
    in_len = in_at = 0;
    pcm_in = pcm_out = 0;
    pos = 0;
    step = 1 << 16;
    underruns = 0;
    ended = false;
    audio_left = metaint;
    meta_left = 0;
    meta_len = -1;
    feed((const uint8_t *)body, body_len);
    psram_free(hdr);
    last_data_ms = to_ms_since_boot(get_absolute_time());
    state = RADIO_BUFFERING;
    audio_set_source(radio_source);
    core1_job_start(radio_decode_job);
    job_on = true;
    cyw43_arch_lwip_begin();
    pump_on = true;
    pump_worker.do_work = pump;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &pump_worker, PUMP_MS);
    cyw43_arch_lwip_end();
    return true;
}

void radio_get_status(radio_status_t *st) {
    // Another program took the sound: stop for good.
    if (radio_active() && state != RADIO_CONNECTING && audio_get_source() != radio_source) {
        radio_stop();
        snprintf(info.error, sizeof info.error, "another program took the sound");
        state = RADIO_ENDED;
    }
    *st = info;
    st->state = state;
    uint32_t have = ring_count();
    st->buffer_pct = ring ? (int)((uint64_t)have * 100 / RING_SIZE) : 0;
    int br = info.bitrate ? info.bitrate : 128;
    st->buffer_ms = (int)((uint64_t)have * 8 / br);
    st->underruns = underruns;
    st->level[0] = level_l > 64 ? 64 : level_l;
    st->level[1] = level_r > 64 ? 64 : level_r;
    static uint32_t last_us, last_busy;
    uint32_t now = time_us_32(), b = busy_us;
    if (now - last_us > 1000000) {  // (decoding comes in bursts as the PCM ring fills)
        info.load = (unsigned)((uint64_t)(b - last_busy) * 1000 / (now - last_us));
        last_us = now;
        last_busy = b;
    }
    st->load = info.load;
}

void radio_set_volume(int percent) {
    volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

int radio_volume(void) {
    return volume;
}

void radio_set_mute(bool on) {
    muted = on;
}

bool radio_muted(void) {
    return muted;
}

int radio_http_get(const char *url_text, char *out, int max, const char **err) {
    url_t u, next;
    stream_t s = {0};
    char *hdr = psram_malloc(HEADER_MAX);
    if (!hdr) {
        *err = "not enough memory";
        return -1;
    }
    if (!parse_url(url_text, &u, NULL)) {
        psram_free(hdr);
        *err = "a bad address";
        return -1;
    }
    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        if (!s_open(&s, &u)) {
            psram_free(hdr);
            *err = "couldn't connect";
            return -1;
        }
        int len = request(&s, &u, false, hdr, out, max - 1);
        if (len < 0) {
            s_close(&s);
            psram_free(hdr);
            *err = fail;
            return -1;
        }
        int code = status_code(hdr);
        char loc[512];
        if (code >= 300 && code < 400 && header(hdr, "Location", loc, sizeof loc) &&
            parse_url(loc, &next, &u)) {
            s_close(&s);
            u = next;
            continue;
        }
        if (code != 200) {
            s_close(&s);
            psram_free(hdr);
            *err = "the server refused";
            return -1;
        }
        absolute_time_t until = make_timeout_time_ms(IDLE_MS);
        for (;;) {
            if (net_stop_key()) {
                len = -1;
                *err = "stopped";
                break;
            }
            int n = len < max - 1 ? s_read(&s, out + len, max - 1 - len) : -1;
            if (n < 0)
                break;
            if (!n) {
                if (time_reached(until)) {
                    *err = "the reply stopped coming";
                    len = -1;
                    break;
                }
                sleep_ms(2);
                continue;
            }
            len += n;
            until = make_timeout_time_ms(IDLE_MS);
        }
        s_close(&s);
        psram_free(hdr);
        if (len >= 0)
            out[len] = 0;
        return len;
    }
    psram_free(hdr);
    *err = "too many redirects";
    return -1;
}

// ---- The mute hot key ------------------------------------------------------------------------

#include "dvi.h"
#include "font8x16.h"

/// The "Radio muted" note should be shown.
static volatile bool note_wanted;

void radio_hotkey_mute(void) {
    if (!radio_active())
        return;
    muted = !muted;
    note_wanted = true;
}

/// The note's width.
#define NOTE_W (14 * 8 + 8)
/// Its height.
#define NOTE_H 20
/// Its left edge.
#define NOTE_X (DVI_WIDTH - NOTE_W - 4)
/// Its top.
#define NOTE_Y 4

void radio_note_poll(void) {
    static uint8_t *under;  // what the note covers, while it's up (PSRAM)
    static uint32_t until;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (under && (note_wanted || (int32_t)(now - until) >= 0)) {
        for (int r = 0; r < NOTE_H; ++r)
            memcpy(dvi_framebuf + (NOTE_Y + r) * DVI_WIDTH + NOTE_X, under + r * NOTE_W, NOTE_W);
        psram_free(under);
        under = NULL;
    }
    if (!note_wanted)
        return;
    note_wanted = false;
    if (!(under = psram_malloc(NOTE_W * NOTE_H)))
        return;
    for (int r = 0; r < NOTE_H; ++r)
        memcpy(under + r * NOTE_W, dvi_framebuf + (NOTE_Y + r) * DVI_WIDTH + NOTE_X, NOTE_W);
    const char *text = muted ? " Radio muted " : " Radio on    ";
    uint8_t bg = muted ? 0xa0 : 0x0c, fg = 0xff;
    for (int r = 0; r < NOTE_H; ++r)
        memset(dvi_framebuf + (NOTE_Y + r) * DVI_WIDTH + NOTE_X, r == 0 || r == NOTE_H - 1 ? fg : bg, NOTE_W);
    for (int i = 0; text[i]; ++i) {
        const uint8_t *g = font8x16[(uint8_t)text[i]];
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x)
                dvi_framebuf[(NOTE_Y + 2 + y) * DVI_WIDTH + NOTE_X + 4 + i * 8 + x] = g[y] & (0x80 >> x) ? fg : bg;
    }
    until = now + 1500;
}
