# PSRAM watchdog (#40, #93): canary blocks verified every ~100 ms; psram_boot.c = tuning report + boot check. Pure and
# host-tested: psram_pattern.c, psram_tuning.c, psram_heap.c.
idf_component_register(SRCS "psram_guard.c" "psram_pattern.c" "psram_tuning.c" "psram_boot.c" "psram_heap.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap esp_mm esp_hw_support esp_timer freertos fw_console lvgl__lvgl nvs_flash soc hal esp_hal_pmu esp_hal_mspi esp_rom)
# The 120 MHz clock is a Lichen patch to the IDF (tools/idf-patches, #94): make sure it really took, an unknown choice
# would silently become the 200 MHz default that flips bits on this badge
if(CONFIG_SPIRAM_SPEED_120M AND NOT CONFIG_SPIRAM_SPEED EQUAL 120)
    message(FATAL_ERROR "CONFIG_SPIRAM_SPEED_120M is set but CONFIG_SPIRAM_SPEED is ${CONFIG_SPIRAM_SPEED}: the IDF patch is missing")
endif()
# Capture what the IDF's per-boot MSPI tuning chose (psram_boot.c); the select functions are called through a function pointer.
# Record where the IDF put the PSRAM heap (psram_guard.c, the free() guard of #89); the call passes straight through.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mspi_timing_psram_select_best_tuning_phase"
                                                 "-Wl,--wrap=mspi_timing_psram_select_best_tuning_delayline"
                                                 "-Wl,--wrap=heap_caps_add_region_with_caps")
