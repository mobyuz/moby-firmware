// SPDX-License-Identifier: GPL-3.0-only

#include "moby_oled.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "moby_face.h"

static const char *const TAG = "moby_oled";

static esp_err_t run_oled(QueueHandle_t views)
{
    enum { PRIMARY_ADDRESS = 0x3c, SECONDARY_ADDRESS = 0x3d, PROBE_MS = 30 };
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "create I2C bus");

    uint8_t address = PRIMARY_ADDRESS;
    esp_err_t ret = i2c_master_probe(bus, address, PROBE_MS);
    if (ret == ESP_ERR_NOT_FOUND) {
        address = SECONDARY_ADDRESS;
        ret = i2c_master_probe(bus, address, PROBE_MS);
    }
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "probe SSD1306");

    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = address,
        .scl_speed_hz = 100000,
        .transaction_timeout_ms = 150,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306 = { .height = MOBY_FACE_HEIGHT };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &ssd1306,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io), cleanup, TAG, "create OLED IO");
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ssd1306(io, &panel_config, &panel), cleanup, TAG, "create panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), cleanup, TAG, "reset panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), cleanup, TAG, "initialize panel");

    uint8_t frame[MOBY_FACE_BYTES];
    moby_face_view_t view;
    xQueuePeek(views, &view, portMAX_DELAY);
    moby_face_render(frame, view, false);
    ESP_GOTO_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, MOBY_FACE_WIDTH, MOBY_FACE_HEIGHT, frame),
                      cleanup, TAG, "draw initial frame");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), cleanup, TAG, "turn on OLED");
    ESP_LOGI(TAG, "ssd1306_address=0x%02x", (unsigned)address);

    bool blink = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        moby_face_view_t next = view;
        xQueuePeek(views, &next, 0);
        const bool blink_next = !next.attention &&
            (next.expression == MOBY_FACE_DISCONNECTED || next.expression == MOBY_FACE_IDLE) &&
            esp_timer_get_time() / 1000 % 3000 >= 2800;
        if (next.expression == view.expression && next.attention == view.attention && blink_next == blink) {
            continue;
        }
        view = next;
        blink = blink_next;
        moby_face_render(frame, view, blink);
        ESP_GOTO_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, MOBY_FACE_WIDTH, MOBY_FACE_HEIGHT, frame),
                          cleanup, TAG, "draw frame");
    }

cleanup:
    if (panel) {
        esp_lcd_panel_del(panel);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    i2c_del_master_bus(bus);
    return ret;
}

void moby_oled_task(void *view_queue)
{
    esp_err_t error = run_oled(view_queue);
    ESP_LOGW(TAG, "OLED unavailable: %s", esp_err_to_name(error));
    vTaskDelete(NULL);
}
