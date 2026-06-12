idf_component_register(
    SRCS "weight_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s freertos weight_config esp32_p4_wifi6_touch_lcd_x
)
