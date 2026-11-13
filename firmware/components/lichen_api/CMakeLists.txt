# Lichen's app API (#84), the gate (L4, #216/#220): the headers apps may use, the pure logic behind them and the gate's
# own LVGL widgets. No hardware dependencies: the implementations that need NVS, the storage drive, the SD card, the
# crash log, the LVGL port lock or the board live in lichen_os (L3) and resolve at link time (main requires both).
# lichen_ui.h's navigation and lichen_keys.h are the shell's work: lichen_ui_ops.c forwards them to the table the shell
# sets at boot (pure, ui_ops_test.c, #221), so the gate links to nothing above it. Apps PRIV_REQUIRE lichen_api, never lichen_os or
# lichen_shell. Notifications are gate headers too (#245): lichen_notify.h (apps), notify_os.h (the shell) and
# notify_entry.h (an entry's type), implemented by the lichen_notify service (L3); the gate depends on no service.
# Pure and host-tested: list_filter.c, scroll_anchor.c, tz_table.c, app_ns.c, app_secret.c (#288), fs_path.c (an app's directory paths),
# crashlog_fmt.c (the Crash log app's text), badge_table.c (the tile badges home draws, #102),
# settings_backup.c, qr_payload.c (#153), task_book.c and mem_book.c (#181), tile_order.c (#189), hold_exit.c (#180),
# ui_text.c, menu_filter.c, os_name.c (the OS name and version line, #236), app_ctx.c (the app contexts, #230), gate_app.c (what a ctx stands for in the settings, file, badge and notice calls, #230 E1c); each has its <name>_test.c in host_tests/.
# lichen_ui_ops.c (the forwarding to the shell's table, #221) is pure too, tested by host_tests/ui_ops_test.c.
# gate_mem.c (the gate's widgets allocate through the lichen_alloc table lichen_os hands over at boot, #296) is pure,
# tested by host_tests/gate_mem_test.c.
# Widgets: lichen_app.c (the app registry, its contexts and the current app in thread-local slot 1, #230), menu.c (the menu widget: it uses the shell's screen calls, so it stays a
# gate widget rather than a lichen_os service, #220), text_entry.c, qr_screen.c (LVGL's QR widget, needs
# CONFIG_LV_USE_QRCODE) and lichen_ui.c's plain LVGL helpers (heading, labels, icon box, page hint).
# test_card.c (the bench test card of #352, drawn from test_card_layout.c: pure, test_card_layout_test.c) is shared by the
# shell's `fw ui card` overlay and the Test card app.
# bench_scope.c (the bench console scope MeshCore binds for the shell's remote answers, #377) is pure, tested by
# host_tests/bench_scope_test.c. key_cause.c (what set off the UI work running now, for hand-only actions, #400) is pure,
# tested by host_tests/key_cause_test.c.
# fonts.c holds the font slots lichen_font() reads; lichen_os loads the asset fonts into them (font_slots.h).
idf_component_register(SRCS "lichen_app.c" "fonts.c" "menu.c" "menu_filter.c" "list_filter.c" "scroll_anchor.c"
                            "text_entry.c"
                            "tz_table.c" "app_ns.c" "app_secret.c" "fs_path.c" "crashlog_fmt.c" "settings_backup.c"
                            "badge_table.c" "lichen_ui.c" "ui_text.c" "qr_payload.c" "qr_screen.c" "tile_order.c"
                            "task_book.c" "mem_book.c" "hold_exit.c" "os_name.c" "lichen_ui_ops.c" "gate_mem.c" "app_ctx.c" "gate_app.c" "test_card_layout.c" "test_card.c"
                            "bench_scope.c" "key_cause.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES freertos fw_console lichen_random)
