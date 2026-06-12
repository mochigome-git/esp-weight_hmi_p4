idf_component_register(
    SRCS "weight_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES weight_state weight_config weight_model_store
    EMBED_TXTFILES "../../certs/emqxsl-ca.crt"
)
