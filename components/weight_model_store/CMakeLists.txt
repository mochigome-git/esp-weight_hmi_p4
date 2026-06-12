idf_component_register(
    SRCS "weight_model_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash weight_state
)
