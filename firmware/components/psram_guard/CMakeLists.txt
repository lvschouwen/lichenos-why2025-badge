# PSRAM watchdog (#40): canary blocks verified every ~100 ms; psram_boot.c = tuning report + boot check. psram_pattern.c and psram_tuning.c are pure (host_tests).
idf_component_register(SRCS "psram_guard.c" "psram_pattern.c" "psram_tuning.c" "psram_boot.c" "psram_scan.c" "psram_quar.c" "psram_learn.c" "psram_learned.c" "psram_sweep.c" "psram_ldo.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap esp_mm esp_timer freertos fw_console lvgl__lvgl nvs_flash soc hal esp_hal_pmu esp_hal_mspi esp_rom)
# Capture what the IDF's per-boot MSPI tuning chose (psram_boot.c); the select functions are called through a function pointer
# Hold back the IDF's PSRAM heap registration until the learned bad pages are known (psram_scan.c, #86)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mspi_timing_psram_select_best_tuning_phase"
                                                 "-Wl,--wrap=mspi_timing_psram_select_best_tuning_delayline"
                                                 "-Wl,--wrap=heap_caps_add_region_with_caps")
