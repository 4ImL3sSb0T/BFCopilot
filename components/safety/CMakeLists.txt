idf_component_register(
    SRCS "safety.c"
    INCLUDE_DIRS "include"
    REQUIRES common board servo telemetry
)
