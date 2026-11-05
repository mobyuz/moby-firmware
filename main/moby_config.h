// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_CONFIG_H
#define MOBY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_msg.h"

// The identity and the pairing key a robot keeps across resets. Storage is
// behind a small interface: moby_config_nvs.c backs it with NVS on the device
// and the host tests use RAM.

typedef enum {
    MOBY_STORE_OK,
    MOBY_STORE_MISSING,
    MOBY_STORE_FAIL,
} moby_store_status_t;

typedef struct {
    void *ctx;
    // size is the buffer capacity on entry and the stored length on return.
    moby_store_status_t (*read)(void *ctx, const char *name, void *buffer, size_t *size);
    bool (*write)(void *ctx, const char *name, const void *data, size_t size);
    bool (*erase)(void *ctx, const char *name);
} moby_store_t;

typedef struct {
    char device_id[MOBY_DEVICE_ID_LEN + 1];
    bool paired;
    uint8_t pair_key[MOBY_USB_PAIR_KEY_BYTES];
} moby_stored_t;

typedef enum {
    MOBY_CONFIG_OK,
    MOBY_CONFIG_NO_RANDOM,
    MOBY_CONFIG_STORE_FAILED,
} moby_config_status_t;

typedef bool (*moby_random_fn)(void *ctx, uint8_t *out, size_t length);

// Loads the identity. On the first boot it makes a random 128-bit device_id
// ("dev_" and 32 lowercase hex digits) and stores it. A stored key of the wrong
// length is erased and the robot counts as unpaired.
moby_config_status_t moby_config_load(moby_stored_t *stored, const moby_store_t *store, moby_random_fn random, void *random_ctx);

bool moby_config_store_key(const moby_store_t *store, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES]);
bool moby_config_erase_key(const moby_store_t *store);

#endif
