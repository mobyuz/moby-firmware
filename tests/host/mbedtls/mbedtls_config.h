/* SPDX-License-Identifier: GPL-3.0-only */
/* A minimal mbedTLS configuration for the host tests: only what HMAC-SHA256 needs. */
#ifndef MOBY_HOST_MBEDTLS_CONFIG_H
#define MOBY_HOST_MBEDTLS_CONFIG_H

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_NO_PLATFORM_ENTROPY

#endif
