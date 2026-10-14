idf_component_register(
    SRCS "board/board.c" "board/board_status_led.c" "board/board_buzzer.c"
         "common/bf_crc.c" "common/bf_fault.c" "common/bf_runtime.c"
    INCLUDE_DIRS "board/include" "common/include"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_rmt esp_timer
)
