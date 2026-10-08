idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board common parameter servo sensors safety telemetry bf_console esp_timer esp_system
)
