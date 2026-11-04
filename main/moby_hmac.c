// SPDX-License-Identifier: GPL-3.0-only

#include "moby_hmac.h"

#include "mbedtls/md.h"

bool moby_hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                      uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, key_length, message, message_length, out) == 0;
}
