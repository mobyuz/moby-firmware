set(sources "main.c" "moby_controls.c" "moby_face.c" "moby_font.c" "moby_oled.c")
if(CONFIG_MOBY_F3_USB_LINK)
    list(APPEND sources "moby_config.c" "moby_config_nvs.c" "moby_hmac.c" "moby_json.c" "moby_link.c"
         "moby_link_task.c" "moby_session.c" "moby_transport_usb_jtag.c")
endif()
if(CONFIG_MOBY_F4_PTT_AUDIO)
    list(APPEND sources "moby_capture.c" "moby_pcm.c" "moby_ring.c")
endif()
if(CONFIG_MOBY_F2_MIC_TEST)
    list(APPEND sources "moby_audio.c" "moby_pcm.c")
endif()

idf_component_register(
    SRCS ${sources}
    PRIV_REQUIRES bootloader_support esp_app_format esp_driver_gpio esp_driver_i2c esp_driver_i2s
                  esp_driver_usb_serial_jtag esp_hw_support esp_lcd esp_psram esp_timer json mbedtls moby_protocol
                  nvs_flash spi_flash
)
