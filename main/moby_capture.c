// SPDX-License-Identifier: GPL-3.0-only

#include "moby_capture.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "moby_pins.h"
#include "moby_ring.h"

// Nothing in this file logs a sample, a level or a buffer: lines carry error names only.
static const char *const TAG = "moby_capture";

enum {
    // 64 blocks of 20 ms: 1.28 s, 40 KiB in PSRAM. A longer stall of the USB host is an overrun and ends the stream.
    RING_BLOCKS = 64,
    RING_SAMPLES = RING_BLOCKS * MOBY_PCM_FRAME_SAMPLES,
    READ_TIMEOUT_MS = 30,
    LOCK_WAIT_MS = 5,
    // The button must read released for this many blocks (120 ms) before the task stops on its own.
    TALK_RELEASED_BLOCKS = 6,
    // Longer than the protocol's longest utterance (60 s): a last resort if the session never stops the stream.
    HARD_LIMIT_US = 65LL * 1000 * 1000,
};

static SemaphoreHandle_t lock;
static TaskHandle_t task;
static moby_ring_t ring;
static int16_t *storage;
static bool hw_running;
static uint32_t events;
static moby_pcm_source_t source;

static void lock_take(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void lock_give(void)
{
    xSemaphoreGive(lock);
}

// The red LED is lit while the sample gate is open or the microphone clocks run, so it is never dark while the
// microphone can hear. Call with the lock held.
static void refresh_led(void)
{
    gpio_set_level(MOBY_PIN_RED_LED, ring.open || hw_running ? 1 : 0);
}

static bool muted_by_switch(void)
{
    return gpio_get_level(MOBY_PIN_MUTE) != 0;
}

static bool gate_open(void)
{
    lock_take();
    const bool open = ring.open;
    lock_give();
    return open;
}

// True while the microphone may run: the session wants it and the switch lets the path through. The switch is read
// here directly, so the I2S clocks stop even if the session has not heard about the mute yet.
static bool may_run(void)
{
    return gate_open() && !muted_by_switch();
}

static void zero(volatile void *buffer, size_t size)
{
    volatile uint8_t *byte = buffer;
    while (size-- != 0) {
        *byte++ = 0;
    }
}

static void post_event(uint32_t bits)
{
    lock_take();
    events |= bits;
    lock_give();
}

static bool run_capture(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = MOBY_PCM_BLOCK_FRAMES;
    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MOBY_PCM_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MOBY_PIN_I2S_BCLK,
            .ws = MOBY_PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MOBY_PIN_I2S_DIN,
        },
    };
    i2s_chan_handle_t rx = NULL;
    esp_err_t error = i2s_new_channel(&channel_config, NULL, &rx);
    if (error == ESP_OK) {
        error = i2s_channel_init_std_mode(rx, &std_config);
    }
    bool ok = error == ESP_OK;
    if (ok && may_run()) {
        // The LED is already lit by capture_set; the clocks start only now.
        error = i2s_channel_enable(rx);
        ok = error == ESP_OK;
        if (ok) {
            lock_take();
            hw_running = true;
            refresh_led();
            lock_give();
        }
    } else if (ok) {
        error = ESP_OK;
    }

    uint32_t stereo[MOBY_PCM_BLOCK_FRAMES * MOBY_I2S_SLOTS];
    int16_t mono[MOBY_PCM_BLOCK_FRAMES];
    unsigned released = 0;
    const int64_t deadline = esp_timer_get_time() + HARD_LIMIT_US;
    while (ok && hw_running && may_run()) {
        size_t bytes_read = 0;
        error = i2s_channel_read(rx, stereo, sizeof stereo, &bytes_read, READ_TIMEOUT_MS);
        if (error != ESP_OK || bytes_read != sizeof stereo) {
            ok = false;
            break;
        }
        released = gpio_get_level(MOBY_PIN_TALK) != 0 ? released + 1 : 0;
        // Look again after the read: a block that finished just as the mute or the stop came is thrown away.
        if (!may_run() || released >= TALK_RELEASED_BLOCKS) {
            break;
        }
        moby_pcm_convert(stereo, mono, MOBY_PCM_BLOCK_FRAMES);
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(LOCK_WAIT_MS)) == pdTRUE) {
            moby_ring_write(&ring, mono, MOBY_PCM_BLOCK_FRAMES);
            lock_give();
        } else {
            // The link is holding the ring: this block is lost and the stream ends as an overrun.
            post_event(MOBY_PCM_EVENT_OVERRUN);
        }
        if (esp_timer_get_time() > deadline) {
            ok = false;
            break;
        }
    }
    zero(stereo, sizeof stereo);
    zero(mono, sizeof mono);

    if (rx) {
        if (hw_running) {
            i2s_channel_disable(rx);
        }
        i2s_del_channel(rx);
    }
    lock_take();
    hw_running = false;
    refresh_led();
    lock_give();
    if (!ok) {
        ESP_LOGE(TAG, "i2s=%s", esp_err_to_name(error == ESP_OK ? ESP_FAIL : error));
        post_event(MOBY_PCM_EVENT_FAULT);
    }
    return ok;
}

static void capture_task(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // One attempt per request: a failed start is reported once and waits for the next press.
        if (may_run()) {
            run_capture();
        }
    }
}

void moby_capture_set(void *ctx, bool on)
{
    (void)ctx;
    if (!lock) {
        return;
    }
    lock_take();
    if (on) {
        events = 0;
        moby_ring_open(&ring, MOBY_PCM_WARMUP_SAMPLES);
    } else {
        moby_ring_close(&ring);
    }
    refresh_led();
    lock_give();
    if (on && task) {
        xTaskNotifyGive(task);
    }
}

void moby_capture_discard(void *ctx)
{
    (void)ctx;
    if (!lock) {
        return;
    }
    lock_take();
    moby_ring_clear(&ring);
    events = 0;
    refresh_led();
    lock_give();
}

static size_t source_peek(void *ctx, int16_t *out, size_t samples)
{
    (void)ctx;
    lock_take();
    const size_t got = moby_ring_peek(&ring, out, samples);
    lock_give();
    return got;
}

static void source_consume(void *ctx, size_t samples)
{
    (void)ctx;
    lock_take();
    moby_ring_consume(&ring, samples);
    lock_give();
}

static uint32_t source_events(void *ctx)
{
    (void)ctx;
    lock_take();
    uint32_t bits = events | (moby_ring_take_overrun(&ring) ? MOBY_PCM_EVENT_OVERRUN : 0);
    events = 0;
    lock_give();
    return bits;
}

const moby_pcm_source_t *moby_capture_source(void)
{
    return &source;
}

esp_err_t moby_capture_init(void)
{
    // The ring lives in PSRAM only: with none, there is no audio path (the session never opens a stream).
    storage = heap_caps_calloc(RING_SAMPLES, sizeof *storage, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        return ESP_ERR_NO_MEM;
    }
    lock = xSemaphoreCreateMutex();
    if (!lock) {
        return ESP_ERR_NO_MEM;
    }
    moby_ring_init(&ring, storage, RING_SAMPLES);
    source = (moby_pcm_source_t){ .peek = source_peek, .consume = source_consume, .take_events = source_events };
    if (xTaskCreate(capture_task, "moby_capture", 8192, NULL, 5, &task) != pdPASS) {
        task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
