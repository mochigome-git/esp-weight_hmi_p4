idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_system
        esp_event
        esp_hosted 
        esp_wifi_remote
        esp_wifi
        esp32_p4_wifi6_touch_lcd_x
        weight_state
        weight_config
        weight_uart
        weight_evaluator
        weight_model_store
        weight_mqtt
        weight_audio
        weight_wifi
        weight_ui
        weight_time
)

# Pack /spiffs folder into the 'storage' partition image at flash time.
# Drop your WAV files into the project-level spiffs/ folder; they are
# auto-flashed to /spiffs at runtime.
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
