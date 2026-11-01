# The microSD card at /sd (#60): sdcard.c is the ESP glue (LDO VO4, SDMMC slot 0, FAT mount, the batch mutex),
# sd_console.c is "fw sd". sd_policy.c is pure and host-tested (host_tests/sd_policy_test.c).
idf_component_register(SRCS "sdcard.c" "sd_console.c" "sd_policy.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio heap freertos esp_timer fw_console vfs)
