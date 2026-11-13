idf_component_register(SRCS "assets.c" "assets_fmt.c" "flash_chunk.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_partition)
