idf_component_register(
    SRCS "bench_main.c" "hwtest/hwtest.c" "net/net.c"
    INCLUDE_DIRS "." "hwtest/include" "net/include"
    REQUIRES service driver bsp console esp_timer esp_system esp_wifi esp_netif
             esp_event nvs_flash lwip esp_driver_gpio
)
