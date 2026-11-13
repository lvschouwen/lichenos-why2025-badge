# Protocol parsing, the inflate rules (ota_inflate.c, #298) and SHA-256 are pure, the same protocol tools/badge.sh speaks (host-tested in host_tests/ota_proto_test.c).
# ota_os.c (pure, ota_os_test.c) holds the settings and the Wi-Fi switch lichen_os binds (#245): no gate header here.
idf_component_register(SRCS "ota_server.c" "ota_path.c" "ota_valid.c" "ota_trial.c" "ota_proto.c" "ota_pieces.c" "ota_inflate.c" "ota_bmp.c" "ota_rle.c" "ota_autooff.c" "ota_os.c" "ota_hw.c" "sha256.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap app_update lichen_random esp_app_format esp_rom esp_system lwip fw_console c6link lichen_net esp_partition assets lichen_mdns storage)
