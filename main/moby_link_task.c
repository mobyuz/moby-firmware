// SPDX-License-Identifier: GPL-3.0-only

#include "moby_link_task.h"

#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "moby_config.h"
#include "moby_config_nvs.h"
#include "moby_hmac.h"
#include "moby_link.h"
#include "moby_session.h"
#include "moby_transport_usb_jtag.h"

static const char *const TAG = "moby_link";

// One lock guards the session. The link task holds it for a pass, which never waits on the host.
static SemaphoreHandle_t lock;
static moby_store_t store;
static moby_transport_t transport;
static moby_session_t session;
static moby_link_t usb_link;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool op_random(void *ctx, uint8_t *out, size_t length)
{
    (void)ctx;
    esp_fill_random(out, length);
    return true;
}

static bool op_hmac(void *ctx, const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                    uint8_t out[32])
{
    (void)ctx;
    return moby_hmac_sha256(key, key_length, message, message_length, out);
}

static bool op_store_key(void *ctx, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES])
{
    (void)ctx;
    return moby_config_store_key(&store, key);
}

static bool op_erase_key(void *ctx)
{
    (void)ctx;
    return moby_config_erase_key(&store);
}

// Session notes carry state names only, never a key, a nonce or a proof.
static void op_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

static void link_task(void *unused)
{
    (void)unused;
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        moby_link_step(&usb_link, now_ms());
        xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Copies text into a field that must be 1-32 printable ASCII characters without a space.
static void version_text(char out[MOBY_USB_VERSION_TEXT_MAX_CHARS + 1], const char *text)
{
    size_t length = 0;
    for (; text[length] && length < MOBY_USB_VERSION_TEXT_MAX_CHARS; ++length) {
        out[length] = text[length] > 0x20 && text[length] < 0x7f ? text[length] : '?';
    }
    if (length == 0) {
        out[length++] = '?';
    }
    out[length] = '\0';
}

esp_err_t moby_link_task_start(void)
{
    ESP_RETURN_ON_ERROR(moby_config_nvs_open(&store), TAG, "open NVS");
    // Draw entropy from the SAR ADC noise source for the whole run. The ADC and the RF
    // subsystem stay unused in this firmware, so nothing else contends for it.
    bootloader_random_enable();

    moby_stored_t stored;
    const moby_config_status_t loaded = moby_config_load(&stored, &store, op_random, NULL);
    ESP_RETURN_ON_FALSE(loaded == MOBY_CONFIG_OK, ESP_FAIL, TAG, "identity unavailable (%d)", (int)loaded);

    moby_identity_t identity = { 0 };
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    memcpy(identity.device_id, stored.device_id, sizeof identity.device_id);
    snprintf(identity.hw_rev, sizeof identity.hw_rev, "esp32s3-v%u.%u", (unsigned)(chip.revision / 100),
             (unsigned)(chip.revision % 100));
    version_text(identity.fw_version, esp_app_get_description()->version);
    // No audio and no servo in this stage, so the default image advertises no capability.
    identity.capabilities = 0;

    ESP_RETURN_ON_ERROR(moby_transport_usb_jtag_init(&transport), TAG, "USB-Serial-JTAG driver");
    const moby_session_ops_t ops = {
        .fill_random = op_random,
        .hmac_sha256 = op_hmac,
        .store_key = op_store_key,
        .erase_key = op_erase_key,
        .log = op_log,
    };
    moby_session_init(&session, &identity, stored.paired ? stored.pair_key : NULL, &ops, now_ms());
    memset(&stored.pair_key, 0, sizeof stored.pair_key);
    moby_link_init(&usb_link, &transport, &session);

    lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG, "lock");
    ESP_RETURN_ON_FALSE(xTaskCreate(link_task, "moby_link", 6144, NULL, 3, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "paired=%u transport=usb_serial_jtag", (unsigned)session.paired);
    return ESP_OK;
}

void moby_link_task_controls(const moby_controls_t *controls, uint32_t now)
{
    if (lock && xSemaphoreTake(lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        moby_session_controls(&session, now, controls->muted, controls->talk_down, controls->attention);
        xSemaphoreGive(lock);
    }
}

bool moby_link_task_view(moby_face_view_t *view)
{
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    moby_session_view(&session, view);
    xSemaphoreGive(lock);
    return true;
}
