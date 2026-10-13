idf_component_register(
    SRCS "hwtest.c"
    INCLUDE_DIRS "include"
    REQUIRES net console board parameter sensors servo esp_driver_gpio
)
