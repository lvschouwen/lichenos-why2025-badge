# The C6 flasher came from BadgeVMS, its file access moved to stdio on /storage;
# c6_image.c, c6_boot_policy.c, c6_watch.c and c6_hooks.c are pure (host-tested in host_tests/).
# An L2 driver (#223): nothing above board and fw_console. The boot step and the Wi-Fi service (lichen_net) reach it
# through the hooks of c6_hooks.h, which they register at boot.
idf_component_register(SRCS "c6link.c" "c6_paths.c" "c6_flasher.c" "c6_firmware.c" "c6_image.c" "c6_boot_policy.c" "c6_watch.c" "c6_sup.c" "c6_hooks.c" "c6_log_arg.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../shared/reg"
                       REQUIRES esp_hosted esp_event
                       PRIV_REQUIRES board fw_console nvs_flash app_update esp_driver_uart esp_driver_gpio esp_timer
                                     espressif__esp-serial-flasher)
