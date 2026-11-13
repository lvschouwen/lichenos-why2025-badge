# The microSD card at /sd (#60): sdcard.c is the ESP glue (LDO VO4, SDMMC slot 0, FAT mount, the batch mutex),
# sd_console.c is "fw sd". sd_policy.c and sd_bounce.c are pure and host-tested (host_tests/sd_policy_test.c, sd_bounce_test.c),
# as are sd_path_policy.c and sd_cat_fmt.c, what "fw sd cat" may read and how it prints it (sd_path_policy_test.c, #378):
# a debug command, so a release build (LICHEN_DEV_COMMANDS off) leaves them out.
set(srcs "sdcard.c" "sd_console.c" "sd_policy.c" "sd_bounce.c")
if(CONFIG_LICHEN_DEV_COMMANDS)
    list(APPEND srcs "sd_path_policy.c" "sd_cat_fmt.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board fatfs sdmmc esp_driver_sdmmc esp_driver_gpio heap freertos esp_timer fw_console vfs psram_guard)
