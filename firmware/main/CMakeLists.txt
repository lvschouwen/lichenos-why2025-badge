idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_app_format esp_partition heap nvs_flash storage fw_console c6link ota app_update lora board esp_timer mesh crash lichen)

# The FAT drive: C6 image and lora.toml, staged by tools/stage_storage.sh
fatfs_create_spiflash_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/../storage_staging FLASH_IN_PROJECT)
