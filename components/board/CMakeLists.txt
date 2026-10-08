idf_component_register(
    SRCS "board.c" "board_status_led.c" "board_buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_rmt esp_timer
)
