# display_rot.c and uart_lend.c are pure (host_tests/display_rot_test.c, uart_lend_test.c). lv_mem_psram.c is LVGL's heap (LV_STDLIB_CUSTOM). kbd_map.c and backlight_level.c are pure (host_tests/kbd_map_test.c, backlight_level_test.c); the TCA8418 table and US layout (tca8418_keymap.h, key_codes.h) come from BadgeVMS.
# board_sdmmc.c is the lock around the SDMMC controller the C6 link and the SD card share (#60).
idf_component_register(SRCS "display.c" "backlight.c" "backlight_level.c" "uart_lend.c" "r16_hold.c" "keyboard.c" "board_i2c.c" "board_sdmmc.c" "kbd_map.c" "lv_mem_psram.c" "display_rot.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl esp_lvgl_port fw_console
                       PRIV_REQUIRES esp_lcd esp_lcd_st7703 esp_driver_ppa esp_mm esp_tca8418 esp_hw_support esp_driver_uart
                                     esp_driver_gpio esp_driver_i2c psram_guard)
# nothing outside LVGL references the allocator, so pull it in before liblvgl is scanned
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
