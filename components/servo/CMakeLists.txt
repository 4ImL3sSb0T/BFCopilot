idf_component_register(
    SRCS "servo.c"
    INCLUDE_DIRS "include"
    REQUIRES common board parameter esp_driver_mcpwm
)
