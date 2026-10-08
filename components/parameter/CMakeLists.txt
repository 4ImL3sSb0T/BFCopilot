idf_component_register(
    SRCS "parameter.c"
    INCLUDE_DIRS "include"
    REQUIRES common nvs_flash esp_partition
)
