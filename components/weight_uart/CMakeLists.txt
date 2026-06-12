idf_component_register(
    SRCS "weight_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart freertos weight_config weight_state
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=stringop-truncation
    -Wno-error=format-truncation
)