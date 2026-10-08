idf_component_register(
    SRCS "bf_console.c"
    INCLUDE_DIRS "include"
    REQUIRES common board parameter servo safety sensors telemetry console
)
