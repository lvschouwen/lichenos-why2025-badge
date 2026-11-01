# Lichen, the badge's OS shell (#66): home, boot screen, setup, update, status bar, the system apps. Apps never require
# it; they PRIV_REQUIRE lichen_api (#84). Pure and host-tested: log_ring.c (log_ring_test.c; log_store.c keeps the log
# in PSRAM for GET /log, #38), nav.c (nav_test.c), home_layout.c (home_layout_test.c), shot_blend.c (shot_blend_test.c), time_fields.c (time_fields_test.c, #64),
# dialog_countdown.c (dialog_countdown_test.c, #14), ui_perf_stats.c (ui_perf_stats_test.c, #93), bench_calc.c (bench_calc_test.c, #96), sd_settings_model.c (sd_settings_model_test.c, #99),
# idle_policy.c (idle_policy_test.c, #43; screen_idle.c applies it), lora_activity.c (lora_activity_test.c, #29; the
# status bar's TX/RX chevrons), notify_view.c (notify_view_test.c, #62),
# sys_notice_rules.c (sys_notice_rules_test.c, #62; sys_notices.c raises what it decides),
# shortcut.c (shortcut_test.c, #79; shortcuts.c runs the shape-key shortcuts it decides).
# notify_ui.c and notify_centre.c are the notifications' toast, tile badges, cloud key, centre and settings page (#62).
# app_bench.c is the Bench app (#96), app_crashlog.c the Crash log app (#72, on lichen_crashlog.h), app_power.c the Power app (#65).
# shell.c, boot_screen.c, status_bar.c and logo.c are the LVGL screens; shell.c and app_settings.c implement lichen_ui.h.
idf_component_register(SRCS "log_store.c" "log_ring.c" "nav.c" "shell.c" "home.c" "home_layout.c"
                            "app_settings.c" "app_about.c" "app_keytest.c" "key_matrix.c" "hold_exit.c" "raw_keys.c" "app_icons.c" "wifi_screen.c" "setup_state.c" "setup_screens.c" "ui_keys.c" "ui_probe.c"
                            "boot_screen.c" "status_bar.c" "lora_activity.c" "link_state.c" "logo.c" "update_status.c" "update_mode.c" "update_screen.c" "shot_blend.c" "screenshot.c" "time_fields.c" "time_set_screen.c"
                            "dialog.c" "dialog_countdown.c" "ui_check.c" "ui_perf.c" "ui_perf_stats.c" "app_bench.c" "bench_calc.c" "app_crashlog.c" "app_power.c"
                            "sd_screen.c" "sd_settings_model.c"
                            "idle_policy.c" "screen_idle.c"
                            "notify_view.c" "notify_ui.c" "notify_centre.c" "sys_notices.c" "sys_notice_rules.c"
                            "shortcut.c" "shortcuts.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl lichen_api lichen_net
                       PRIV_REQUIRES lichen_boot lichen_clock freertos fw_console heap esp_hw_support esp_lvgl_port esp_app_format esp_timer crash board storage sdcard app_update esp_system psram_guard esp_mm lichen_led ota)
