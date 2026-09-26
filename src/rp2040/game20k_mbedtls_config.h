/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 scullymi */
/** @file game20k_mbedtls_config.h
 *  @brief mbedTLS configuration for the connection to retroachievements.org.
 *
 *  A TLS 1.2 client only, ECDHE-ECDSA with AES-GCM, curves P-256 (server key)
 *  and P-384 (GTS Root R4). MBEDTLS_DEBUG_C stays off, its __FILE__ strings
 *  would put the build paths into the flash. */
#ifndef GAME20K_MBEDTLS_CONFIG_H
#define GAME20K_MBEDTLS_CONFIG_H

/** @name Platform
 *  Entropy from the RP2350 hardware, dates from the NTP clock. */
/** @{ */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_NO_PLATFORM_ENTROPY     /* no /dev/urandom */
#define MBEDTLS_ENTROPY_HARDWARE_ALT    /* mbedtls_hardware_poll from pico_mbedtls */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE          /* certificate dates, needs the NTP clock */
#define MBEDTLS_PLATFORM_MS_TIME_ALT    /* mbedtls_ms_time() in ra_net.c */
/** @} */

/** @name TLS 1.2 client
 *  ECDHE-ECDSA with the server name (SNI), nothing else. */
/** @{ */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* the server needs SNI */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
/** @} */

/** @name Crypto
 *  P-256 (server key), P-384 (the chain up to GTS Root R4), AES-GCM, SHA-2. */
/** @{ */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
/** @} */

/** @name Certificates
 *  X.509 for the chain, PEM for the anchor in ra_ca.h. */
/** @{ */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C             /* the anchor is PEM */
/** @} */

#endif /* GAME20K_MBEDTLS_CONFIG_H */
