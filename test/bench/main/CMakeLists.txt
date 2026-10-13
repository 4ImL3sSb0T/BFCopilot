idf_component_register(
    SRCS "bench_main.c"
    INCLUDE_DIRS "."
    REQUIRES net hwtest board parameter sensors servo esp_timer esp_system
)
