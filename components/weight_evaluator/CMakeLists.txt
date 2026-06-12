idf_component_register(
    SRCS "weight_evaluator.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos weight_state weight_config weight_uart weight_audio weight_mqtt
)
