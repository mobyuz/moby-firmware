// SPDX-License-Identifier: GPL-3.0-only

#include "moby_config.h"

#include <string.h>

static const char KEY_DEVICE_ID[] = "device_id";
static const char KEY_PAIR_KEY[] = "pair_key";

static bool device_id_valid(const char *text, size_t length)
{
    return length == MOBY_DEVICE_ID_LEN && memcmp(text, "dev_", 4) == 0 && moby_stable_id_valid(text);
}

static moby_config_status_t make_device_id(moby_stored_t *stored, const moby_store_t *store, moby_random_fn random,
                                           void *random_ctx)
{
    uint8_t bytes[16];
    if (!random || !random(random_ctx, bytes, sizeof bytes)) {
        return MOBY_CONFIG_NO_RANDOM;
    }
    memcpy(stored->device_id, "dev_", 4);
    moby_hex_encode(bytes, sizeof bytes, stored->device_id + 4);
    stored->device_id[MOBY_DEVICE_ID_LEN] = '\0';
    if (!store->write(store->ctx, KEY_DEVICE_ID, stored->device_id, MOBY_DEVICE_ID_LEN)) {
        return MOBY_CONFIG_STORE_FAILED;
    }
    return MOBY_CONFIG_OK;
}

moby_config_status_t moby_config_load(moby_stored_t *stored, const moby_store_t *store, moby_random_fn random, void *random_ctx)
{
    memset(stored, 0, sizeof *stored);
    size_t size = MOBY_DEVICE_ID_LEN;
    char id[MOBY_DEVICE_ID_LEN + 1] = { 0 };
    const moby_store_status_t id_status = store->read(store->ctx, KEY_DEVICE_ID, id, &size);
    if (id_status == MOBY_STORE_FAIL) {
        return MOBY_CONFIG_STORE_FAILED;
    }
    if (id_status == MOBY_STORE_OK && device_id_valid(id, size)) {
        memcpy(stored->device_id, id, MOBY_DEVICE_ID_LEN);
    } else {
        const moby_config_status_t made = make_device_id(stored, store, random, random_ctx);
        if (made != MOBY_CONFIG_OK) {
            return made;
        }
    }

    size = sizeof stored->pair_key;
    const moby_store_status_t key_status = store->read(store->ctx, KEY_PAIR_KEY, stored->pair_key, &size);
    if (key_status == MOBY_STORE_FAIL) {
        return MOBY_CONFIG_STORE_FAILED;
    }
    if (key_status == MOBY_STORE_OK && size == sizeof stored->pair_key) {
        stored->paired = true;
    } else {
        memset(stored->pair_key, 0, sizeof stored->pair_key);
        if (key_status == MOBY_STORE_OK && !store->erase(store->ctx, KEY_PAIR_KEY)) {
            return MOBY_CONFIG_STORE_FAILED;
        }
    }
    return MOBY_CONFIG_OK;
}

bool moby_config_store_key(const moby_store_t *store, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES])
{
    return store->write(store->ctx, KEY_PAIR_KEY, key, MOBY_USB_PAIR_KEY_BYTES);
}

bool moby_config_erase_key(const moby_store_t *store)
{
    return store->erase(store->ctx, KEY_PAIR_KEY);
}
