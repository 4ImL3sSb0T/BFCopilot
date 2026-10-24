idf_component_register(
    SRCS "app_main.c" "net_test.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lib melody
)
