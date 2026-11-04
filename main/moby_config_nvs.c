// SPDX-License-Identifier: GPL-3.0-only

#include "moby_config_nvs.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *const TAG = "moby_config";

static moby_store_status_t store_read(void *ctx, const char *name, void *buffer, size_t *size)
{
    size_t length = *size;
    const esp_err_t error = nvs_get_blob(*(nvs_handle_t *)ctx, name, buffer, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return MOBY_STORE_MISSING;
    }
    // A blob larger than the buffer reports its real length, which the caller treats as invalid.
    if (error != ESP_OK && error != ESP_ERR_NVS_INVALID_LENGTH) {
        return MOBY_STORE_FAIL;
    }
    *size = length;
    return MOBY_STORE_OK;
}

static bool store_write(void *ctx, const char *name, const void *data, size_t size)
{
    nvs_handle_t handle = *(nvs_handle_t *)ctx;
    return nvs_set_blob(handle, name, data, size) == ESP_OK && nvs_commit(handle) == ESP_OK;
}

static bool store_erase(void *ctx, const char *name)
{
    nvs_handle_t handle = *(nvs_handle_t *)ctx;
    const esp_err_t error = nvs_erase_key(handle, name);
    return (error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
}

esp_err_t moby_config_nvs_open(moby_store_t *store)
{
    static nvs_handle_t handle;
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // An unreadable partition holds no usable identity either: start over, and the robot is unpaired.
        ESP_LOGW(TAG, "nvs reset: %s", esp_err_to_name(error));
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_open("moby", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    *store = (moby_store_t){ .ctx = &handle, .read = store_read, .write = store_write, .erase = store_erase };
    return ESP_OK;
}
