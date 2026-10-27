/**
 * @file mbedtls_config.h
 * @brief mbedTLS 3.6 settings for wget's HTTPS (a TLS 1.2 client only, with the key exchanges and ciphers today's web servers offer) and for scp's SSH (ssh.c).
 *
 * Certificates are checked against CACERT.PEM on the drive (tls.c). Entropy
 * comes from the RP2350's TRNG through pico_rand (mbedtls_hardware_poll in
 * pico_mbedtls.c).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

// ---- Platform ----------------------------------------------------------------
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_MS_TIME_ALT     ///< mbedtls_ms_time() in tls.c
#define MBEDTLS_PLATFORM_GMTIME_R_ALT    ///< mbedtls_platform_gmtime_r() in tls.c
#define MBEDTLS_NO_PLATFORM_ENTROPY
/// mbedTLS's buffers (40 KB and more for a TLS handshake) come from PSRAM:
/// SRAM is scarce, and malloc panics when it runs out (tls.c).
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO   tls_free
#include <stddef.h>
void *tls_calloc(size_t count, size_t size);
void tls_free(void *p);
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ERROR_C

// ---- TLS ---------------------------------------------------------------------
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
/// A whole 16 KB record must fit coming in (servers needn't honour the
/// max_fragment_length request); what we send is small.
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

// ---- Ciphers and hashes --------------------------------------------------------
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES     ///< tables in flash, not RAM
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR    ///< ssh: aes128-ctr, aes256-ctr
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C   ///< SMB: NTLMv2 (HMAC-MD5)
#define MBEDTLS_CMAC_C  ///< SMB 3 signing (AES-CMAC)
#define MBEDTLS_SHA1_C             ///< older certificate signatures
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

// ---- Public keys ---------------------------------------------------------------
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0  ///< saves RAM
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// ---- Certificates --------------------------------------------------------------
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK  ///< roots are looked up on the drive
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
