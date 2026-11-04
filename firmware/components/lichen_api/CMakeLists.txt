# Lichen's app API (#84): what an app may use. Apps PRIV_REQUIRE lichen_api and never lichen_shell. lichen_ui.h's
# navigation calls are implemented by lichen_shell and resolve at link time (main requires both). Pure and host-tested:
# list_filter.c (list_filter_test.c), scroll_anchor.c (scroll_anchor_test.c), tz_table.c (tz_table_test.c).
# app_ns.c (app_ns_test.c). lichen_settings.c is an app's own NVS namespace (T5); settings_store is Lichen's (badge_fw,
# with tz_table for its default zone) and stays here because lichen_boot needs it; apps use lichen_settings.h.
# lichen_fs.c: an app's directory on the storage drive (T6) and on the SD card (#60); fs_path.c builds both paths
# (pure, fs_path_test.c). lichen_crashlog.c is the Crash log app's view of crash.c's log (#72); crashlog_fmt.c is its
# text (pure, crashlog_fmt_test.c). lichen_badge.c: the tile badges home draws (#102), over badge_table.c (pure,
# badge_table_test.c). lichen_ui.c: lichen_ui_call (lv_async_call under the LVGL lock, from any task) and lichen_ui_with_group
# (#84 T8a), so apps need no esp_lvgl_port. qr_screen.c is the QR code screen (#153; LVGL's QR widget, needs CONFIG_LV_USE_QRCODE), qr_payload.c its pure text (qr_payload_test.c). REQUIRES lichen_notify publicly: notifications (lichen_notify.h, #62) are part of the app API.
# lichen_task.c: lichen_task_start, tasks booked per app (#181, over task_book.c, pure, task_book_test.c); lichen_mem.c:
# lichen_alloc with the per-app internal quota (#181, over mem_book.c, pure, mem_book_test.c).
idf_component_register(SRCS "lichen_app.c" "menu.c" "menu_filter.c" "list_filter.c" "scroll_anchor.c" "fonts.c" "text_entry.c"
                            "settings_store.c" "tz_table.c" "app_ns.c" "lichen_settings.c" "lichen_fs.c" "fs_path.c"
                            "lichen_crashlog.c" "crashlog_fmt.c" "legacy_blob.c" "lichen_legacy.c" "settings_backup.c" "settings_backup_io.c" "lichen_badge.c" "badge_table.c" "radio_state.c" "lichen_radio.c"
                            "lichen_ui.c" "ui_text.c" "qr_payload.c" "qr_screen.c"
                            "lichen_task.c" "task_book.c" "lichen_mem.c" "mem_book.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl lichen_notify
                       PRIV_REQUIRES freertos esp_hw_support assets fw_console nvs_flash heap storage sdcard crash esp_lvgl_port esp_app_format)
