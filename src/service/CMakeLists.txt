set(service_sources "parameter/parameter.c" "flapping/flapping.c")
set(service_includes "parameter/include" "flapping/include")
if(NOT BF_BENCH)
    list(APPEND service_sources "protocol/bf_protocol.c" "radio/bf_radio.c"
         "safety/safety.c" "telemetry/telemetry.c")
    list(APPEND service_includes "protocol/include" "radio/include"
         "safety/include" "telemetry/include")
endif()
idf_component_register(
    SRCS ${service_sources}
    INCLUDE_DIRS ${service_includes}
    REQUIRES bsp driver nvs_flash esp_partition esp_timer
)
