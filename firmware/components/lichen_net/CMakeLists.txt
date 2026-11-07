# Lichen's Wi-Fi data (#84): the scan list and the saved credentials (wifi_list.c, badge_fw wifi_ssid/wifi_pass through
# the settings service, which binds its store at boot: #235, passwords as the secrets class), the saved list
# (wifi_saved.c, #160, wifi_saved_test.c) and the join state (wifi_join.c). All pure and host-tested (wifi_list_test.c,
# wifi_cred_test.c, wifi_join_test.c). wifi_service.c is the Wi-Fi service on top of them (the c6wifi task, SNTP, `fw wifi`), moved out of
# c6link in #223: it rides on the C6 link (c6link, L2) and registers c6link's net hooks; the shell shows its state.
idf_component_register(SRCS "wifi_list.c" "wifi_saved.c" "wifi_analyser.c" "wifi_join.c" "wifi_service.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fw_console c6link esp_wifi esp_netif esp_event esp_timer lwip lichen_boot
                                     lichen_clock lichen_mdns)
