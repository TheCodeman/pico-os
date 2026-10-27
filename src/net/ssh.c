/**
 * @file ssh.c
 * @brief A small SSH-2 client (RFC 4251-4254, 5656, 8731, 8332): see ssh.h.
 *
 * One session channel: a command for scp, or a terminal session (pty and
 * shell) for the ssh command. All the
 * crypto is mbedTLS (X25519 and P-256 key exchange, ECDSA and RSA host keys,
 * AES-CTR, HMAC-SHA-256). Packets are built in place in the send buffer and
 * decrypted in place in the receive buffer; channel data is copied into a
 * ring whose free space is the window we give the server, so it can never
 * send more than we can hold.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "ssh.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

/// Our version line.
#define CLIENT_VERSION  "SSH-2.0-PicoOS_1.0"
#define RX_MAX          (20 * 1024)  ///< biggest packet we accept
#define TX_MAX          (17 * 1024)  ///< biggest packet we send
#define DATA_MAX        16384        ///< channel data per packet, both ways
#define INQ_SIZE        (32 * 1024)  ///< received channel data (also our window)
/// How long to wait for the server.
#define TIMEOUT_MS      30000
/// Host keys we have seen.
#define KNOWN_HOSTS     "/SSH/KNOWN_HOSTS"

/// SSH message numbers.
enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
    MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6, MSG_KEXINIT = 20, MSG_NEWKEYS = 21,
    MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31, MSG_USERAUTH_REQUEST = 50,
    MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52, MSG_USERAUTH_BANNER = 53,
    MSG_USERAUTH_INFO_REQUEST = 60, MSG_USERAUTH_INFO_RESPONSE = 61,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82, MSG_CHANNEL_OPEN = 90,
    MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
    MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
    MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};

/// One direction's cipher and MAC.
typedef struct {
    bool on;  ///< keys are in use
    mbedtls_aes_context aes;  ///< AES-CTR
    /// The counter and its key stream.
    ///@{
    uint8_t counter[16], stream[16];
    ///@}
    size_t stream_off;  ///< bytes of the stream used
    mbedtls_md_context_t mac;  ///< HMAC-SHA256
    uint32_t seq;  ///< the packet sequence number
} dir_t;

/// An SSH connection.
struct ssh {
    net_conn_t *conn;  ///< the TCP connection
    /// The receive and send buffers (PSRAM).
    ///@{
    uint8_t *rx, *tx;
    ///@}
    /// The two directions' ciphers.
    ///@{
    dir_t in, out;
    ///@}
    uint8_t session_id[32];  ///< the first exchange hash
    bool have_session_id;  ///< session_id is set
    /// The channel.
    ///@{
    uint32_t remote_id, remote_window, remote_max;
    ///@}
    /// Open; the server sent EOF; it sent close; we sent close.
    ///@{
    bool open, got_eof, got_close, sent_close;
    ///@}
    bool dead;             ///< the connection failed: don't try to say goodbye
    int exit_status;  ///< the remote program's exit status
    uint8_t *inq;          ///< received data waiting for ssh_read
    /// Where the waiting data starts, and its length.
    ///@{
    size_t inq_head, inq_len;
    ///@}
    size_t window_given;   ///< bytes the server may still send us
    int reply;             ///< channel request answer: 0 none yet, 1 ok, -1 refused
    bool interactive;      ///< a terminal session: stderr joins stdout, keys aren't "stop"
    int peeked;            ///< a byte read ahead by ssh_poll, else -1
    char exit_signal[16];  ///< the signal that ended the command, if one did
};

// ---- Random numbers ---------------------------------------------------------------

/// The random number generator.
static mbedtls_ctr_drbg_context drbg;

/// Seeds the generator, once.
/// @return it is ready
static bool rng_ready(void) {
    static bool ready;
    static mbedtls_entropy_context entropy;
    if (!ready) {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        ready = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                      (const uint8_t *)"pico-os ssh", 11) == 0;
    }
    return ready;
}

/// mbedTLS's random callback.
/// @param ctx unused
/// @param buf where
/// @param len how many
/// @return 0, or an error
static int rng(void *ctx, unsigned char *buf, size_t len) {
    return mbedtls_ctr_drbg_random(&drbg, buf, len);
}

// ---- Building and reading messages ------------------------------------------------

/// A message being built.
typedef struct {
    uint8_t *p;  ///< the bytes
    /// Its length and room.
    ///@{
    size_t len, cap;
    ///@}
    bool full;  ///< it overflowed
} out_t;

/// Adds bytes.
/// @param o the message
/// @param d them
/// @param n how many
static void put_bytes(out_t *o, const void *d, size_t n) {
    if (o->len + n > o->cap) {
        o->full = true;
        return;
    }
    memcpy(o->p + o->len, d, n);
    o->len += n;
}

/// Adds a byte.
/// @param o the message
/// @param v it
static void put_u8(out_t *o, uint8_t v) {
    put_bytes(o, &v, 1);
}

/// Adds a big-endian uint32.
/// @param o the message
/// @param v it
static void put_u32(out_t *o, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    put_bytes(o, b, 4);
}

/// Adds a string: its length, then it.
/// @param o the message
/// @param d its bytes
/// @param n their count
static void put_string(out_t *o, const void *d, size_t n) {
    put_u32(o, n);
    put_bytes(o, d, n);
}

/// Adds a C string.
/// @param o the message
/// @param s it
static void put_cstring(out_t *o, const char *s) {
    put_string(o, s, strlen(s));
}

/// A message being read.
typedef struct {
    const uint8_t *p;  ///< the next byte
    size_t left;  ///< bytes left
    bool bad;  ///< it was too short
} in_t;

/// Reads a byte.
/// @param i the message
/// @return it (0 if none)
static uint8_t get_u8(in_t *i) {
    if (i->left < 1) {
        i->bad = true;
        return 0;
    }
    --i->left;
    return *i->p++;
}

/// Reads a big-endian uint32.
/// @param i the message
/// @return it (0 if none)
static uint32_t get_u32(in_t *i) {
    if (i->left < 4) {
        i->bad = true;
        i->left = 0;
        return 0;
    }
    uint32_t v = (uint32_t)i->p[0] << 24 | i->p[1] << 16 | i->p[2] << 8 | i->p[3];
    i->p += 4;
    i->left -= 4;
    return v;
}

/// A string's bytes (not NUL-terminated) and length.
/// @param i the message
/// @param len set to its length
/// @return its bytes
static const uint8_t *get_string(in_t *i, size_t *len) {
    uint32_t n = get_u32(i);
    if (n > i->left) {
        i->bad = true;
        i->left = 0;
        *len = 0;
        return (const uint8_t *)"";
    }
    const uint8_t *s = i->p;
    i->p += n;
    i->left -= n;
    *len = n;
    return s;
}

/// Whether a string (not NUL-terminated) is want.
/// @param s its bytes
/// @param n their count
/// @param want the text
/// @return it is
static bool str_is(const uint8_t *s, size_t n, const char *want) {
    return n == strlen(want) && memcmp(s, want, n) == 0;
}

/// Prints a string from the server, leaving out control characters.
/// @param s its bytes
/// @param n their count
static void print_text(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] >= 0x20 || s[i] == '\n')
            putchar(s[i]);
}

// ---- Hashing helpers ----------------------------------------------------------------

/// Hashes a string: its length, then it.
/// @param h the hash
/// @param d its bytes
/// @param n their count
static void hash_string(mbedtls_sha256_context *h, const void *d, size_t n) {
    uint8_t b[4] = {n >> 24, n >> 16, n >> 8, n};
    mbedtls_sha256_update(h, b, 4);
    mbedtls_sha256_update(h, d, n);
}

/// `n` big-endian bytes as an mpint (a positive number).
/// @param h the hash
/// @param d the bytes
/// @param n their count
static void hash_mpint(mbedtls_sha256_context *h, const uint8_t *d, size_t n) {
    while (n && !*d) {
        ++d;
        --n;
    }
    uint8_t zero = 0;
    bool pad = n && (d[0] & 0x80);
    uint32_t len = n + pad;
    uint8_t b[4] = {len >> 24, len >> 16, len >> 8, len};
    mbedtls_sha256_update(h, b, 4);
    if (pad)
        mbedtls_sha256_update(h, &zero, 1);
    mbedtls_sha256_update(h, d, n);
}

// ---- Packets ----------------------------------------------------------------------

/// Whether the user pressed Ctrl-C or Esc (not in a terminal session).
/// @param s the connection
/// @return stop
static bool stop_requested(ssh_t *s) {
    if (s->interactive)
        return false;  // Ctrl-C and Esc belong to the remote program
    if (net_stop_key()) {
        printf("\nStopped\n");
        return true;
    }
    return false;
}

/// Reads exactly n bytes, waiting up to the timeout.
/// @param s the connection
/// @param buf where
/// @param n how many
/// @return read
static bool read_full(ssh_t *s, uint8_t *buf, size_t n) {
    uint64_t last = time_us_64();
    if (n && s->peeked >= 0) {
        *buf++ = s->peeked;
        s->peeked = -1;
        --n;
    }
    while (n) {
        int got = net_conn_read(s->conn, buf, n);
        if (got < 0) {
            printf("\nThe server closed the connection\n");
            s->dead = true;
            return false;
        }
        if (got > 0) {
            buf += got;
            n -= got;
            last = time_us_64();
            continue;
        }
        if (time_us_64() - last > TIMEOUT_MS * 1000ull) {
            printf("\nThe server stopped answering\n");
            s->dead = true;
            return false;
        }
        if (stop_requested(s))
            return false;
        sleep_ms(1);
    }
    return true;
}

/// Encrypts or decrypts in place.
/// @param d the direction
/// @param buf the bytes
/// @param n their count
static void crypt(dir_t *d, uint8_t *buf, size_t n) {
    mbedtls_aes_crypt_ctr(&d->aes, n, &d->stream_off, d->counter, d->stream, buf, buf);
}

/// A packet's MAC.
/// @param d the direction
/// @param packet the packet
/// @param n its length
/// @param out the MAC
static void mac_of(dir_t *d, const uint8_t *packet, size_t n, uint8_t out[32]) {
    uint8_t seq[4] = {d->seq >> 24, d->seq >> 16, d->seq >> 8, d->seq};
    mbedtls_md_hmac_reset(&d->mac);
    mbedtls_md_hmac_update(&d->mac, seq, 4);
    mbedtls_md_hmac_update(&d->mac, packet, n);
    mbedtls_md_hmac_finish(&d->mac, out);
}

/// The send buffer's payload area, for building the next packet.
/// @param s the connection
/// @param type the message type
/// @return the message
static out_t start_msg(ssh_t *s, uint8_t type) {
    out_t o = {s->tx + 5, 0, TX_MAX - 5 - 16 - 32, false};
    put_u8(&o, type);
    return o;
}

/// Pads, MACs, encrypts and sends the packet built with start_msg.
/// @param s the connection
/// @param o the message
/// @return sent
static bool send_msg(ssh_t *s, out_t *o) {
    if (o->full) {
        printf("\nssh: message too big\n");
        return false;
    }
    size_t bs = s->out.on ? 16 : 8;
    size_t pad = bs - (5 + o->len) % bs;
    if (pad < 4)
        pad += bs;
    uint32_t plen = 1 + o->len + pad;
    uint8_t *t = s->tx;
    t[0] = plen >> 24;
    t[1] = plen >> 16;
    t[2] = plen >> 8;
    t[3] = plen;
    t[4] = pad;
    rng(NULL, t + 5 + o->len, pad);
    size_t total = 4 + plen;
    if (s->out.on) {
        mac_of(&s->out, t, total, t + total);
        crypt(&s->out, t, total);
        total += 32;
    }
    ++s->out.seq;
    if (s->dead)
        return false;
    if (!net_conn_write(s->conn, t, total)) {
        printf("\nThe connection was lost\n");
        s->dead = true;
        return false;
    }
    return true;
}

static bool handle_channel_msg(ssh_t *s, uint8_t type, in_t *m);

/// Receives the next packet, dealing with transport messages (and, when
/// `channel` is set, channel housekeeping) itself. Returns the message type
/// with *m positioned after it, or -1 on failure.
/// @param s the connection
/// @param m set to the message
/// @return the type, or -1
static int recv_msg(ssh_t *s, in_t *m) {
    for (;;) {
        size_t bs = s->in.on ? 16 : 8;
        uint8_t *r = s->rx;
        if (!read_full(s, r, bs))
            return -1;
        if (s->in.on)
            crypt(&s->in, r, bs);
        uint32_t plen = (uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
        size_t total = 4 + plen;
        if (plen < 5 || total > RX_MAX - 32 || total % bs) {
            printf("\nssh: bad packet from the server (length %lu)\n", (unsigned long)plen);
            return -1;
        }
        if (!read_full(s, r + bs, total - bs))
            return -1;
        if (s->in.on) {
            crypt(&s->in, r + bs, total - bs);
            uint8_t want[32];
            if (!read_full(s, r + total, 32))
                return -1;
            mac_of(&s->in, r, total, want);
            uint8_t diff = 0;
            for (int i = 0; i < 32; ++i)
                diff |= want[i] ^ r[total + i];
            if (diff) {
                printf("\nssh: corrupted packet (bad MAC)\n");
                return -1;
            }
        }
        ++s->in.seq;
        uint8_t pad = r[4];
        if (pad + 1u >= plen) {
            printf("\nssh: bad padding\n");
            return -1;
        }
        m->p = r + 5;
        m->left = plen - pad - 1;
        m->bad = false;
        uint8_t type = get_u8(m);
        switch (type) {
        case MSG_IGNORE:
        case MSG_DEBUG:
        case MSG_UNIMPLEMENTED:
            continue;
        case MSG_DISCONNECT: {
            get_u32(m);
            size_t n;
            const uint8_t *why = get_string(m, &n);
            printf("\nThe server disconnected: ");
            print_text(why, n);
            printf("\n");
            return -1;
        }
        case MSG_GLOBAL_REQUEST: {
            size_t n;
            get_string(m, &n);
            if (get_u8(m)) {  // wants a reply
                out_t o = start_msg(s, MSG_REQUEST_FAILURE);
                if (!send_msg(s, &o))
                    return -1;
            }
            continue;
        }
        case MSG_KEXINIT:
            if (s->have_session_id) {
                printf("\nssh: the server wants new keys; that isn't supported\n");
                return -1;
            }
            return type;
        default:
            if (type >= MSG_CHANNEL_WINDOW_ADJUST && type <= MSG_CHANNEL_FAILURE &&
                type != MSG_CHANNEL_OPEN_CONFIRMATION && type != MSG_CHANNEL_OPEN_FAILURE) {
                if (!handle_channel_msg(s, type, m))
                    return -1;
                return 0;  // dealt with: the caller loops if it wants more
            }
            return type;
        }
    }
}

/// Waits for a message of type `want` (other channel traffic is dealt with on
/// the way). -1 on failure; prints `what` if something else arrives.
/// @param s the connection
/// @param m set to the message
/// @param want the type
/// @param what what we were doing, for the error
/// @return the type, or -1
static int expect_msg(ssh_t *s, in_t *m, int want, const char *what) {
    for (;;) {
        int t = recv_msg(s, m);
        if (t < 0)
            return -1;
        if (t == 0)
            continue;
        if (t == want)
            return t;
        printf("\nssh: unexpected message %d while %s\n", t, what);
        return -1;
    }
}

// ---- Key exchange -------------------------------------------------------------------

/// Our key exchanges, best first.
static const char *const kex_algs[] = {"curve25519-sha256", "curve25519-sha256@libssh.org",
                                       "ecdh-sha2-nistp256", NULL};
/// Our host key types, best first.
static const char *const key_algs[] = {"ecdsa-sha2-nistp256", "rsa-sha2-256", "rsa-sha2-512",
                                       NULL};
/// Our ciphers, best first.
static const char *const cipher_algs[] = {"aes128-ctr", "aes256-ctr", NULL};
/// Our MACs.
static const char *const mac_algs[] = {"hmac-sha2-256", NULL};

/// Adds a name-list.
/// @param o the message
/// @param names the names, NULL last
static void put_namelist(out_t *o, const char *const *names) {
    char buf[256];
    size_t n = 0;
    for (int i = 0; names[i]; ++i)
        n += snprintf(buf + n, sizeof buf - n, "%s%s", i ? "," : "", names[i]);
    put_string(o, buf, n);
}

/// The first of our `names` that is in the server's comma-separated list.
/// @param names ours, NULL last
/// @param list the server's
/// @param len its length
/// @return our index, or -1
static int choose(const char *const *names, const uint8_t *list, size_t len) {
    for (int i = 0; names[i]; ++i) {
        size_t w = strlen(names[i]);
        for (size_t p = 0; p < len;) {
            size_t e = p;
            while (e < len && list[e] != ',')
                ++e;
            if (e - p == w && memcmp(list + p, names[i], w) == 0)
                return i;
            p = e + 1;
        }
    }
    return -1;
}

/// Whether the server's list has a name.
/// @param list the list
/// @param len its length
/// @param name the name
/// @return it has
static bool list_has(const uint8_t *list, size_t len, const char *name) {
    const char *names[] = {name, NULL};
    return choose(names, list, len) >= 0;
}

/// Base64 of SHA-256(blob), no padding: OpenSSH's "SHA256:..." fingerprint.
/// @param blob the key
/// @param n its length
/// @param out the fingerprint
static void fingerprint(const uint8_t *blob, size_t n, char out[48]) {
    uint8_t h[32];
    mbedtls_sha256(blob, n, h, 0);
    size_t olen;
    mbedtls_base64_encode((uint8_t *)out, 48, &olen, h, 32);
    while (olen && out[olen - 1] == '=')
        --olen;
    out[olen] = 0;
}

/// Waits for y or n. False for n, Ctrl-C or Esc.
/// @return y
static bool ask_yes(void) {
    for (;;) {
        int c = getchar_timeout_us(100000);
        if (c == 'y' || c == 'Y') {
            printf("y\n");
            return true;
        }
        if (c == 'n' || c == 'N' || c == 3 || c == 0x1b) {
            printf("n\n");
            return false;
        }
    }
}

/// Checks the host key against KNOWN_HOSTS, asking about a new one.
/// @param host the host
/// @param port its port
/// @param type the key's type
/// @param blob the key
/// @param n its length
/// @return accepted
static bool check_host_key(const char *host, uint16_t port, const char *type,
                           const uint8_t *blob, size_t n) {
    char fp[48], name[160];
    fingerprint(blob, n, fp);
    if (port == 22)
        snprintf(name, sizeof name, "%s", host);
    else
        snprintf(name, sizeof name, "[%s]:%u", host, port);
    for (char *c = name; *c; ++c)
        *c = tolower((unsigned char)*c);
    FIL f;
    bool other_types = false;
    if (f_open(&f, KNOWN_HOSTS, FA_READ) == FR_OK) {
        char line[300];
        while (f_gets(line, sizeof line, &f)) {
            char h[160], t[40], k[60];
            if (sscanf(line, "%159s %39s %59s", h, t, k) != 3 || strcmp(h, name))
                continue;
            if (strcmp(t, type)) {
                other_types = true;
                continue;
            }
            f_close(&f);
            if (strncmp(k, "SHA256:", 7) == 0 && strcmp(k + 7, fp) == 0)
                return true;
            printf("\n@@@ WARNING: THE HOST KEY FOR %s HAS CHANGED! @@@\n"
                   "Someone could be eavesdropping on you (man-in-the-middle), or the\n"
                   "server's key was replaced. Its %s key is now\n"
                   "  SHA256:%s\n"
                   "but " KNOWN_HOSTS " has\n"
                   "  %s\n"
                   "Not connecting. If the change is expected, delete the server's line\n"
                   "from " KNOWN_HOSTS ".\n",
                   name, type, fp, k);
            return false;
        }
        f_close(&f);
    }
    printf("The authenticity of host '%s' can't be established.\n", name);
    if (other_types)
        printf("(A different type of key is saved for it.)\n");
    printf("Its %s key fingerprint is\n  SHA256:%s\n", type, fp);
    printf("Trust it and save it in " KNOWN_HOSTS " (y/n)? ");
    fflush(stdout);
    if (!ask_yes()) {
        printf("Not connecting.\n");
        return false;
    }
    f_mkdir("/SSH");  // fine if it already exists
    if (f_open(&f, KNOWN_HOSTS, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
        printf("Can't write " KNOWN_HOSTS " (connecting anyway)\n");
        return true;
    }
    f_printf(&f, "%s %s SHA256:%s\n", name, type, fp);
    f_close(&f);
    return true;
}

/// Checks the server's signature `sig` on the exchange hash with key `blob`.
/// @param alg the signature's algorithm
/// @param blob the key
/// @param blob_len its length
/// @param sig the signature
/// @param sig_len its length
/// @param hash the exchange hash
/// @return good
static bool verify_host_sig(const char *alg, const uint8_t *blob, size_t blob_len,
                            const uint8_t *sig, size_t sig_len, const uint8_t hash[32]) {
    in_t k = {blob, blob_len, false}, g = {sig, sig_len, false};
    size_t n, sn;
    const uint8_t *ktype = get_string(&k, &n);
    const uint8_t *stype = get_string(&g, &sn);
    const uint8_t *sdata = get_string(&g, &sn);
    if (k.bad || g.bad)
        return false;
    bool ok = false;
    if (!strcmp(alg, "ecdsa-sha2-nistp256")) {
        if (!str_is(ktype, n, alg) || !str_is(stype, strlen(alg), alg))
            return false;
        size_t cn, qn, rn, sn2;
        get_string(&k, &cn);
        const uint8_t *q = get_string(&k, &qn);
        in_t rs = {sdata, sn, false};
        const uint8_t *r = get_string(&rs, &rn);
        const uint8_t *sv = get_string(&rs, &sn2);
        if (k.bad || rs.bad)
            return false;
        mbedtls_ecp_group grp;
        mbedtls_ecp_point Q;
        mbedtls_mpi R, S;
        mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point_init(&Q);
        mbedtls_mpi_init(&R);
        mbedtls_mpi_init(&S);
        uint8_t h2[32];
        mbedtls_sha256(hash, 32, h2, 0);
        ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
             mbedtls_ecp_point_read_binary(&grp, &Q, q, qn) == 0 &&
             mbedtls_ecp_check_pubkey(&grp, &Q) == 0 &&
             mbedtls_mpi_read_binary(&R, r, rn) == 0 && mbedtls_mpi_read_binary(&S, sv, sn2) == 0 &&
             mbedtls_ecdsa_verify(&grp, h2, 32, &Q, &R, &S) == 0;
        mbedtls_ecp_group_free(&grp);
        mbedtls_ecp_point_free(&Q);
        mbedtls_mpi_free(&R);
        mbedtls_mpi_free(&S);
    } else {
        bool sha512 = !strcmp(alg, "rsa-sha2-512");
        if (!str_is(ktype, n, "ssh-rsa") || !str_is(stype, strlen(alg), alg))
            return false;
        size_t en, nn;
        const uint8_t *e = get_string(&k, &en);
        const uint8_t *nm = get_string(&k, &nn);
        if (k.bad)
            return false;
        while (nn && !*nm) {  // mpint's sign byte
            ++nm;
            --nn;
        }
        if (nn < 256 || nn > 1024 || sn != nn)  // 2048-8192 bit keys
            return false;
        uint8_t h2[64];
        if (sha512)
            mbedtls_sha512(hash, 32, h2, 0);
        else
            mbedtls_sha256(hash, 32, h2, 0);
        mbedtls_rsa_context rsa;
        mbedtls_rsa_init(&rsa);
        ok = mbedtls_rsa_import_raw(&rsa, nm, nn, NULL, 0, NULL, 0, NULL, 0, e, en) == 0 &&
             mbedtls_rsa_complete(&rsa) == 0 &&
             mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) == 0 &&
             mbedtls_rsa_pkcs1_verify(&rsa, sha512 ? MBEDTLS_MD_SHA512 : MBEDTLS_MD_SHA256,
                                      sha512 ? 64 : 32, h2, sdata) == 0;
        mbedtls_rsa_free(&rsa);
    }
    return ok;
}

/// Key i (A-F) of RFC 4253 7.2: SHA-256(K || H || letter || session id); 32
/// bytes is all any of ours need.
/// @param k the shared secret (an mpint)
/// @param kn its length
/// @param h the exchange hash
/// @param sid the session id
/// @param letter the key's letter
/// @param out the key
static void derive(const uint8_t *k, size_t kn, const uint8_t h[32], const uint8_t sid[32],
                   char letter, uint8_t out[32]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    hash_mpint(&c, k, kn);
    mbedtls_sha256_update(&c, h, 32);
    mbedtls_sha256_update(&c, (const uint8_t *)&letter, 1);
    mbedtls_sha256_update(&c, sid, 32);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

/// Turns a direction's encryption on.
/// @param d the direction
/// @param iv the IV
/// @param key the AES key
/// @param key_bits 128 or 256
/// @param mac the MAC key
static void set_keys(dir_t *d, const uint8_t iv[32], const uint8_t key[32], int key_bits,
                     const uint8_t mac[32]) {
    mbedtls_aes_init(&d->aes);
    mbedtls_aes_setkey_enc(&d->aes, key, key_bits);
    memcpy(d->counter, iv, 16);
    d->stream_off = 0;
    mbedtls_md_init(&d->mac);
    mbedtls_md_setup(&d->mac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&d->mac, mac, 32);
    d->on = true;
}

/// Reads the server's version line (skipping any lines before it).
/// @param s the connection
/// @param v the line
/// @param max room in v
/// @return read
static bool read_version(ssh_t *s, char *v, size_t max) {
    for (int lines = 0; lines < 50; ++lines) {
        size_t n = 0;
        for (;;) {
            uint8_t c;
            if (!read_full(s, &c, 1))
                return false;
            if (c == '\n')
                break;
            if (n < max - 1)
                v[n++] = c;
        }
        if (n && v[n - 1] == '\r')
            --n;
        v[n] = 0;
        if (strncmp(v, "SSH-", 4) == 0) {
            if (strncmp(v, "SSH-2.0-", 8) && strncmp(v, "SSH-1.99-", 9)) {
                printf("The server only speaks an old SSH version (%s)\n", v);
                return false;
            }
            return true;
        }
    }
    printf("That doesn't look like an SSH server\n");
    return false;
}

/// The key exchange: versions, KEXINIT, curve25519 ECDH, host key check, NEWKEYS.
/// @param s the connection
/// @param host the host
/// @param port its port
/// @return done
static bool key_exchange(ssh_t *s, const char *host, uint16_t port) {
    char vs[256];
    static const char vc[] = CLIENT_VERSION;
    if (!net_conn_write(s->conn, CLIENT_VERSION "\r\n", sizeof vc + 1) ||
        !read_version(s, vs, sizeof vs))
        return false;

    // Our KEXINIT, kept for the exchange hash.
    out_t o = start_msg(s, MSG_KEXINIT);
    uint8_t cookie[16];
    rng(NULL, cookie, 16);
    put_bytes(&o, cookie, 16);
    put_namelist(&o, kex_algs);
    put_namelist(&o, key_algs);
    put_namelist(&o, cipher_algs);
    put_namelist(&o, cipher_algs);
    put_namelist(&o, mac_algs);
    put_namelist(&o, mac_algs);
    put_cstring(&o, "none");
    put_cstring(&o, "none");
    put_cstring(&o, "");
    put_cstring(&o, "");
    put_u8(&o, 0);
    put_u32(&o, 0);
    size_t ic_len = o.len;
    uint8_t *ic = malloc(ic_len);
    if (!ic)
        return false;
    memcpy(ic, o.p, ic_len);
    if (!send_msg(s, &o)) {
        free(ic);
        return false;
    }

    in_t m;
    if (expect_msg(s, &m, MSG_KEXINIT, "starting") < 0) {
        free(ic);
        return false;
    }
    size_t is_len = m.left + 1;
    uint8_t *is = malloc(is_len);
    if (!is) {
        free(ic);
        return false;
    }
    memcpy(is, m.p - 1, is_len);
    m.p += 16;  // cookie
    m.left -= 16;
    size_t ln[8];
    const uint8_t *lists[8];
    for (int i = 0; i < 8; ++i)
        lists[i] = get_string(&m, &ln[i]);
    bool guess_follows = get_u8(&m);
    int kex = choose(kex_algs, lists[0], ln[0]);
    int hk = choose(key_algs, lists[1], ln[1]);
    int c_cs = choose(cipher_algs, lists[2], ln[2]), c_sc = choose(cipher_algs, lists[3], ln[3]);
    int m_cs = choose(mac_algs, lists[4], ln[4]), m_sc = choose(mac_algs, lists[5], ln[5]);
    bool comp = list_has(lists[6], ln[6], "none") && list_has(lists[7], ln[7], "none");
    if (m.bad || kex < 0 || hk < 0 || c_cs < 0 || c_sc < 0 || m_cs < 0 || m_sc < 0 || !comp) {
        printf("No algorithms in common with the server (it offers kex ");
        print_text(lists[0], ln[0]);
        printf("; host keys ");
        print_text(lists[1], ln[1]);
        printf("; ciphers ");
        print_text(lists[2], ln[2]);
        printf("; MACs ");
        print_text(lists[4], ln[4]);
        printf(")\n");
        free(ic);
        free(is);
        return false;
    }
    (void)guess_follows;  // we never guess, and a server guess can't match ours here

    // ECDH.
    bool x25519 = kex < 2;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point qc, qs;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&qc);
    mbedtls_ecp_point_init(&qs);
    uint8_t qc_bin[65], k[32], h[32];
    size_t qc_len = 0;
    bool ok = mbedtls_ecp_group_load(&grp, x25519 ? MBEDTLS_ECP_DP_CURVE25519
                                                  : MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_gen_keypair(&grp, &d, &qc, rng, NULL) == 0 &&
              mbedtls_ecp_point_write_binary(&grp, &qc, MBEDTLS_ECP_PF_UNCOMPRESSED, &qc_len,
                                             qc_bin, sizeof qc_bin) == 0;
    if (ok) {
        o = start_msg(s, MSG_KEX_ECDH_INIT);
        put_string(&o, qc_bin, qc_len);
        ok = send_msg(s, &o) && expect_msg(s, &m, MSG_KEX_ECDH_REPLY, "exchanging keys") > 0;
    }
    size_t ks_len = 0, qs_len = 0, sig_len = 0;
    const uint8_t *ks = NULL, *qs_bin = NULL, *sig = NULL;
    if (ok) {
        ks = get_string(&m, &ks_len);
        qs_bin = get_string(&m, &qs_len);
        sig = get_string(&m, &sig_len);
        ok = !m.bad && mbedtls_ecp_point_read_binary(&grp, &qs, qs_bin, qs_len) == 0 &&
             mbedtls_ecdh_compute_shared(&grp, &z, &qs, &d, rng, NULL) == 0;
        if (ok && x25519)
            ok = mbedtls_mpi_write_binary_le(&z, k, 32) == 0;  // X25519's output bytes
        else if (ok)
            ok = mbedtls_mpi_write_binary(&z, k, 32) == 0;
        if (ok) {
            uint8_t any = 0;
            for (int i = 0; i < 32; ++i)
                any |= k[i];
            ok = any != 0;
        }
        if (!ok)
            printf("ssh: the key exchange failed\n");
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&qc);
    mbedtls_ecp_point_free(&qs);

    if (ok) {
        // The exchange hash H.
        mbedtls_sha256_context hc;
        mbedtls_sha256_init(&hc);
        mbedtls_sha256_starts(&hc, 0);
        hash_string(&hc, vc, strlen(vc));
        hash_string(&hc, vs, strlen(vs));
        hash_string(&hc, ic, ic_len);
        hash_string(&hc, is, is_len);
        hash_string(&hc, ks, ks_len);
        hash_string(&hc, qc_bin, qc_len);
        hash_string(&hc, qs_bin, qs_len);
        hash_mpint(&hc, k, 32);
        mbedtls_sha256_finish(&hc, h);
        mbedtls_sha256_free(&hc);
        if (!verify_host_sig(key_algs[hk], ks, ks_len, sig, sig_len, h)) {
            printf("ssh: the server's host key signature is wrong; not connecting\n");
            ok = false;
        }
    }
    free(ic);
    free(is);
    if (ok) {
        in_t kt = {ks, ks_len, false};
        size_t tn;
        const uint8_t *t = get_string(&kt, &tn);
        char type[40];
        snprintf(type, sizeof type, "%.*s", (int)tn, t);
        // The key blob lives in rx, which the next packet reuses: check now.
        ok = check_host_key(host, port, type, ks, ks_len);
    }
    if (ok) {
        memcpy(s->session_id, h, 32);
        s->have_session_id = true;
        o = start_msg(s, MSG_NEWKEYS);
        ok = send_msg(s, &o);
    }
    if (ok) {
        uint8_t iv[32], key[32], mac[32];
        int bits_cs = c_cs == 1 ? 256 : 128, bits_sc = c_sc == 1 ? 256 : 128;
        derive(k, 32, h, s->session_id, 'A', iv);
        derive(k, 32, h, s->session_id, 'C', key);
        derive(k, 32, h, s->session_id, 'E', mac);
        set_keys(&s->out, iv, key, bits_cs, mac);
        ok = expect_msg(s, &m, MSG_NEWKEYS, "exchanging keys") > 0;
        if (ok) {
            derive(k, 32, h, s->session_id, 'B', iv);
            derive(k, 32, h, s->session_id, 'D', key);
            derive(k, 32, h, s->session_id, 'F', mac);
            set_keys(&s->in, iv, key, bits_sc, mac);
        }
        mbedtls_platform_zeroize(iv, sizeof iv);
        mbedtls_platform_zeroize(key, sizeof key);
        mbedtls_platform_zeroize(mac, sizeof mac);
    }
    mbedtls_platform_zeroize(k, sizeof k);
    return ok;
}

// ---- Logging in -----------------------------------------------------------------------

/// Reads a line without showing it. False on Ctrl-C or Esc.
/// @param buf the line
/// @param max room in buf
/// @return read
static bool read_secret(char *buf, size_t max) {
    size_t n = 0;
    for (;;) {
        int c = getchar_timeout_us(100000);
        if (c < 0)
            continue;
        if (c == '\r' || c == '\n') {
            buf[n] = 0;
            printf("\n");
            return true;
        }
        if (c == 3 || c == 0x1b) {
            mbedtls_platform_zeroize(buf, max);
            printf("\n");
            return false;
        }
        if ((c == '\b' || c == 0x7f) && n)
            --n;
        else if (c >= 0x20 && c != 0x7f && n < max - 1)
            buf[n++] = c;
    }
}

/// Prints a login banner.
/// @param m the message
static void print_banner(in_t *m) {
    size_t n;
    const uint8_t *t = get_string(m, &n);
    print_text(t, n);
    if (n && t[n - 1] != '\n')
        printf("\n");
}

/// Sends a login request.
/// @param s the connection
/// @param user the user
/// @param method "none", "password" or "keyboard-interactive"
/// @param password the password
/// @return sent
static bool send_auth(ssh_t *s, const char *user, const char *method, const char *password) {
    out_t o = start_msg(s, MSG_USERAUTH_REQUEST);
    put_cstring(&o, user);
    put_cstring(&o, "ssh-connection");
    put_cstring(&o, method);
    if (!strcmp(method, "password")) {
        put_u8(&o, 0);
        put_cstring(&o, password);
    } else if (!strcmp(method, "keyboard-interactive")) {
        put_cstring(&o, "");  // language
        put_cstring(&o, "");  // submethods
    }
    bool ok = send_msg(s, &o);
    mbedtls_platform_zeroize(s->tx, TX_MAX);  // the password was in there
    return ok;
}

/// Answers a keyboard-interactive request (*m is just after its type).
/// @param s the connection
/// @param m the request
/// @return answered
static bool answer_info_request(ssh_t *s, in_t *m) {
    size_t n;
    const uint8_t *name = get_string(m, &n);
    if (n) {
        print_text(name, n);
        printf("\n");
    }
    const uint8_t *instr = get_string(m, &n);
    if (n) {
        print_text(instr, n);
        printf("\n");
    }
    get_string(m, &n);  // language
    uint32_t count = get_u32(m);
    if (m->bad || count > 4)
        return false;
    // Ask everything first: the answers are built in tx, the questions are
    // in rx.
    char answers[4][128];
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
        const uint8_t *prompt = get_string(m, &n);
        bool echo = get_u8(m);
        print_text(prompt, n);
        fflush(stdout);
        if (!echo) {
            ok = read_secret(answers[i], sizeof answers[i]);
            continue;
        }
        size_t k = 0;
        for (;;) {
            int c = getchar_timeout_us(100000);
            if (c == '\r' || c == '\n')
                break;
            if (c == 3 || c == 0x1b) {
                ok = false;
                break;
            }
            if (c >= 0x20 && c != 0x7f && k < sizeof answers[i] - 1) {
                answers[i][k++] = c;
                putchar(c);
            }
        }
        answers[i][k] = 0;
        printf("\n");
    }
    if (ok) {
        out_t o = start_msg(s, MSG_USERAUTH_INFO_RESPONSE);
        put_u32(&o, count);
        for (uint32_t i = 0; i < count; ++i)
            put_cstring(&o, answers[i]);
        ok = send_msg(s, &o);
        mbedtls_platform_zeroize(s->tx, TX_MAX);
    }
    mbedtls_platform_zeroize(answers, sizeof answers);
    return ok;
}

/// An mbedTLS number as an SSH mpint.
/// @param o the message
/// @param x the number
static void put_mpi(out_t *o, const mbedtls_mpi *x) {
    uint8_t b[520];
    size_t n = mbedtls_mpi_size(x);
    if (n + 1 > sizeof b) {
        o->full = true;
        return;
    }
    b[0] = 0;
    mbedtls_mpi_write_binary(x, b + 1, n);
    bool pad = n && (b[1] & 0x80);
    put_string(o, b + !pad, n + pad);
}

/// Our key files, tried in turn.
static const char *const key_files[] = {"/SSH/ID_ECDSA", "/SSH/ID_RSA"};

/// Reads and parses a PEM private key file. False (saying why, unless the
/// file just isn't there) if it can't be used.
/// @param path the file
/// @param pk the key
/// @return usable
static bool load_key(const char *path, mbedtls_pk_context *pk) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    size_t size = f_size(&f);
    uint8_t *buf = size < 16384 ? psram_buffer_alloc(size + 1) : NULL;
    UINT got = 0;
    bool ok = buf && f_read(&f, buf, size, &got) == FR_OK && got == size;
    f_close(&f);
    if (ok) {
        buf[size] = 0;
        ok = mbedtls_pk_parse_key(pk, buf, size + 1, NULL, 0, rng, NULL) == 0;
        if (!ok) {
            if (strstr((char *)buf, "OPENSSH PRIVATE KEY"))
                printf("%s is in OpenSSH's own format: convert it to PEM on the PC with\n"
                       "  ssh-keygen -p -m PEM -f yourkey\n", path);
            else if (strstr((char *)buf, "ENCRYPTED"))
                printf("%s has a passphrase; only keys without one are supported\n", path);
            else
                printf("Can't read the key in %s\n", path);
        }
        mbedtls_platform_zeroize(buf, size);
    } else {
        printf("Can't read %s\n", path);
    }
    psram_buffer_free(buf);
    return ok;
}

/// Sends a publickey login signed with the first usable key file. Returns 1
/// if one was sent, 0 if there's no usable key, -1 on failure.
/// @param s the connection
/// @param user the user
/// @return 1, 0 or -1
static int send_key_auth(ssh_t *s, const char *user) {
    mbedtls_pk_context pk;
    const char *path = NULL;
    for (size_t i = 0; i < sizeof key_files / sizeof *key_files && !path; ++i) {
        mbedtls_pk_init(&pk);
        if (load_key(key_files[i], &pk))
            path = key_files[i];
        else
            mbedtls_pk_free(&pk);
    }
    if (!path)
        return 0;
    bool is_ec = mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY;
    bool is_rsa = mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA;
    if ((!is_ec && !is_rsa) ||
        (is_ec && mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(pk)) != MBEDTLS_ECP_DP_SECP256R1)) {
        printf("%s: only ECDSA P-256 and RSA keys are supported\n", path);
        mbedtls_pk_free(&pk);
        return 0;
    }
    const char *alg = is_ec ? "ecdsa-sha2-nistp256" : "rsa-sha2-256";
    // The public key blob.
    uint8_t blob[600];
    out_t b = {blob, 0, sizeof blob, false};
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, sv;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&sv);
    mbedtls_ecp_point_init(&q);
    bool ok = true;
    if (is_ec) {
        uint8_t qb[65];
        size_t qn;
        ok = mbedtls_ecp_export(mbedtls_pk_ec(pk), &grp, &d, &q) == 0 &&
             mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &qn, qb,
                                            sizeof qb) == 0;
        put_cstring(&b, alg);
        put_cstring(&b, "nistp256");
        put_string(&b, qb, qn);
    } else {
        mbedtls_mpi n, e;
        mbedtls_mpi_init(&n);
        mbedtls_mpi_init(&e);
        ok = mbedtls_rsa_export(mbedtls_pk_rsa(pk), &n, NULL, NULL, NULL, &e) == 0;
        put_cstring(&b, "ssh-rsa");
        put_mpi(&b, &e);
        put_mpi(&b, &n);
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);
    }
    // What gets signed (RFC 4252 7).
    uint8_t hash[32];
    if (ok && !b.full) {
        mbedtls_sha256_context h;
        mbedtls_sha256_init(&h);
        mbedtls_sha256_starts(&h, 0);
        hash_string(&h, s->session_id, 32);
        uint8_t type = MSG_USERAUTH_REQUEST, yes = 1;
        mbedtls_sha256_update(&h, &type, 1);
        hash_string(&h, user, strlen(user));
        hash_string(&h, "ssh-connection", 14);
        hash_string(&h, "publickey", 9);
        mbedtls_sha256_update(&h, &yes, 1);
        hash_string(&h, alg, strlen(alg));
        hash_string(&h, blob, b.len);
        mbedtls_sha256_finish(&h, hash);
        mbedtls_sha256_free(&h);
    } else {
        ok = false;
    }
    out_t o = start_msg(s, MSG_USERAUTH_REQUEST);
    put_cstring(&o, user);
    put_cstring(&o, "ssh-connection");
    put_cstring(&o, "publickey");
    put_u8(&o, 1);
    put_cstring(&o, alg);
    put_string(&o, blob, b.len);
    uint8_t sig[600];
    out_t g = {sig, 0, sizeof sig, false};
    put_cstring(&g, alg);
    if (ok && is_ec) {
        uint8_t inner[80];
        out_t in = {inner, 0, sizeof inner, false};
        ok = mbedtls_ecdsa_sign(&grp, &r, &sv, &d, hash, 32, rng, NULL) == 0;
        put_mpi(&in, &r);
        put_mpi(&in, &sv);
        put_string(&g, inner, in.len);
        ok = ok && !in.full;
    } else if (ok) {
        uint8_t raw[512];
        size_t len = mbedtls_rsa_get_len(mbedtls_pk_rsa(pk));
        ok = len <= sizeof raw &&
             mbedtls_rsa_pkcs1_sign(mbedtls_pk_rsa(pk), rng, NULL, MBEDTLS_MD_SHA256, 32, hash,
                                    raw) == 0;
        put_string(&g, raw, len);
    }
    put_string(&o, sig, g.len);
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&sv);
    mbedtls_ecp_point_free(&q);
    mbedtls_pk_free(&pk);
    if (!ok || g.full) {
        printf("Couldn't sign with %s\n", path);
        return 0;
    }
    printf("Logging in with the key in %s\n", path);
    return send_msg(s, &o) ? 1 : -1;
}

/// Logs in: a key, then the password or keyboard-interactive.
/// @param s the connection
/// @param user the user
/// @param host the host, for the prompt
/// @return logged in
static bool authenticate(ssh_t *s, const char *user, const char *host) {
    out_t o = start_msg(s, MSG_SERVICE_REQUEST);
    put_cstring(&o, "ssh-userauth");
    in_t m;
    if (!send_msg(s, &o) || expect_msg(s, &m, MSG_SERVICE_ACCEPT, "logging in") < 0)
        return false;
    // "none" gets us the list of methods the server takes.
    if (!send_auth(s, user, "none", NULL))
        return false;
    bool key_tried = false, key_pending = false;
    for (int tries = 0;;) {
        int t = recv_msg(s, &m);
        if (t < 0)
            return false;
        if (t == 0)
            continue;
        if (t == MSG_USERAUTH_BANNER) {
            print_banner(&m);
            continue;
        }
        if (t == MSG_USERAUTH_SUCCESS)
            return true;
        if (t == MSG_USERAUTH_INFO_REQUEST) {
            if (!answer_info_request(s, &m))
                return false;
            continue;
        }
        if (t != MSG_USERAUTH_FAILURE) {
            printf("ssh: unexpected message %d while logging in\n", t);
            return false;
        }
        size_t n;
        const uint8_t *list = get_string(&m, &n);
        bool password = list_has(list, n, "password");
        bool kbd = list_has(list, n, "keyboard-interactive");
        if (key_pending) {
            printf("The server didn't accept the key\n");
            key_pending = false;
        }
        if (!key_tried && list_has(list, n, "publickey")) {
            key_tried = true;
            int sent = send_key_auth(s, user);
            if (sent < 0)
                return false;
            if (sent) {
                key_pending = true;
                continue;
            }
        }
        if (!password && !kbd) {
            printf("The server doesn't take passwords (it wants: ");
            print_text(list, n);
            printf(")\n");
            return false;
        }
        if (tries == 3) {
            printf("Permission denied.\n");
            return false;
        }
        if (tries++)
            printf("Permission denied, please try again.\n");
        if (!password) {
            if (!send_auth(s, user, "keyboard-interactive", NULL))
                return false;
            continue;
        }
        char pw[128];
        printf("%s@%s's password: ", user, host);
        fflush(stdout);
        if (!read_secret(pw, sizeof pw))
            return false;
        bool ok = send_auth(s, user, "password", pw);
        mbedtls_platform_zeroize(pw, sizeof pw);
        if (!ok)
            return false;
    }
}

// ---- The channel ------------------------------------------------------------------------

/// Gives the server more window when half of it is used.
/// @param s the connection
/// @return sent (or not needed)
static bool send_window_adjust(ssh_t *s) {
    size_t room = INQ_SIZE - s->inq_len;
    if (room - s->window_given < INQ_SIZE / 2)
        return true;
    uint32_t add = room - s->window_given;
    out_t o = start_msg(s, MSG_CHANNEL_WINDOW_ADJUST);
    put_u32(&o, s->remote_id);
    put_u32(&o, add);
    s->window_given += add;
    return send_msg(s, &o);
}

/// Deals with a channel message: data, EOF, close, exit status and the like.
/// @param s the connection
/// @param type the message type
/// @param m the message
/// @return all right
static bool handle_channel_msg(ssh_t *s, uint8_t type, in_t *m) {
    uint32_t id = get_u32(m);
    (void)id;  // we have only one channel
    switch (type) {
    case MSG_CHANNEL_WINDOW_ADJUST:
        s->remote_window += get_u32(m);
        break;
    case MSG_CHANNEL_DATA: {
        size_t n;
        const uint8_t *d = get_string(m, &n);
        if (m->bad || n > s->window_given || n > INQ_SIZE - s->inq_len) {
            printf("\nssh: the server sent more than the window allows\n");
            return false;
        }
        s->window_given -= n;
        for (size_t i = 0; i < n; ++i)
            s->inq[(s->inq_head + s->inq_len + i) % INQ_SIZE] = d[i];
        s->inq_len += n;
        break;
    }
    case MSG_CHANNEL_EXTENDED_DATA: {  // stderr
        get_u32(m);
        size_t n;
        const uint8_t *d = get_string(m, &n);
        if (s->interactive) {  // in order with the rest, as a terminal shows it
            if (m->bad || n > s->window_given || n > INQ_SIZE - s->inq_len) {
                printf("\nssh: the server sent more than the window allows\n");
                return false;
            }
            s->window_given -= n;
            for (size_t i = 0; i < n; ++i)
                s->inq[(s->inq_head + s->inq_len + i) % INQ_SIZE] = d[i];
            s->inq_len += n;
            break;
        }
        s->window_given -= n < s->window_given ? n : s->window_given;
        print_text(d, n);
        return send_window_adjust(s);
    }
    case MSG_CHANNEL_EOF:
        s->got_eof = true;
        break;
    case MSG_CHANNEL_CLOSE:
        s->got_close = true;
        s->got_eof = true;
        if (!s->sent_close) {
            out_t o = start_msg(s, MSG_CHANNEL_CLOSE);
            put_u32(&o, s->remote_id);
            s->sent_close = true;
            return send_msg(s, &o);
        }
        break;
    case MSG_CHANNEL_REQUEST: {
        size_t n;
        const uint8_t *what = get_string(m, &n);
        bool want_reply = get_u8(m);
        if (str_is(what, n, "exit-status")) {
            s->exit_status = get_u32(m);
        } else if (str_is(what, n, "exit-signal")) {
            size_t sn;
            const uint8_t *sig = get_string(m, &sn);
            snprintf(s->exit_signal, sizeof s->exit_signal, "%.*s", (int)(sn < 15 ? sn : 15), sig);
        }
        if (want_reply) {
            out_t o = start_msg(s, MSG_CHANNEL_FAILURE);
            put_u32(&o, s->remote_id);
            return send_msg(s, &o);
        }
        break;
    }
    case MSG_CHANNEL_SUCCESS:
        s->reply = 1;
        break;
    case MSG_CHANNEL_FAILURE:
        s->reply = -1;
        break;
    }
    return true;
}

/// Opens the session channel.
/// @param s the connection
/// @return opened
static bool open_session(ssh_t *s) {
    out_t o = start_msg(s, MSG_CHANNEL_OPEN);
    put_cstring(&o, "session");
    put_u32(&o, 0);  // our channel number
    put_u32(&o, INQ_SIZE);
    put_u32(&o, DATA_MAX);
    s->window_given = INQ_SIZE;
    in_t m;
    if (!send_msg(s, &o))
        return false;
    int t;
    while ((t = recv_msg(s, &m)) == 0)
        ;
    if (t == MSG_CHANNEL_OPEN_FAILURE) {
        get_u32(&m);
        get_u32(&m);
        size_t n;
        const uint8_t *why = get_string(&m, &n);
        printf("The server refused a session: ");
        print_text(why, n);
        printf("\n");
        return false;
    }
    if (t != MSG_CHANNEL_OPEN_CONFIRMATION) {
        if (t > 0)
            printf("ssh: unexpected message %d opening a session\n", t);
        return false;
    }
    get_u32(&m);  // our number
    s->remote_id = get_u32(&m);
    s->remote_window = get_u32(&m);
    s->remote_max = get_u32(&m);
    if (s->remote_max > DATA_MAX)
        s->remote_max = DATA_MAX;
    s->open = true;
    return true;
}

/// Waits for the answer to a channel request sent with want-reply.
/// @param s the connection
/// @return accepted
static bool request_ok(ssh_t *s) {
    in_t m;
    int t;
    while (!s->reply && !s->got_close) {
        t = recv_msg(s, &m);
        if (t < 0)
            return false;
        if (t) {
            printf("ssh: unexpected message %d starting the session\n", t);
            return false;
        }
    }
    return s->reply > 0 && !s->got_close;
}

/// Sends a channel request: name, want-reply, then what the caller adds.
/// @param s the connection
/// @param name the request
/// @return the message
static out_t start_request(ssh_t *s, const char *name) {
    out_t o = start_msg(s, MSG_CHANNEL_REQUEST);
    put_u32(&o, s->remote_id);
    put_cstring(&o, name);
    put_u8(&o, 1);
    s->reply = 0;
    return o;
}

bool ssh_exec(ssh_t *s, const char *command) {
    if (!open_session(s))
        return false;
    out_t o = start_request(s, "exec");
    put_cstring(&o, command);
    if (!send_msg(s, &o))
        return false;
    if (!request_ok(s)) {
        printf("The server wouldn't run \"%s\"\n", command);
        return false;
    }
    return true;
}

bool ssh_shell(ssh_t *s, const char *term, int cols, int rows, const char *command) {
    if (!open_session(s))
        return false;
    s->interactive = true;
    out_t o = start_request(s, "pty-req");
    put_cstring(&o, term);
    put_u32(&o, cols);
    put_u32(&o, rows);
    put_u32(&o, cols * 8);   // pixels, as the console's 8x16 font
    put_u32(&o, rows * 16);
    // Terminal modes: VERASE is DEL (what Backspace sends), then the end.
    static const uint8_t modes[] = {3, 0, 0, 0, 127, 0};
    put_string(&o, modes, sizeof modes);
    if (!send_msg(s, &o))
        return false;
    if (!request_ok(s))
        printf("(The server gave no terminal; full-screen programs won't work.)\n");
    o = start_request(s, command ? "exec" : "shell");
    if (command)
        put_cstring(&o, command);
    if (!send_msg(s, &o))
        return false;
    if (!request_ok(s)) {
        if (command)
            printf("The server wouldn't run \"%s\"\n", command);
        else
            printf("The server wouldn't start a shell\n");
        return false;
    }
    return true;
}

int ssh_poll(ssh_t *s, void *buf, size_t max) {
    // Deal with whatever has arrived, without waiting for more. After EOF
    // the exit status and CLOSE are still to come.
    while (!s->inq_len && !s->got_close) {
        uint8_t b;
        int got = net_conn_read(s->conn, &b, 1);
        if (got < 0) {
            s->dead = true;
            return -1;
        }
        if (got == 0)
            return 0;
        s->peeked = b;
        in_t m;
        int t = recv_msg(s, &m);
        if (t < 0)
            return -1;
        if (t)
            return -1;  // nothing else is expected now
    }
    if (!s->inq_len)
        return -1;  // the remote end has finished
    size_t n = s->inq_len < max ? s->inq_len : max;
    uint8_t *out = buf;
    for (size_t i = 0; i < n; ++i)
        out[i] = s->inq[(s->inq_head + i) % INQ_SIZE];
    s->inq_head = (s->inq_head + n) % INQ_SIZE;
    s->inq_len -= n;
    if (!s->got_eof && !send_window_adjust(s))
        return -1;
    return n;
}

bool ssh_keepalive(ssh_t *s) {
    out_t o = start_msg(s, MSG_IGNORE);
    put_cstring(&o, "");
    return send_msg(s, &o);
}

const char *ssh_exit_signal(ssh_t *s) {
    return s->exit_signal[0] ? s->exit_signal : NULL;
}

int ssh_read(ssh_t *s, void *buf, size_t max) {
    while (!s->inq_len) {
        if (s->got_eof)
            return 0;
        in_t m;
        int t = recv_msg(s, &m);
        if (t < 0)
            return -1;
        if (t) {
            printf("\nssh: unexpected message %d\n", t);
            return -1;
        }
    }
    size_t n = s->inq_len < max ? s->inq_len : max;
    uint8_t *out = buf;
    for (size_t i = 0; i < n; ++i)
        out[i] = s->inq[(s->inq_head + i) % INQ_SIZE];
    s->inq_head = (s->inq_head + n) % INQ_SIZE;
    s->inq_len -= n;
    if (!s->got_eof && !send_window_adjust(s))
        return -1;
    return n;
}

bool ssh_write(ssh_t *s, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        if (s->got_close) {
            printf("\nThe server closed the session\n");
            return false;
        }
        if (!s->remote_window) {
            in_t m;
            int t = recv_msg(s, &m);
            if (t < 0)
                return false;
            if (t) {
                printf("\nssh: unexpected message %d\n", t);
                return false;
            }
            continue;
        }
        size_t n = len;
        if (n > s->remote_window)
            n = s->remote_window;
        if (n > s->remote_max)
            n = s->remote_max;
        out_t o = start_msg(s, MSG_CHANNEL_DATA);
        put_u32(&o, s->remote_id);
        put_string(&o, p, n);
        if (!send_msg(s, &o))
            return false;
        s->remote_window -= n;
        p += n;
        len -= n;
    }
    return true;
}

void ssh_send_eof(ssh_t *s) {
    if (!s->open || s->got_close)
        return;
    out_t o = start_msg(s, MSG_CHANNEL_EOF);
    put_u32(&o, s->remote_id);
    send_msg(s, &o);
}

int ssh_exit_status(ssh_t *s) {
    return s->exit_status;
}

// ---- Opening and closing ----------------------------------------------------------------

ssh_t *ssh_open(const char *host, uint16_t port, const char *user) {
    if (!rng_ready()) {
        printf("ssh: no random numbers\n");
        return NULL;
    }
    ssh_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->exit_status = -1;
    s->peeked = -1;
    s->rx = psram_buffer_alloc(RX_MAX);
    s->tx = psram_buffer_alloc(TX_MAX);
    s->inq = psram_buffer_alloc(INQ_SIZE);
    if (!s->rx || !s->tx || !s->inq) {
        printf("ssh: not enough memory\n");
        ssh_close(s);
        return NULL;
    }
    s->conn = net_conn_open(host, port);
    if (!s->conn || !key_exchange(s, host, port) || !authenticate(s, user, host)) {
        ssh_close(s);
        return NULL;
    }
    return s;
}

/// Frees a direction's cipher and MAC.
/// @param d the direction
static void free_dir(dir_t *d) {
    if (d->on) {
        mbedtls_aes_free(&d->aes);
        mbedtls_md_free(&d->mac);
    }
}

void ssh_close(ssh_t *s) {
    if (!s)
        return;
    if (s->conn && s->out.on) {
        if (s->open && !s->sent_close) {
            out_t o = start_msg(s, MSG_CHANNEL_CLOSE);
            put_u32(&o, s->remote_id);
            send_msg(s, &o);
        }
        out_t o = start_msg(s, MSG_DISCONNECT);
        put_u32(&o, 11);  // by application
        put_cstring(&o, "bye");
        put_cstring(&o, "");
        send_msg(s, &o);
    }
    net_conn_close(s->conn);
    free_dir(&s->in);
    free_dir(&s->out);
    if (s->tx)
        mbedtls_platform_zeroize(s->tx, TX_MAX);
    psram_buffer_free(s->rx);
    psram_buffer_free(s->tx);
    psram_buffer_free(s->inq);
    mbedtls_platform_zeroize(s, sizeof *s);
    free(s);
}
