# The C6 flasher came from BadgeVMS, its file access moved to stdio on /storage;
# c6_image.c, c6_boot_policy.c and c6_watch.c are pure (host-tested in host_tests/)
idf_component_register(SRCS "c6link.c" "c6_paths.c" "c6_flasher.c" "c6_firmware.c" "c6_image.c" "c6_boot_policy.c" "c6_watch.c" "c6_sup.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hosted esp_wifi esp_netif esp_event lichen_net
                       PRIV_REQUIRES board lichen_boot lichen_clock fw_console nvs_flash app_update esp_driver_uart esp_driver_gpio esp_timer
                                     espressif__esp-serial-flasher)
