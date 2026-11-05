// SPDX-License-Identifier: GPL-3.0-only

#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "moby_audio.h"
#include "moby_controls.h"
#include "moby_face.h"
#ifdef CONFIG_MOBY_F3_USB_LINK
#include "moby_link_task.h"
#endif
#include "moby_oled.h"
#include "moby_pins.h"
#include "sdkconfig.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 6)
#error "Moby firmware requires ESP-IDF v5.5.6"
#endif

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "Moby firmware requires the esp32s3 target"
#endif

static const char *const TAG = "moby";

static esp_err_t init_controls(void)
{
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << MOBY_PIN_TALK) | (1ULL << MOBY_PIN_MUTE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t output = {
        .pin_bit_mask = 1ULL << MOBY_PIN_RED_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(MOBY_PIN_RED_LED, 0), TAG, "preload red LED off");
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "configure red LED");
    return gpio_config(&inputs);
}

static void run_controls(void)
{
    esp_err_t error = init_controls();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "f1=fail gpio=%s", esp_err_to_name(error));
        return;
    }

    moby_controls_t controls;
    moby_controls_init(&controls, gpio_get_level(MOBY_PIN_MUTE) != 0, gpio_get_level(MOBY_PIN_TALK) == 0,
                       (uint32_t)(esp_timer_get_time() / 1000));
    QueueHandle_t views = xQueueCreate(1, sizeof(moby_face_view_t));
    if (views) {
        const moby_face_view_t initial = { .expression = MOBY_FACE_MUTED };
        xQueueOverwrite(views, &initial);
        if (xTaskCreate(moby_oled_task, "moby_oled", 4096, views, 4, NULL) != pdPASS) {
            vQueueDelete(views);
            views = NULL;
        }
    }
    if (!views) {
        ESP_LOGW(TAG, "OLED task unavailable");
    }
    ESP_LOGI(TAG, "f1=ready muted=1 talk=%s", controls.talk_down ? "down" : "up");
#ifdef CONFIG_MOBY_F3_USB_LINK
    // The face and the controls do not depend on the link. If it fails to start, F1 behaviour stays.
    const esp_err_t link_error = moby_link_task_start();
    if (link_error == ESP_OK) {
        ESP_LOGI(TAG, "f3=ready transport=usb_serial_jtag");
    } else {
        ESP_LOGE(TAG, "f3=fail link=%s", esp_err_to_name(link_error));
    }
    moby_face_view_t shown = { .expression = MOBY_FACE_MUTED };
#endif
#ifdef CONFIG_MOBY_F1_LED_TEST
    ESP_LOGW(TAG, "F1 red LED bench test enabled");
#endif
#ifdef CONFIG_MOBY_F2_MIC_TEST
    TaskHandle_t audio = NULL;
    uint32_t audio_event = 0;
    if (xTaskCreate(moby_audio_task, "moby_audio", 8192, NULL, 3, &audio) == pdPASS) {
        ESP_LOGI(TAG, "f2=ready");
    } else {
        ESP_LOGE(TAG, "f2=error audio task unavailable");
    }
#endif

    for (;;) {
        const bool was_muted = controls.muted;
        const bool was_down = controls.talk_down;
        const bool had_attention = controls.attention;
        moby_controls_update(&controls, gpio_get_level(MOBY_PIN_MUTE) != 0, gpio_get_level(MOBY_PIN_TALK) == 0,
                             (uint32_t)(esp_timer_get_time() / 1000));
#ifdef CONFIG_MOBY_F1_LED_TEST
        if (controls.attention != had_attention) {
            gpio_set_level(MOBY_PIN_RED_LED, controls.attention);
        }
#endif
#ifdef CONFIG_MOBY_F2_MIC_TEST
        if (audio && controls.attention != had_attention) {
            uint32_t command = MOBY_AUDIO_RELEASE;
            if (controls.attention) {
                command = MOBY_AUDIO_PRESS;
            } else if (controls.muted) {
                command = MOBY_AUDIO_MUTE;
            }
            audio_event += MOBY_AUDIO_EVENT_STEP;
            xTaskNotify(audio, audio_event | command, eSetValueWithOverwrite);
        }
#endif
        if (controls.muted != was_muted) {
            ESP_LOGI(TAG, "muted=%u", (unsigned)controls.muted);
        }
        if (controls.talk_down != was_down) {
            ESP_LOGI(TAG, "talk=%s", controls.talk_down ? "down" : "up");
        }
#ifdef CONFIG_MOBY_F3_USB_LINK
        (void)had_attention;
        // The hardware mute reaches the session and the face first; nothing the desktop sends can undo it.
        moby_link_task_controls(&controls, (uint32_t)(esp_timer_get_time() / 1000));
        moby_face_view_t view = {
            .expression = controls.muted ? MOBY_FACE_MUTED : MOBY_FACE_DISCONNECTED,
            .attention = controls.attention,
        };
        moby_face_view_t from_session;
        if (moby_link_task_view(&from_session)) {
            view = from_session;
        }
        if (controls.muted) {
            view = (moby_face_view_t){ .expression = MOBY_FACE_MUTED };
        }
        if (views && !moby_face_view_equal(&view, &shown)) {
            xQueueOverwrite(views, &view);
            shown = view;
        }
#else
        if (views && (controls.muted != was_muted || controls.attention != had_attention)) {
            const moby_face_view_t view = {
                .expression = controls.muted ? MOBY_FACE_MUTED : MOBY_FACE_DISCONNECTED,
                .attention = controls.attention,
            };
            xQueueOverwrite(views, &view);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "firmware=%s idf=%s chip=%s cores=%u revision=%u.%u",
             app->version, app->idf_ver, CONFIG_IDF_TARGET, (unsigned)chip.cores,
             (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100));

    const size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "psram_bytes=%zu", psram_bytes);

    uint32_t flash_bytes = 0;
    const esp_err_t flash_error = esp_flash_get_physical_size(NULL, &flash_bytes);
    if (flash_error != ESP_OK) {
        ESP_LOGE(TAG, "f0=fail flash_probe=%s", esp_err_to_name(flash_error));
        return;
    }
    ESP_LOGI(TAG, "physical_flash_bytes=%" PRIu32, flash_bytes);

    const uint32_t mib = 1024U * 1024U;
    if (psram_bytes != 8U * mib || (flash_bytes != 16U * mib && flash_bytes != 8U * mib)) {
        ESP_LOGE(TAG, "f0=fail unsupported_memory_profile");
        return;
    }

    ESP_LOGI(TAG, "f0=pass memory_profile=%s", flash_bytes == 16U * mib ? "N16R8" : "N8R8");
    run_controls();
}
