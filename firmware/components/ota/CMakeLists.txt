# Protocol parsing and SHA-256 are pure, the same protocol tools/badge.sh speaks (host-tested in host_tests/ota_proto_test.c)
idf_component_register(SRCS "ota_server.c" "ota_path.c" "ota_valid.c" "ota_proto.c" "ota_pieces.c" "ota_bmp.c" "sha256.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap app_update lichen_random esp_app_format esp_rom esp_system lwip fw_console c6link esp_partition assets)
