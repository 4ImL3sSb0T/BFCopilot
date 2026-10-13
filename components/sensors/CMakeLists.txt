idf_component_register(
    SRCS "sensors.c" "motion.c" "motion_math.c"
    INCLUDE_DIRS "include"
    REQUIRES common board parameter esp_adc esp_driver_i2c esp_driver_spi lsm6ds3tr_c
)
