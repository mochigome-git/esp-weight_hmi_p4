idf_component_register(
    SRCS
        "weight_ui.c"
        "screens/screen_main.c"
        "screens/screen_models.c"
        "screens/screen_model_edit.c"
        "screens/screen_settings.c"
        "screens/screen_wifi.c"
        "screens/screen_mqtt.c"
        "screens/ui_loading.c"
        "screens/screen_keyboard.c"
        "i18n/weight_i18n.c"   
        "fonts/lv_font_montserrat_bold_178.c"
        "fonts/lv_font_nato_sans_jp_bold_14.c"
        "fonts/lv_font_nato_sans_jp_bold_16.c"
        "fonts/lv_font_nato_sans_jp_bold_24.c"
    INCLUDE_DIRS "include"  "i18n"  
    PRIV_INCLUDE_DIRS "screens"
    REQUIRES
        esp_timer
        esp32_p4_wifi6_touch_lcd_x   # Waveshare BSP - provides bsp_display_*, lvgl, etc.
        weight_state
        weight_config
        weight_model_store
        weight_wifi
        weight_mqtt
        weight_audio
)
