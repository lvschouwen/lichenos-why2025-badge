# Lichen, the badge's OS shell (#66). Pure and host-tested: boot_steps.c (host_tests/boot_steps_test.c), log_clean.c
# (log_clean_test.c), nav.c (nav_test.c), home_layout.c (home_layout_test.c), tz_table.c (tz_table_test.c), scroll_anchor.c (scroll_anchor_test.c), shot_blend.c (shot_blend_test.c). boot_progress.c wraps boot_steps in a critical section for the badge;
# log_tap.c keeps the last console lines; shell.c, boot_screen.c, status_bar.c and logo.c are the LVGL screens.
idf_component_register(SRCS "boot_steps.c" "boot_text.c" "boot_progress.c" "log_tap.c" "log_clean.c" "nav.c" "shell.c" "home.c" "home_layout.c"
                            "settings_store.c" "tz_table.c" "menu.c" "app_settings.c" "app_about.c" "app_icons.c" "wifi_list.c" "wifi_join.c" "wifi_screen.c" "setup_state.c" "setup_screens.c" "text_entry.c" "ui_keys.c" "ui_probe.c"
                            "boot_screen.c" "status_bar.c" "link_state.c" "logo.c" "update_status.c" "update_mode.c" "update_screen.c" "scroll_anchor.c" "fonts.c" "shot_blend.c" "screenshot.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES assets freertos nvs_flash fw_console heap esp_hw_support esp_lvgl_port esp_app_format esp_timer crash board storage app_update esp_system)
