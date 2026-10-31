# Lichen's Wi-Fi data (#84): the scan list and the saved credentials (wifi_list.c, NVS badge_fw wifi_ssid/wifi_pass)
# and the join state (wifi_join.c). Both pure apart from NVS and host-tested (wifi_list_test.c, wifi_cred_test.c,
# wifi_join_test.c); c6link fills them, the shell shows them.
idf_component_register(SRCS "wifi_list.c" "wifi_join.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fw_console nvs_flash)
