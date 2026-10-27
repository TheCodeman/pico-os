/**
 * @file smb.c
 * @brief SMB2/SMB3 client (MS-SMB2), NTLMv2 login (MS-NLMP).
 *
 * See smb.h.
 *
 * One request is in flight at a time. Messages are built in place in one
 * buffer, which the answer then overwrites; it holds a 64 KB read or write.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "smb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha512.h"
#include "net.h"
#include "pico/stdlib.h"
#include "psram_heap.h"

#define CHUNK       65536                 ///< biggest read or write we ask for
#define BUF_SIZE    (CHUNK + 4 + 64 + 256)  ///< NetBIOS + header + body + data
#define TIMEOUT_MS  30000

enum {
    NEGOTIATE = 0, SESSION_SETUP = 1, LOGOFF = 2, TREE_CONNECT = 3, TREE_DISCONNECT = 4,
    CREATE = 5, CLOSE = 6, FLUSH = 7, READ = 8, WRITE = 9, QUERY_DIRECTORY = 14,
    SET_INFO = 17,
};

#define FLAG_RESPONSE 0x1
#define FLAG_ASYNC    0x2
#define FLAG_SIGNED   0x8

#define STATUS_PENDING          0x00000103u
#define STATUS_MORE_PROCESSING  0xC0000016u

/// CREATE: access, dispositions, options
#define FILE_READ_DATA        0x00000001
#define FILE_WRITE_DATA       0x00000002
#define FILE_APPEND_DATA      0x00000004
#define FILE_READ_ATTRIBUTES  0x00000080
#define FILE_WRITE_ATTRIBUTES 0x00000100
#define ACCESS_DELETE         0x00010000
#define SYNCHRONIZE           0x00100000
#define DISP_OPEN             1
#define DISP_CREATE           2
#define DISP_OPEN_IF          3
#define DISP_OVERWRITE_IF     5
#define OPT_DIRECTORY         0x00000001
#define OPT_NON_DIRECTORY     0x00000040

struct smb {
    net_conn_t *conn;
    uint8_t *buf;  ///< PSRAM
    uint64_t msg_id, session_id;
    uint32_t tree_id;
    uint16_t dialect;
    bool sign;  ///< requests are signed (and signed answers checked)
    bool guest;
    bool dead;
    uint8_t signing_key[16];
    uint8_t preauth[64];  ///< 3.1.1: hash of the negotiate and login messages
    uint32_t max_read, max_write, max_trans;
    size_t resp_len;  ///< the last answer's SMB2 message length
};

// ---- Little helpers -----------------------------------------------------------------

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, v);
    put32(p + 4, v >> 32);
}

static uint16_t get16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

static uint32_t get32(const uint8_t *p) {
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static uint64_t get64(const uint8_t *p) {
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

/// Code page 437 text to UTF-16LE, / becoming \ (for paths). Bytes written.
static int to_utf16(const char *s, uint8_t *out, int max_bytes) {
    int n = 0;
    for (; *s && n + 2 <= max_bytes; ++s) {
        uint8_t c = *s;
        uint16_t u = c < 0x80 ? c : ff_oem2uni(c, FF_CODE_PAGE);
        if (u == '/')
            u = '\\';
        if (!u)
            u = '_';
        put16(out + n, u);
        n += 2;
    }
    return n;
}

/// UTF-16LE to code page 437 ('?' for what it hasn't got).
static void from_utf16(const uint8_t *in, int bytes, char *out, int size) {
    int n = 0;
    for (int i = 0; i + 1 < bytes && n < size - 1; i += 2) {
        uint16_t u = get16(in + i);
        if (u >= 0xd800 && u < 0xdc00 && i + 3 < bytes) {
            i += 2;  // a character outside the BMP: no CP437 for it
            u = '?';
        }
        uint8_t c = u < 0x80 ? u : (uint8_t)ff_uni2oem(u, FF_CODE_PAGE);
        out[n++] = c ? c : '?';
    }
    out[n] = 0;
}

// ---- MD4 (for the NT password hash; mbedTLS 3 has dropped it) ------------------------

#define ROTL(x, n) ((x) << (n) | (x) >> (32 - (n)))

static void md4_block(uint32_t h[4], const uint8_t *p) {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = get32(p + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    static const int s1[4] = {3, 7, 11, 19}, s2[4] = {3, 5, 9, 13}, s3[4] = {3, 9, 11, 15};
    for (int i = 0; i < 16; ++i) {
        uint32_t t = a + ((b & c) | (~b & d)) + x[i];
        a = d, d = c, c = b, b = ROTL(t, s1[i % 4]);
    }
    for (int i = 0; i < 16; ++i) {
        int k = (i % 4) * 4 + i / 4;
        uint32_t t = a + ((b & c) | (b & d) | (c & d)) + x[k] + 0x5a827999;
        a = d, d = c, c = b, b = ROTL(t, s2[i % 4]);
    }
    static const int order[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (int i = 0; i < 16; ++i) {
        uint32_t t = a + (b ^ c ^ d) + x[order[i]] + 0x6ed9eba1;
        a = d, d = c, c = b, b = ROTL(t, s3[i % 4]);
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

static void md4(const uint8_t *msg, size_t len, uint8_t out[16]) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        md4_block(h, msg + i);
    uint8_t last[128] = {0};
    size_t rest = len - i;
    memcpy(last, msg + i, rest);
    last[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    put64(last + total - 8, (uint64_t)len * 8);
    md4_block(h, last);
    if (total == 128)
        md4_block(h, last + 64);
    for (int k = 0; k < 4; ++k)
        put32(out + 4 * k, h[k]);
}

static void hmac(mbedtls_md_type_t type, const uint8_t *key, size_t key_len, const uint8_t *a,
                 size_t a_len, const uint8_t *b, size_t b_len, uint8_t *out) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(type), 1);
    mbedtls_md_hmac_starts(&ctx, key, key_len);
    mbedtls_md_hmac_update(&ctx, a, a_len);
    if (b_len)
        mbedtls_md_hmac_update(&ctx, b, b_len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

/// SP800-108 counter-mode KDF with HMAC-SHA256, 128 bits out (MS-SMB2 3.1.4.2).
static void kdf(const uint8_t key[16], const char *label, int label_len, const uint8_t *context,
                int context_len, uint8_t out[16]) {
    uint8_t data[128], full[32];
    int n = 0;
    put32(data, 0), data[3] = 1, n = 4;  // i = 1, big-endian
    memcpy(data + n, label, label_len), n += label_len;
    data[n++] = 0;
    memcpy(data + n, context, context_len), n += context_len;
    data[n++] = 0, data[n++] = 0, data[n++] = 0, data[n++] = 128;  // L = 128, big-endian
    hmac(MBEDTLS_MD_SHA256, key, 16, data, n, NULL, 0, full);
    memcpy(out, full, 16);
}

// ---- Messages -----------------------------------------------------------------------

static uint8_t *body(smb_t *s) {
    return s->buf + 4 + 64;
}

/// The message's signature (computed with its signature field zeroed; out
/// may be that field).
static void signature_of(smb_t *s, uint8_t *msg, size_t len, uint8_t out[16]) {
    uint8_t saved[16], sig[32];
    memcpy(saved, msg + 48, 16);
    memset(msg + 48, 0, 16);
    if (s->dialect < 0x0300)
        hmac(MBEDTLS_MD_SHA256, s->signing_key, 16, msg, len, NULL, 0, sig);
    else
        mbedtls_cipher_cmac(mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB),
                            s->signing_key, 128, msg, len, sig);
    memcpy(msg + 48, saved, 16);
    memcpy(out, sig, 16);
}

static bool read_exact(smb_t *s, uint8_t *p, size_t n) {
    uint64_t last = time_us_64();
    while (n) {
        int got = net_conn_read(s->conn, p, n);
        if (got < 0) {
            s->dead = true;
            return false;
        }
        if (got > 0) {
            p += got;
            n -= got;
            last = time_us_64();
            continue;
        }
        uint64_t waited = time_us_64() - last;
        if (waited > TIMEOUT_MS * 1000ull) {
            s->dead = true;
            return false;
        }
        // Only a long wait checks the keyboard, so keys typed meanwhile
        // aren't eaten.
        if (waited > 2000000 && net_stop_key()) {
            s->dead = true;  // a half-read answer: the connection is unusable
            return false;
        }
        sleep_ms(1);
    }
    return true;
}

/// Sends the request whose body (body_len bytes) is in place and waits for
/// its answer. The answer's status; *rbody/*rlen are its body. hash_request:
/// fold the request into the 3.1.1 pre-authentication hash.
static uint32_t exchange(smb_t *s, uint16_t cmd, size_t body_len, uint16_t charge,
                         bool hash_request, uint8_t **rbody, size_t *rlen) {
    if (s->dead)
        return SMB_CONNECTION_LOST;
    uint8_t *h = s->buf + 4;
    size_t len = 64 + body_len;
    memset(h, 0, 64);
    h[0] = 0xfe, h[1] = 'S', h[2] = 'M', h[3] = 'B';
    put16(h + 4, 64);
    put16(h + 6, s->dialect >= 0x0210 ? charge : 0);
    put16(h + 12, cmd);
    put16(h + 14, 64);  // credits wanted
    put32(h + 16, s->sign ? FLAG_SIGNED : 0);
    uint64_t id = s->msg_id;
    s->msg_id += charge ? charge : 1;
    put64(h + 24, id);
    put32(h + 36, s->tree_id);
    put64(h + 40, s->session_id);
    if (s->sign)
        signature_of(s, h, len, h + 48);
    if (hash_request) {
        uint8_t data[64];
        memcpy(data, s->preauth, 64);
        mbedtls_sha512_context c;
        mbedtls_sha512_init(&c);
        mbedtls_sha512_starts(&c, 0);
        mbedtls_sha512_update(&c, data, 64);
        mbedtls_sha512_update(&c, h, len);
        mbedtls_sha512_finish(&c, s->preauth);
        mbedtls_sha512_free(&c);
    }
    s->buf[0] = 0;
    s->buf[1] = len >> 16;
    s->buf[2] = len >> 8;
    s->buf[3] = len;
    if (!net_conn_write(s->conn, s->buf, 4 + len)) {
        s->dead = true;
        return SMB_CONNECTION_LOST;
    }
    for (;;) {
        uint8_t nb[4];
        if (!read_exact(s, nb, 4))
            return SMB_CONNECTION_LOST;
        size_t n = (size_t)nb[1] << 16 | nb[2] << 8 | nb[3];
        if (nb[0] != 0 || n < 64 || n > BUF_SIZE - 4) {
            s->dead = true;
            return SMB_CONNECTION_LOST;
        }
        if (!read_exact(s, h, n))
            return SMB_CONNECTION_LOST;
        if (h[0] != 0xfe || h[1] != 'S' || get64(h + 24) != id)
            continue;  // not ours (an oplock break, say): skip it
        uint32_t status = get32(h + 8);
        uint32_t flags = get32(h + 16);
        if (status == STATUS_PENDING && (flags & FLAG_ASYNC))
            continue;  // the real answer follows
        if (s->sign && (flags & FLAG_SIGNED)) {
            uint8_t want[16];
            signature_of(s, h, n, want);
            if (memcmp(want, h + 48, 16)) {
                s->dead = true;
                return SMB_CONNECTION_LOST;  // tampered with: stop using it
            }
        }
        s->resp_len = n;
        *rbody = h + 64;
        *rlen = n - 64;
        return status;
    }
}

/// Folds the last answer into the pre-authentication hash.
static void hash_response(smb_t *s) {
    uint8_t data[64];
    memcpy(data, s->preauth, 64);
    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    mbedtls_sha512_starts(&c, 0);
    mbedtls_sha512_update(&c, data, 64);
    mbedtls_sha512_update(&c, s->buf + 4, s->resp_len);
    mbedtls_sha512_finish(&c, s->preauth);
    mbedtls_sha512_free(&c);
}

// ---- Negotiate and log in ---------------------------------------------------------------

static uint32_t negotiate(smb_t *s) {
    static const uint16_t dialects[] = {0x0202, 0x0210, 0x0300, 0x0302, 0x0311};
    uint8_t *b = body(s);
    memset(b, 0, 128);
    put16(b, 36);
    put16(b + 2, 5);
    put16(b + 4, 1);  // signing enabled
    smb_random(b + 12, 16);  // client GUID
    for (int i = 0; i < 5; ++i)
        put16(b + 36 + 2 * i, dialects[i]);
    // One negotiate context (8-aligned from the header): SHA-512
    // pre-authentication integrity.
    int ctx = 112 - 64;
    put32(b + 28, 112);
    put16(b + 32, 1);
    put16(b + ctx, 1);       // PREAUTH_INTEGRITY_CAPABILITIES
    put16(b + ctx + 2, 38);
    put16(b + ctx + 8, 1);   // hash algorithms
    put16(b + ctx + 10, 32); // salt length
    put16(b + ctx + 12, 1);  // SHA-512
    smb_random(b + ctx + 14, 32);
    memset(s->preauth, 0, 64);
    uint8_t *r;
    size_t rlen;
    uint32_t st = exchange(s, NEGOTIATE, ctx + 8 + 38, 0, true, &r, &rlen);
    if (st)
        return st;
    if (rlen < 64)
        return SMB_CONNECTION_LOST;
    s->dialect = get16(r + 4);
    s->max_trans = get32(r + 28);
    s->max_read = get32(r + 32);
    s->max_write = get32(r + 36);
    if (s->dialect == 0x0311)
        hash_response(s);
    return 0;
}

/// Raw NTLMSSP tokens (no SPNEGO wrapping: Windows and Samba take them).
#define NTLM_FLAGS 0xa0888215u  ///< unicode, target, sign, NTLM, always sign,
                                // extended session security, target info, 128-bit

static int ntlm_negotiate(uint8_t *out) {
    memset(out, 0, 32);
    memcpy(out, "NTLMSSP", 8);
    put32(out + 8, 1);
    put32(out + 12, NTLM_FLAGS);
    return 32;
}

/// Builds the AUTHENTICATE message from the server's CHALLENGE; sets the
/// session key. Its length, or -1 if the challenge is bad.
static int ntlm_authenticate(const uint8_t *chal, size_t chal_len, const char *domain,
                             const char *user, const char *password, uint8_t *out, int max,
                             uint8_t session_key[16]) {
    if (chal_len < 48 || memcmp(chal, "NTLMSSP", 8) || get32(chal + 8) != 2)
        return -1;
    const uint8_t *server_chal = chal + 24;
    uint16_t ti_len = get16(chal + 40);
    uint32_t ti_off = get32(chal + 44);
    if (ti_off + ti_len > chal_len)
        return -1;
    const uint8_t *ti = chal + ti_off;
    uint8_t u_user[128], u_dom[128], u_upper[256];
    int user_n = to_utf16(user, u_user, sizeof u_user);
    int dom_n = to_utf16(domain, u_dom, sizeof u_dom);
    bool anonymous = !user[0];

    // The NTLMv2 response (MS-NLMP 3.3.2).
    uint8_t nt_hash[16], key[16], blob[600], nt_resp[16 + 600], lm_resp[24];
    int blob_n = 0, nt_n = 0, lm_n = 0;
    memset(session_key, 0, 16);
    if (!anonymous) {
        uint8_t u_pw[256];
        int pw_n = to_utf16(password, u_pw, sizeof u_pw);
        md4(u_pw, pw_n, nt_hash);
        mbedtls_platform_zeroize(u_pw, sizeof u_pw);
        int up_n = 0;
        for (int i = 0; i < user_n; i += 2) {  // the user name in capitals
            uint16_t c = get16(u_user + i);
            if (c >= 'a' && c <= 'z')
                c -= 32;
            put16(u_upper + up_n, c);
            up_n += 2;
        }
        memcpy(u_upper + up_n, u_dom, dom_n);
        hmac(MBEDTLS_MD_MD5, nt_hash, 16, u_upper, up_n + dom_n, NULL, 0, key);
        // The server's time from the target info, else ours.
        uint64_t stamp = 0;
        bool have_stamp = false;
        for (int i = 0; i + 4 <= ti_len;) {
            uint16_t id = get16(ti + i), n = get16(ti + i + 2);
            if (id == 0)
                break;
            if (id == 7 && n == 8) {
                stamp = get64(ti + i + 4);
                have_stamp = true;
            }
            i += 4 + n;
        }
        if (!have_stamp)
            stamp = (smb_unix_time() + 11644473600ull) * 10000000ull;
        if (ti_len > sizeof blob - 40)
            return -1;
        uint8_t client_chal[8];
        smb_random(client_chal, 8);
        memset(blob, 0, 28);
        blob[0] = 1, blob[1] = 1;
        put64(blob + 8, stamp);
        memcpy(blob + 16, client_chal, 8);
        memcpy(blob + 28, ti, ti_len);
        blob_n = 28 + ti_len;
        memset(blob + blob_n, 0, 4);
        blob_n += 4;
        uint8_t proof[16];
        hmac(MBEDTLS_MD_MD5, key, 16, server_chal, 8, blob, blob_n, proof);
        memcpy(nt_resp, proof, 16);
        memcpy(nt_resp + 16, blob, blob_n);
        nt_n = 16 + blob_n;
        hmac(MBEDTLS_MD_MD5, key, 16, proof, 16, NULL, 0, session_key);
        if (have_stamp) {
            memset(lm_resp, 0, 24);  // as MS-NLMP asks when the server sent a time
        } else {
            uint8_t both[16];
            memcpy(both, server_chal, 8);
            memcpy(both + 8, client_chal, 8);
            hmac(MBEDTLS_MD_MD5, key, 16, both, 16, NULL, 0, lm_resp);
            memcpy(lm_resp + 16, client_chal, 8);
        }
        lm_n = 24;
        mbedtls_platform_zeroize(nt_hash, 16);
        mbedtls_platform_zeroize(key, 16);
    } else {
        lm_resp[0] = 0;
        lm_n = 1;
    }
    int need = 64 + lm_n + nt_n + dom_n + user_n;
    if (need > max)
        return -1;
    memset(out, 0, 64);
    memcpy(out, "NTLMSSP", 8);
    put32(out + 8, 3);
    int off = 64;
    struct {
        int field;
        const uint8_t *data;
        int n;
    } parts[] = {{12, lm_resp, lm_n}, {20, nt_resp, nt_n}, {28, u_dom, dom_n}, {36, u_user, user_n},
                 {44, NULL, 0}, {52, NULL, 0}};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i) {
        put16(out + parts[i].field, parts[i].n);
        put16(out + parts[i].field + 2, parts[i].n);
        put32(out + parts[i].field + 4, off);
        if (parts[i].n)
            memcpy(out + off, parts[i].data, parts[i].n);
        off += parts[i].n;
    }
    put32(out + 60, NTLM_FLAGS | (anonymous ? 0x800 : 0));  // 0x800: anonymous
    return off;
}

static uint32_t session_setup(smb_t *s, const char *domain, const char *user, const char *password) {
    uint8_t *b = body(s);
    uint8_t *r;
    size_t rlen;
    memset(b, 0, 24);
    put16(b, 25);
    b[3] = 1;  // signing enabled
    put16(b + 12, 64 + 24);
    int n = ntlm_negotiate(b + 24);
    put16(b + 14, n);
    bool h311 = s->dialect == 0x0311;
    uint32_t st = exchange(s, SESSION_SETUP, 24 + n, 1, h311, &r, &rlen);
    if (st != STATUS_MORE_PROCESSING)
        return st ? st : SMB_ACCESS_DENIED;
    s->session_id = get64(s->buf + 4 + 40);
    if (h311)
        hash_response(s);
    uint16_t off = get16(r + 4), len = get16(r + 6);
    if (off < 64 || off + len > s->resp_len)
        return SMB_ACCESS_DENIED;
    // The challenge is overwritten by the next request: keep a copy.
    uint8_t *chal = malloc(len);
    if (!chal)
        return SMB_DISK_FULL;
    memcpy(chal, s->buf + 4 + off, len);
    // A SPNEGO-wrapped answer has the NTLMSSP message inside it.
    size_t at = 0;
    while (at + 8 <= len && memcmp(chal + at, "NTLMSSP", 8))
        ++at;
    uint8_t key[16];
    memset(b, 0, 24);
    put16(b, 25);
    b[3] = 1;
    put16(b + 12, 64 + 24);
    n = at + 8 <= len ? ntlm_authenticate(chal + at, len - at, domain, user, password, b + 24,
                                          BUF_SIZE - 200, key)
                      : -1;
    free(chal);
    if (n < 0)
        return SMB_ACCESS_DENIED;
    put16(b + 14, n);
    st = exchange(s, SESSION_SETUP, 24 + n, 1, h311, &r, &rlen);
    mbedtls_platform_zeroize(b + 24, n);
    if (st)
        return st;
    uint16_t session_flags = get16(r + 2);
    s->guest = !user[0] || (session_flags & 3);  // guest or null session: no key
    if (session_flags & 4)
        return SMB_NOT_SUPPORTED_ENCRYPT;
    if (!s->guest) {
        if (s->dialect < 0x0300)
            memcpy(s->signing_key, key, 16);
        else if (s->dialect < 0x0311)
            kdf(key, "SMB2AESCMAC", 12, (const uint8_t *)"SmbSign", 8, s->signing_key);
        else
            kdf(key, "SMBSigningKey", 14, s->preauth, 64, s->signing_key);
        s->sign = true;
    }
    mbedtls_platform_zeroize(key, 16);
    return 0;
}

static uint32_t tree_connect(smb_t *s, const char *host, const char *share) {
    uint8_t *b = body(s);
    char path[300];
    snprintf(path, sizeof path, "\\\\%s\\%s", host, share);
    memset(b, 0, 8);
    put16(b, 9);
    put16(b + 4, 64 + 8);
    int n = to_utf16(path, b + 8, 600);
    put16(b + 6, n);
    uint8_t *r;
    size_t rlen;
    uint32_t st = exchange(s, TREE_CONNECT, 8 + n, 1, false, &r, &rlen);
    if (st)
        return st;
    s->tree_id = get32(s->buf + 4 + 36);
    if (get32(r + 4) & 0x8000)  // SMB2_SHAREFLAG_ENCRYPT_DATA
        return SMB_NOT_SUPPORTED_ENCRYPT;
    return 0;
}

smb_t *smb_connect(const char *host, uint16_t port, const char *share, const char *domain,
                   const char *user, const char *password, char *err, int err_size) {
    smb_t *s = psram_malloc(sizeof *s);  // (PSRAM: SRAM is for what's running)
    if (s) {
        memset(s, 0, sizeof *s);
        s->buf = psram_buffer_alloc(BUF_SIZE);
    }
    if (!s || !s->buf) {
        snprintf(err, err_size, "not enough memory");
        if (s)
            psram_free(s);
        return NULL;
    }
    s->conn = net_conn_open(host, port);
    if (!s->conn) {
        snprintf(err, err_size, "can't connect to %s port %u", host, port);
        smb_disconnect(s);
        return NULL;
    }
    uint32_t st = negotiate(s);
    const char *step = "negotiate";
    if (!st) {
        step = "log in";
        st = session_setup(s, domain, user, password);
    }
    if (!st) {
        step = "open the share";
        st = tree_connect(s, host, share);
    }
    if (st) {
        snprintf(err, err_size, "can't %s: %s%s", step, smb_status_text(st),
                 st == SMB_ACCESS_DENIED && step[0] == 'l'
                     ? " (or the server requires encryption, which isn't supported)"
                     : "");
        smb_disconnect(s);
        return NULL;
    }
    return s;
}

void smb_disconnect(smb_t *s) {
    if (!s)
        return;
    if (s->conn && !s->dead) {
        uint8_t *r;
        size_t rlen;
        if (s->tree_id) {
            uint8_t *b = body(s);
            memset(b, 0, 4);
            put16(b, 4);
            exchange(s, TREE_DISCONNECT, 4, 1, false, &r, &rlen);
        }
        if (s->session_id) {
            uint8_t *b = body(s);
            memset(b, 0, 4);
            put16(b, 4);
            exchange(s, LOGOFF, 4, 1, false, &r, &rlen);
        }
    }
    net_conn_close(s->conn);
    mbedtls_platform_zeroize(s->signing_key, 16);
    psram_buffer_free(s->buf);
    psram_free(s);
}

bool smb_alive(smb_t *s) {
    return s && !s->dead;
}

void smb_describe(smb_t *s, char *out, int size) {
    const char *d = s->dialect == 0x0202   ? "2.0.2"
                    : s->dialect == 0x0210 ? "2.1"
                    : s->dialect == 0x0300 ? "3.0"
                    : s->dialect == 0x0302 ? "3.0.2"
                    : s->dialect == 0x0311 ? "3.1.1"
                                           : "?";
    snprintf(out, size, "SMB %s, %s", d,
             s->sign ? "signed" : s->guest ? "guest, not signed" : "not signed");
}

// ---- Files ------------------------------------------------------------------------------

static uint32_t create(smb_t *s, const char *path, uint32_t access, uint32_t disposition,
                       uint32_t options, smb_fid_t *fid, smb_info_t *info) {
    uint8_t *b = body(s);
    memset(b, 0, 56);
    put16(b, 57);
    put32(b + 4, 2);  // impersonation
    put32(b + 24, access);
    put32(b + 28, 0x80);  // FILE_ATTRIBUTE_NORMAL for a new file
    put32(b + 32, 7);     // share read, write and delete
    put32(b + 36, disposition);
    put32(b + 40, options);
    put16(b + 44, 64 + 56);
    while (*path == '/' || *path == '\\')
        ++path;
    int n = to_utf16(path, b + 56, 1024);
    put16(b + 46, n);
    if (!n)
        b[56] = 0;  // the share's root: an empty name (the buffer still has a byte)
    uint8_t *r;
    size_t rlen;
    uint32_t st = exchange(s, CREATE, 56 + (n ? n : 1), 1, false, &r, &rlen);
    if (st)
        return st;
    if (rlen < 80)
        return SMB_CONNECTION_LOST;
    memcpy(fid->id, r + 64, 16);
    if (info) {
        info->mtime = get64(r + 24);
        info->size = get64(r + 48);
        info->attrib = get32(r + 56);
    }
    return 0;
}

uint32_t smb_open(smb_t *s, const char *path, int mode, smb_fid_t *fid, smb_info_t *info) {
    uint32_t access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
    if (mode & SMB_READ)
        access |= FILE_READ_DATA;
    if (mode & SMB_WRITE)
        access |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES;
    uint32_t disp = DISP_OPEN;
    switch (mode & 0x30) {
    case SMB_CREATE_NEW: disp = DISP_CREATE; break;
    case SMB_TRUNCATE: disp = DISP_OVERWRITE_IF; break;
    case SMB_OPEN_ALWAYS: disp = DISP_OPEN_IF; break;
    }
    uint32_t opts = (mode & SMB_DIRECTORY) ? OPT_DIRECTORY : OPT_NON_DIRECTORY;
    return create(s, path, access, disp, opts, fid, info);
}

uint32_t smb_close(smb_t *s, const smb_fid_t *fid) {
    uint8_t *b = body(s);
    memset(b, 0, 24);
    put16(b, 24);
    memcpy(b + 8, fid->id, 16);
    uint8_t *r;
    size_t rlen;
    return exchange(s, CLOSE, 24, 1, false, &r, &rlen);
}

static uint32_t chunk(smb_t *s, uint32_t max) {
    uint32_t c = CHUNK;
    if (max && max < c)
        c = max;
    if (s->dialect == 0x0202 && c > 65536)
        c = 65536;
    return c;
}

uint32_t smb_read(smb_t *s, const smb_fid_t *fid, uint64_t offset, void *buf, uint32_t len,
                  uint32_t *got) {
    *got = 0;
    uint8_t *out = buf;
    while (len) {
        uint32_t n = len < chunk(s, s->max_read) ? len : chunk(s, s->max_read);
        uint8_t *b = body(s);
        memset(b, 0, 49);
        put16(b, 49);
        b[2] = 0x50;
        put32(b + 4, n);
        put64(b + 8, offset);
        memcpy(b + 16, fid->id, 16);
        uint8_t *r;
        size_t rlen;
        uint32_t st = exchange(s, READ, 49, (n - 1) / 65536 + 1, false, &r, &rlen);
        if (st == SMB_END_OF_FILE)
            return 0;
        if (st)
            return st;
        uint32_t data_off = r[2], data_len = get32(r + 4);
        if (data_off < 64 || data_off + data_len > s->resp_len || data_len > n)
            return SMB_CONNECTION_LOST;
        memcpy(out, s->buf + 4 + data_off, data_len);
        out += data_len;
        offset += data_len;
        *got += data_len;
        len -= data_len;
        if (data_len < n)
            break;  // the end of the file
    }
    return 0;
}

uint32_t smb_write(smb_t *s, const smb_fid_t *fid, uint64_t offset, const void *buf, uint32_t len,
                   uint32_t *done) {
    *done = 0;
    const uint8_t *in = buf;
    while (len) {
        uint32_t n = len < chunk(s, s->max_write) ? len : chunk(s, s->max_write);
        uint8_t *b = body(s);
        memset(b, 0, 48);
        put16(b, 49);
        put16(b + 2, 64 + 48);
        put32(b + 4, n);
        put64(b + 8, offset);
        memcpy(b + 16, fid->id, 16);
        memcpy(b + 48, in, n);
        uint8_t *r;
        size_t rlen;
        uint32_t st = exchange(s, WRITE, 48 + n, (n - 1) / 65536 + 1, false, &r, &rlen);
        if (st)
            return st;
        uint32_t count = get32(r + 4);
        if (!count || count > n)
            return SMB_DISK_FULL;
        in += count;
        offset += count;
        *done += count;
        len -= count;
    }
    return 0;
}

uint32_t smb_flush(smb_t *s, const smb_fid_t *fid) {
    uint8_t *b = body(s);
    memset(b, 0, 24);
    put16(b, 24);
    memcpy(b + 8, fid->id, 16);
    uint8_t *r;
    size_t rlen;
    return exchange(s, FLUSH, 24, 1, false, &r, &rlen);
}

uint32_t smb_list(smb_t *s, const smb_fid_t *dir, bool restart, smb_entry_fn fn, void *ctx) {
    uint8_t *b = body(s);
    memset(b, 0, 32);
    put16(b, 33);
    b[2] = 0x01;  // FileDirectoryInformation
    b[3] = restart ? 1 : 0;
    memcpy(b + 8, dir->id, 16);
    put16(b + 24, 64 + 32);
    put16(b + 26, 2);
    put16(b + 32, '*');
    uint32_t out_len = chunk(s, s->max_trans);
    put32(b + 28, out_len);
    uint8_t *r;
    size_t rlen;
    uint32_t st = exchange(s, QUERY_DIRECTORY, 34, (out_len - 1) / 65536 + 1, false, &r, &rlen);
    if (st)
        return st;
    uint16_t off = get16(r + 2);
    uint32_t len = get32(r + 4);
    if (off < 64 || off + len > s->resp_len)
        return SMB_CONNECTION_LOST;
    const uint8_t *e = s->buf + 4 + off, *end = e + len;
    while (e + 64 <= end) {
        uint32_t next = get32(e), name_len = get32(e + 60);
        if (e + 64 + name_len > end)
            break;
        char name[FF_LFN_BUF + 1];
        from_utf16(e + 64, name_len, name, sizeof name);
        if (strcmp(name, ".") && strcmp(name, "..")) {
            smb_info_t info = {get64(e + 40), get32(e + 56), get64(e + 24)};
            fn(ctx, name, &info);
        }
        if (!next)
            break;
        e += next;
    }
    return 0;
}

uint32_t smb_stat(smb_t *s, const char *path, smb_info_t *info) {
    smb_fid_t fid;
    uint32_t st = create(s, path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, DISP_OPEN, 0, &fid, info);
    if (st)
        return st;
    return smb_close(s, &fid);
}

uint32_t smb_mkdir(smb_t *s, const char *path) {
    smb_fid_t fid;
    uint32_t st = create(s, path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, DISP_CREATE, OPT_DIRECTORY,
                         &fid, NULL);
    if (st)
        return st;
    return smb_close(s, &fid);
}

uint32_t smb_delete(smb_t *s, const char *path) {
    // Open it, then mark it for deletion (FileDispositionInformation): unlike
    // the delete-on-close option this says when a folder isn't empty.
    smb_fid_t fid;
    uint32_t st = create(s, path, ACCESS_DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, DISP_OPEN, 0,
                         &fid, NULL);
    if (st)
        return st;
    uint8_t *b = body(s);
    memset(b, 0, 33);
    put16(b, 33);
    b[2] = 1;   // file information
    b[3] = 13;  // FileDispositionInformation
    put32(b + 4, 1);
    put16(b + 8, 64 + 32);
    memcpy(b + 16, fid.id, 16);
    b[32] = 1;  // DeletePending
    uint8_t *r;
    size_t rlen;
    st = exchange(s, SET_INFO, 33, 1, false, &r, &rlen);
    uint32_t cst = smb_close(s, &fid);
    return st ? st : cst;
}

uint32_t smb_rename(smb_t *s, const char *from, const char *to) {
    smb_fid_t fid;
    uint32_t st = create(s, from, ACCESS_DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, DISP_OPEN, 0,
                         &fid, NULL);
    if (st)
        return st;
    uint8_t *b = body(s);
    memset(b, 0, 32 + 20);
    put16(b, 33);
    b[2] = 1;   // file information
    b[3] = 10;  // FileRenameInformation
    put16(b + 8, 64 + 32);
    memcpy(b + 16, fid.id, 16);
    while (*to == '/' || *to == '\\')
        ++to;
    int n = to_utf16(to, b + 32 + 20, 1024);
    put32(b + 32 + 16, n);
    put32(b + 4, 20 + n);
    uint8_t *r;
    size_t rlen;
    st = exchange(s, SET_INFO, 32 + 20 + n, 1, false, &r, &rlen);
    uint32_t cst = smb_close(s, &fid);
    return st ? st : cst;
}

const char *smb_status_text(uint32_t st) {
    switch (st) {
    case SMB_OK: return "OK";
    case SMB_NOT_FOUND: return "not found";
    case SMB_PATH_NOT_FOUND: return "folder not found";
    case SMB_NAME_COLLISION: return "already exists";
    case SMB_ACCESS_DENIED: return "access denied";
    case SMB_FILE_IS_DIRECTORY: return "is a folder";
    case SMB_NOT_A_DIRECTORY: return "not a folder";
    case SMB_DIR_NOT_EMPTY: return "folder not empty";
    case SMB_SHARING_VIOLATION: return "in use by someone else";
    case SMB_DISK_FULL: return "the share is full";
    case SMB_NAME_INVALID: return "bad name";
    case SMB_CONNECTION_LOST: return "the connection was lost";
    case SMB_CANCELLED: return "cancelled";
    case SMB_NOT_SUPPORTED_ENCRYPT: return "the share requires encryption, which isn't supported";
    case 0xC000006Du: return "wrong user name or password";
    case 0xC00000CCu: return "no such share";
    case 0xC0000072u: return "account disabled";
    case 0xC0000071u: return "password expired";
    case 0xC000006Fu: return "not allowed to log in now";
    case 0xC0000064u: return "no such user";
    case 0xC00000BBu: return "not supported by the server";
    case 0xC0000203u: return "the server didn't accept the connection";
    }
    static char text[24];
    snprintf(text, sizeof text, "error 0x%08lX", (unsigned long)st);
    return text;
}
