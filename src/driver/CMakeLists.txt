set(driver_sources
    "servo/servo.c" "sensors/sensors.c" "sensors/motion.c" "sensors/motion_math.c"
    "lsm6ds3tr_c/vendor/lsm6ds3tr-c_reg.c")
set(driver_requires bsp esp_driver_mcpwm esp_adc esp_driver_i2c esp_driver_spi)
if(NOT BF_BENCH)
    list(APPEND driver_sources "espnow/espnow.c" "wifi_link/wifi_link.c")
    list(APPEND driver_requires esp_wifi esp_event esp_netif nvs_flash esp_timer lwip)
endif()
idf_component_register(
    SRCS ${driver_sources}
    INCLUDE_DIRS "servo/include" "sensors/include" "lsm6ds3tr_c/vendor" "espnow/include"
                 "wifi_link/include"
    PRIV_INCLUDE_DIRS "sensors"
    REQUIRES ${driver_requires}
)
