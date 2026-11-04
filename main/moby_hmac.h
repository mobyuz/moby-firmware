// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_HMAC_H
#define MOBY_HMAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// HMAC-SHA256 through mbedTLS. The device and the host tests compile this same
// file, so the proof vectors exercise the code the firmware runs.
bool moby_hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                      uint8_t out[32]);

#endif
