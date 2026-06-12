idf_component_register(
    SRCS "weight_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event weight_state weight_config weight_mqtt esp_timer
)

# GCC 14 over-flags strncpy/snprintf truncation in WiFi credential buffers.
# Truncation is intentional and matches the WiFi spec (SSID=32, PW=64).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=stringop-truncation
    -Wno-error=format-truncation
)
