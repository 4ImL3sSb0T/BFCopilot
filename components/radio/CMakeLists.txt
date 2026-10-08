idf_component_register(SRCS "bf_radio.c" INCLUDE_DIRS "include"
    REQUIRES protocol esp_wifi esp_event esp_netif nvs_flash esp_timer)
