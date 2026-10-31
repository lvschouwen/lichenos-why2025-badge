# PSRAM watchdog (#40): canary blocks verified every ~100 ms. psram_pattern.c is pure (host_tests/psram_pattern_test.c).
idf_component_register(SRCS "psram_guard.c" "psram_pattern.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap esp_mm esp_timer freertos fw_console lvgl__lvgl)
