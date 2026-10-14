if(BF_REMOTE)
    set(app_sources "remote_main.c")
else()
    set(app_sources "app_main.c" "flight/flight.c" "bf_console/bf_console.c")
endif()
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "flight/include" "bf_console/include"
    REQUIRES service driver bsp console esp_timer esp_system esp_hw_support esp_driver_uart
)
