idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_app_format esp_partition heap nvs_flash storage fw_console c6link ota app_update lora board esp_timer mesh crash lichen_shell lichen_api lichen_boot lichen_net lichen_random assets psram_guard)

# The FAT drive: C6 image and lora.toml, staged by tools/stage_storage.sh
fatfs_create_spiflash_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/../storage_staging FLASH_IN_PROJECT)

# The assets partition (#22): firmware/assets/ packed by tools/pack_assets.py. Like the FAT image it is rebuilt on
# every build (CMake can't watch a directory's contents) and flashed with the rest; Wi-Fi: badge.sh ota-assets
partition_table_get_partition_info(assets_size "--partition-name assets" "size")
set(assets_image ${CMAKE_BINARY_DIR}/assets.bin)
idf_build_get_property(python PYTHON)
add_custom_target(assets_bin ALL
    COMMAND ${python} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/pack_assets.py ${CMAKE_CURRENT_SOURCE_DIR}/../assets
            ${assets_image} --max-size ${assets_size})
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${assets_image})
esp_partition_register_target(assets "${assets_image}" DEPENDS assets_bin FLASH_IN_PROJECT)
