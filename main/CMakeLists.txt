idf_component_register(
    SRCS "main.c" "moby_controls.c" "moby_face.c" "moby_oled.c"
    PRIV_REQUIRES esp_app_format esp_driver_gpio esp_driver_i2c esp_hw_support esp_lcd esp_psram esp_timer spi_flash
)
