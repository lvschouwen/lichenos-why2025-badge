# Lichen's app API (#84), the gate (L4, #216/#220): the headers apps may use, the pure logic behind them and the gate's
# own LVGL widgets. No hardware dependencies: the implementations that need NVS, the storage drive, the SD card, the
# crash log, the LVGL port lock or the board live in lichen_os (L3) and resolve at link time (main requires both).
# lichen_ui.h's navigation and lichen_keys.h are the shell's work: lichen_ui_ops.c forwards them to the table the shell
# sets at boot (pure, ui_ops_test.c, #221), so the gate links to nothing above it. Apps PRIV_REQUIRE lichen_api, never lichen_os or
# lichen_shell. REQUIRES lichen_notify publicly: notifications (lichen_notify.h, #62) are part of the app API.
# Pure and host-tested: list_filter.c, scroll_anchor.c, tz_table.c, app_ns.c, fs_path.c (an app's directory paths),
# crashlog_fmt.c (the Crash log app's text), badge_table.c (the tile badges home draws, #102), legacy_blob.c,
# settings_backup.c, qr_payload.c (#153), task_book.c and mem_book.c (#181), tile_order.c (#189), hold_exit.c (#180),
# ui_text.c, menu_filter.c, os_name.c (the OS name and version line, #236); each has its <name>_test.c in host_tests/.
# Widgets: lichen_app.c (the app registry), menu.c (the menu widget: it uses the shell's screen calls, so it stays a
# gate widget rather than a lichen_os service, #220), text_entry.c, qr_screen.c (LVGL's QR widget, needs
# CONFIG_LV_USE_QRCODE) and lichen_ui.c's plain LVGL helpers (heading, labels, icon box, page hint).
# fonts.c holds the font slots lichen_font() reads; lichen_os loads the asset fonts into them (font_slots.h).
idf_component_register(SRCS "lichen_app.c" "fonts.c" "menu.c" "menu_filter.c" "list_filter.c" "scroll_anchor.c"
                            "text_entry.c"
                            "tz_table.c" "app_ns.c" "fs_path.c" "crashlog_fmt.c" "legacy_blob.c" "settings_backup.c"
                            "badge_table.c" "lichen_ui.c" "ui_text.c" "qr_payload.c" "qr_screen.c" "tile_order.c"
                            "task_book.c" "mem_book.c" "hold_exit.c" "os_name.c" "lichen_ui_ops.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl lichen_notify
                       PRIV_REQUIRES freertos fw_console)
