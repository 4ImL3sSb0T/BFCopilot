if(BF_REMOTE)
    set(BF_MAIN_SOURCE "remote_main.c")
else()
    set(BF_MAIN_SOURCE "app_main.c")
endif()
idf_component_register(
    SRCS "${BF_MAIN_SOURCE}"
    INCLUDE_DIRS "."
    REQUIRES board common parameter servo sensors safety telemetry bf_console esp_timer esp_system flight radio protocol esp_driver_uart
)
