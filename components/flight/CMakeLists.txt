idf_component_register(SRCS "flight.c" INCLUDE_DIRS "include"
    REQUIRES flapping protocol radio safety servo sensors parameter telemetry esp_timer esp_hw_support)
