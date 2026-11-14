# Protocol parsing, the inflate rules (ota_inflate.c, #298) and SHA-256 are pure, the same protocol tools/badge.sh speaks (host-tested in host_tests/ota_proto_test.c).
# ota_os.c (pure, ota_os_test.c) holds the settings and the Wi-Fi switch lichen_os binds (#245): no gate header here.
idf_component_register(SRCS "ota_server.c" "ota_http.c" "ota_upload.c" "ota_piece_http.c" "ota_extra.c" "ota_route.c" "ota_token.c" "ota_hold.c" "ota_path.c" "ota_valid.c" "ota_trial.c" "ota_proto.c" "ota_pieces.c" "ota_inflate.c" "ota_bmp.c" "ota_rle.c" "ota_autooff.c" "ota_os.c" "ota_hw.c" "sha256.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap app_update lichen_random esp_app_format esp_rom esp_system lwip fw_console c6link lichen_net esp_partition assets lichen_mdns storage)

# #403: stream_erase clamps an erase to the slot's end, which is only safe while the firmware size limit (FIRMWARE_MAX)
# is the slot size; ota_server.c asserts that against the size the partition table gives both app slots.
partition_table_get_partition_info(ota_slot0_size "--partition-name ota_0" "size")
partition_table_get_partition_info(ota_slot1_size "--partition-name ota_1" "size")
if(NOT ota_slot0_size OR NOT ota_slot0_size STREQUAL ota_slot1_size)
    message(FATAL_ERROR "ota: app slots ota_0 (${ota_slot0_size}) and ota_1 (${ota_slot1_size}) must have one size")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE OTA_SLOT_SIZE=${ota_slot0_size})
