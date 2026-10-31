# lv_mem_psram.c is LVGL's heap (LV_STDLIB_CUSTOM). kbd_map.c and backlight_level.c are pure (host_tests/kbd_map_test.c, backlight_level_test.c); the TCA8418 table and US layout (tca8418_keymap.h, key_codes.h) come from BadgeVMS
idf_component_register(SRCS "display.c" "backlight.c" "backlight_level.c" "keyboard.c" "kbd_map.c" "lv_mem_psram.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl esp_lvgl_port
                       PRIV_REQUIRES esp_lcd esp_lcd_st7703 esp_tca8418 esp_hw_support esp_driver_uart
                                     esp_driver_gpio nvs_flash fw_console)
# nothing outside LVGL references the allocator, so pull it in before liblvgl is scanned
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
