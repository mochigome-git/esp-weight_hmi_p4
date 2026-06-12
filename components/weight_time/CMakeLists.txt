idf_component_register(
    SRCS "weight_time.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif
    PRIV_REQUIRES esp_event log
)
