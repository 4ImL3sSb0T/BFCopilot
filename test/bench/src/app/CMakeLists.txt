idf_component_register(
    SRCS "bench_main.c" "hwtest/hwtest.c" "link/link.c" "flap/flap.c"
    INCLUDE_DIRS "." "hwtest/include" "link/include" "flap/include"
    REQUIRES service driver bsp console esp_timer esp_system esp_driver_gpio
)
