/**
 * @file tls.c
 * @brief TLS (HTTPS) on top of a net_conn, using mbedTLS.
 *
 * Everything runs in the calling command, not in lwIP's interrupt: the
 * handshake's arithmetic takes a second or so, and looking up root
 * certificates reads the drive.
 *
 * Root certificates come from CACERT.PEM in the drive's root (the Mozilla
 * bundle, as shipped with curl and Linux). Only a small index of it is kept
 * in RAM; the one root a server's chain needs is read from the file during
 * the handshake.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ff.h"
#include "psram_heap.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "net.h"
#include "pico/stdlib.h"

/// The root certificates.
#define CA_PATH          "0:/CACERT.PEM"
/// The most roots indexed.
#define CA_MAX           256
#define PEM_MAX          8192   ///< largest certificate we accept, as PEM text
/// How long the handshake may take.
#define HANDSHAKE_TIMEOUT_MS 30000

// ---- Time, for mbedTLS ---------------------------------------------------------

/// mbedTLS's calloc: PSRAM.
/// @param count how many
/// @param size each one's size
/// @return the memory, or NULL
void *tls_calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size)
        return NULL;
    void *p = psram_buffer_alloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

/// mbedTLS's free.
/// @param p the memory
void tls_free(void *p) {
    psram_buffer_free(p);
}

/// mbedTLS's millisecond clock.
/// @return the time since boot
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}

/// mbedTLS's gmtime_r.
/// @param tt the time
/// @param tm_buf where
/// @return tm_buf
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf) {
    time_t t = (time_t)*tt;
    return gmtime_r(&t, tm_buf);
}

// ---- Root certificate index -------------------------------------------------------

/// In PSRAM (SRAM is short) with a checksum: if its cells fade it is built again.
typedef struct {
    uint32_t hash;    ///< of the certificate's subject (DER)
    uint32_t offset;  ///< where its PEM text starts in CACERT.PEM
    uint16_t len;  ///< its length
} ca_entry_t;
static ca_entry_t *ca_index;  ///< CA_MAX
/// The roots indexed.
static int ca_count;
/// The index's checksum.
static uint32_t ca_sum;
static FSIZE_t ca_file_size;  ///< the file the index was built from
/// The file's date and time.
///@{
static WORD ca_file_date, ca_file_time;
///@}

/// FNV-1a.
/// @param p the bytes
/// @param n their count
/// @return the hash
static uint32_t hash(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;  // FNV-1a
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/// Reads `len` bytes of PEM text at `offset` and parses it.
/// @param f the file
/// @param offset where
/// @param len how many
/// @param crt the chain it is added to
/// @return 0, or an mbedTLS error
static int load_pem(FIL *f, uint32_t offset, uint16_t len, mbedtls_x509_crt *crt) {
    char *pem = malloc(len + 1);
    if (!pem)
        return -1;
    UINT got = 0;
    int r = -1;
    if (f_lseek(f, offset) == FR_OK && f_read(f, pem, len, &got) == FR_OK && got == len) {
        pem[len] = 0;
        r = mbedtls_x509_crt_parse(crt, (const unsigned char *)pem, len + 1);
    }
    free(pem);
    return r;
}

/// The index's checksum, to see whether its cells have faded.
/// @return the checksum
static uint32_t index_sum(void) {
    return hash((const uint8_t *)ca_index, ca_count * sizeof *ca_index);
}

/// Indexes CACERT.PEM unless the index is already up to date.
/// @return the number of roots, or -1 (with a message) if there's no usable file
static int ca_load_index(void) {
    if (!ca_index && !(ca_index = psram_malloc(CA_MAX * sizeof *ca_index))) {
        printf("Not enough memory for the certificate index.\n");
        return -1;
    }
    if (ca_count && index_sum() != ca_sum)
        ca_count = 0;  // damaged in PSRAM: index the file again
    FILINFO info;
    if (f_stat(CA_PATH, &info) != FR_OK) {
        printf("No CACERT.PEM in the drive's root, so certificates can't be checked.\n"
               "Copy one there (see README), or use wget -k to skip the check.\n");
        return -1;
    }
    if (ca_count && info.fsize == ca_file_size && info.fdate == ca_file_date &&
        info.ftime == ca_file_time)
        return ca_count;
    FIL f;
    if (f_open(&f, CA_PATH, FA_READ) != FR_OK) {
        printf("Can't open CACERT.PEM\n");
        return -1;
    }
    printf("Indexing CACERT.PEM...");
    fflush(stdout);
    char *pem = malloc(PEM_MAX + 1);
    mbedtls_x509_crt crt;
    ca_count = 0;
    int bad = 0;
    char line[100];
    size_t len = 0;
    uint32_t start = 0;
    bool inside = false;
    while (pem) {
        uint32_t here = (uint32_t)f_tell(&f);
        if (!f_gets(line, sizeof line, &f))
            break;
        if (!inside) {
            if (strncmp(line, "-----BEGIN CERTIFICATE-----", 27) == 0) {
                inside = true;
                start = here;
                len = 0;
            } else {
                continue;
            }
        }
        size_t n = strlen(line);
        if (len + n <= PEM_MAX) {
            memcpy(pem + len, line, n);
            len += n;
        } else {
            len = PEM_MAX + 1;  // too big: skipped below
        }
        if (strncmp(line, "-----END CERTIFICATE-----", 25) != 0)
            continue;
        inside = false;
        uint32_t end = (uint32_t)f_tell(&f);
        if (len > PEM_MAX || ca_count == CA_MAX) {
            ++bad;
            continue;
        }
        pem[len] = 0;
        mbedtls_x509_crt_init(&crt);
        if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, len + 1) == 0) {
            ca_index[ca_count].hash = hash(crt.subject_raw.p, crt.subject_raw.len);
            ca_index[ca_count].offset = start;
            ca_index[ca_count].len = (uint16_t)(end - start);
            ++ca_count;
        } else {
            ++bad;
        }
        mbedtls_x509_crt_free(&crt);
    }
    free(pem);
    f_close(&f);
    ca_sum = index_sum();
    printf(" %d root certificates", ca_count);
    if (bad)
        printf(" (%d skipped)", bad);
    printf("\n");
    if (!ca_count)
        return -1;
    ca_file_size = info.fsize;
    ca_file_date = info.fdate;
    ca_file_time = info.ftime;
    return ca_count;
}

/// mbedTLS asks for the roots that could have issued `child`.
/// @param unused unused
/// @param child the certificate
/// @param found set to the candidates
/// @return 0, or -1
static int ca_lookup(void *unused, const mbedtls_x509_crt *child, mbedtls_x509_crt **found) {
    *found = NULL;
    uint32_t h = hash(child->issuer_raw.p, child->issuer_raw.len);
    FIL f;
    if (f_open(&f, CA_PATH, FA_READ) != FR_OK)
        return 0;
    for (int i = 0; i < ca_count; ++i) {
        if (ca_index[i].hash != h)
            continue;
        mbedtls_x509_crt *crt = *found;
        if (!crt) {
            crt = malloc(sizeof *crt);
            if (!crt)
                break;
            mbedtls_x509_crt_init(crt);
        }
        if (load_pem(&f, ca_index[i].offset, ca_index[i].len, crt) == 0)
            *found = crt;
        else if (!*found)
            free(crt);
    }
    f_close(&f);
    // Drop any hash collisions: keep only certificates whose subject really
    // is the child's issuer.
    if (*found) {
        mbedtls_x509_crt *c = *found;
        bool match = false;
        for (; c && c->raw.p; c = c->next)
            if (c->subject_raw.len == child->issuer_raw.len &&
                memcmp(c->subject_raw.p, child->issuer_raw.p, c->subject_raw.len) == 0)
                match = true;
        if (!match) {
            mbedtls_x509_crt_free(*found);
            free(*found);
            *found = NULL;
        }
    }
    return 0;
}

// ---- Connections ------------------------------------------------------------------

/// A TLS connection.
struct net_tls {
    net_conn_t *conn;  ///< the TCP connection
    mbedtls_ssl_context ssl;  ///< the session
    mbedtls_ssl_config conf;  ///< its configuration
    mbedtls_ctr_drbg_context drbg;  ///< its random numbers
    mbedtls_entropy_context entropy;  ///< their entropy
};

/// Whether the clock has been set (from the network).
/// @return set
static bool clock_known(void) {
    return time(NULL) > 1700000000;  // after November 2023: set from the network
}

/// Without the real date, a certificate's validity period can't be checked;
/// everything else still is.
/// @param unused unused
/// @param crt the certificate
/// @param depth its depth in the chain
/// @param flags the problems found
/// @return 0
static int check_cert(void *unused, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    if (!clock_known())
        *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

/// mbedTLS's send.
/// @param ctx the connection
/// @param buf the bytes
/// @param len how many
/// @return len, or an error
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    return net_conn_write(ctx, buf, len) ? (int)len : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/// mbedTLS's receive.
/// @param ctx the connection
/// @param buf where
/// @param len how many
/// @return bytes read, WANT_READ, or 0 when closed
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int n = net_conn_read(ctx, buf, len);
    if (n > 0)
        return n;
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : 0;  // 0 = the other end closed
}

/// Prints an mbedTLS error.
/// @param what what failed
/// @param r the error
static void print_error(const char *what, int r) {
    char msg[100];
    mbedtls_strerror(r, msg, sizeof msg);
    printf("%s: %s\n", what, msg);
}

/// Frees a connection.
/// @param t it
static void free_tls(net_tls_t *t) {
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}

bool net_tls_prepare(void) {
    return ca_load_index() >= 0;
}

net_tls_t *net_tls_start(net_conn_t *c, const char *host, bool verify) {
    if (verify && ca_load_index() < 0)
        return NULL;
    if (verify && !clock_known())
        printf("Warning: the clock isn't set yet, so certificate dates aren't checked\n");
    net_tls_t *t = calloc(1, sizeof *t);
    if (!t) {
        printf("Out of memory\n");
        return NULL;
    }
    t->conn = c;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    int r = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                                  (const unsigned char *)"pico-os", 7);
    if (r == 0)
        r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        print_error("TLS setup", r);
        free_tls(t);
        return NULL;
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    mbedtls_ssl_conf_authmode(&t->conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                               : MBEDTLS_SSL_VERIFY_NONE);
    if (verify) {
        mbedtls_ssl_conf_ca_cb(&t->conf, ca_lookup, NULL);
        mbedtls_ssl_conf_verify(&t->conf, check_cert, NULL);
    }
    // Ask for 4 KB records; servers that ignore this still work.
    mbedtls_ssl_conf_max_frag_len(&t->conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (r == 0)
        r = mbedtls_ssl_set_hostname(&t->ssl, host);  // SNI, and the name checked
    if (r != 0) {
        print_error("TLS setup", r);
        free_tls(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, c, bio_send, bio_recv, NULL);

    absolute_time_t until = make_timeout_time_ms(HANDSHAKE_TIMEOUT_MS);
    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
        if (net_stop_key()) {
            printf("Stopped\n");
            free_tls(t);
            return NULL;
        }
        if (time_reached(until)) {
            printf("TLS handshake timed out\n");
            free_tls(t);
            return NULL;
        }
        sleep_ms(1);
    }
    if (r != 0) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
            char info[512];
            mbedtls_x509_crt_verify_info(info, sizeof info, "  ", flags);
            printf("Certificate not trusted:\n%s(wget -k skips the check)\n", info);
        } else {
            print_error("TLS handshake failed", r);
        }
        free_tls(t);
        return NULL;
    }
    printf("TLS: %s, %s\n", mbedtls_ssl_get_version(&t->ssl),
           mbedtls_ssl_get_ciphersuite(&t->ssl));
    return t;
}

int net_tls_read(net_tls_t *t, void *buf, size_t max) {
    int r = mbedtls_ssl_read(&t->ssl, buf, max);
    if (r > 0)
        return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (r != 0 && r != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && r != MBEDTLS_ERR_SSL_CONN_EOF)
        print_error("\nTLS", r);
    return -1;
}

bool net_tls_write(net_tls_t *t, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        int r = mbedtls_ssl_write(&t->ssl, p, len);
        if (r > 0) {
            p += r;
            len -= r;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            print_error("TLS", r);
            return false;
        }
    }
    return true;
}

void net_tls_end(net_tls_t *t) {
    if (!t)
        return;
    mbedtls_ssl_close_notify(&t->ssl);
    free_tls(t);
}
