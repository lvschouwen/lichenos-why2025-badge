# Keeps every crash (#74): core dump on flash, a text log on /storage, "fw crash". crash_log.c is pure, host-tested in
# host_tests/crash_log_test.c
idf_component_register(SRCS "crash.c" "crash_log.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES espcoredump esp_partition app_update bootloader_support esp_system esp_rom nvs_flash
                                     storage fw_console)
