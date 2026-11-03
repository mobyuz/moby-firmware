// SPDX-License-Identifier: GPL-3.0-only

#include "moby_audio.h"

#include <inttypes.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "moby_pcm.h"
#include "moby_pins.h"

static const char *const TAG = "moby_audio";

static moby_capture_state_t capture_state(uint32_t request)
{
    const uint32_t latest = ulTaskNotifyValueClear(NULL, 0);
    return moby_audio_request_state(request, latest, gpio_get_level(MOBY_PIN_MUTE) != 0);
}

static void clear_samples(void *buffer, size_t size)
{
    volatile uint8_t *byte = buffer;
    while (size-- != 0) {
        *byte++ = 0;
    }
}

static esp_err_t read_capture(i2s_chan_handle_t rx, uint32_t request, moby_pcm_stats_t *stats)
{
    enum { READ_TIMEOUT_MS = 30, CAPTURE_LIMIT_US = 5000000 };
    // 2^18 BCLK cycles at 1.024 MHz are 256 ms. Discard 13 full 20 ms blocks.
    unsigned warmup_blocks = 13;
    uint32_t stereo[MOBY_PCM_BLOCK_FRAMES * MOBY_I2S_SLOTS];
    int16_t mono[MOBY_PCM_BLOCK_FRAMES];
    const int64_t deadline = esp_timer_get_time() + CAPTURE_LIMIT_US;
    esp_err_t error = ESP_OK;

    while (capture_state(request) == MOBY_CAPTURE_ACTIVE && esp_timer_get_time() < deadline) {
        size_t bytes_read = 0;
        error = i2s_channel_read(rx, stereo, sizeof stereo, &bytes_read, READ_TIMEOUT_MS);
        if (capture_state(request) != MOBY_CAPTURE_ACTIVE) {
            error = ESP_OK;
            break;
        }
        if (error != ESP_OK) {
            break;
        }
        if (bytes_read != sizeof stereo) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (warmup_blocks != 0) {
            --warmup_blocks;
            continue;
        }
        moby_pcm_from_i2s(stereo, mono, MOBY_PCM_BLOCK_FRAMES);
        moby_pcm_measure(stats, mono, MOBY_PCM_BLOCK_FRAMES);
    }

    clear_samples(stereo, sizeof stereo);
    clear_samples(mono, sizeof mono);
    return error;
}

static void capture(uint32_t request)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = MOBY_PCM_BLOCK_FRAMES;
    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MOBY_PCM_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_4,
            .ws = GPIO_NUM_5,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_6,
        },
    };

    i2s_chan_handle_t rx = NULL;
    esp_err_t error = i2s_new_channel(&channel_config, NULL, &rx);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "f2=error i2s=%s", esp_err_to_name(error));
        return;
    }

    moby_pcm_stats_t stats = {0};
    error = i2s_channel_init_std_mode(rx, &std_config);
    if (error == ESP_OK && capture_state(request) == MOBY_CAPTURE_ACTIVE) {
        // Indicate capture before clocks start and keep the LED on until I2S stops.
        error = gpio_set_level(MOBY_PIN_RED_LED, 1);
        if (error == ESP_OK) {
            error = i2s_channel_enable(rx);
        }
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "f2=capturing");
            error = read_capture(rx, request, &stats);
            const esp_err_t stop_error = i2s_channel_disable(rx);
            if (error == ESP_OK) {
                error = stop_error;
            }
        }
        gpio_set_level(MOBY_PIN_RED_LED, 0);
    }
    const esp_err_t delete_error = i2s_del_channel(rx);
    if (error == ESP_OK) {
        error = delete_error;
    }

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "f2=error i2s=%s", esp_err_to_name(error));
    } else if (capture_state(request) == MOBY_CAPTURE_CANCELLED) {
        ESP_LOGI(TAG, "f2=cancelled");
    } else if (stats.samples == 0) {
        ESP_LOGI(TAG, "f2=too_short");
    } else {
        ESP_LOGI(TAG, "f2=done samples=%" PRIu32 " peak=%" PRIu32 " rms=%" PRIu32 " full_scale=%" PRIu32,
                 stats.samples, stats.peak, moby_pcm_rms(&stats), stats.full_scale);
    }
}

void moby_audio_task(void *unused)
{
    (void)unused;
    uint32_t request;
    for (;;) {
        if (xTaskNotifyWait(0, 0, &request, portMAX_DELAY) == pdTRUE &&
            (request & MOBY_AUDIO_COMMAND_MASK) == MOBY_AUDIO_PRESS &&
            capture_state(request) == MOBY_CAPTURE_ACTIVE) {
            capture(request);
        }
    }
}
