# Lichen's app API (#84): what an app may use. Apps PRIV_REQUIRE lichen_api and never lichen_shell. lichen_ui.h's
# navigation calls are implemented by lichen_shell and resolve at link time (main requires both). Pure and host-tested:
# list_filter.c (list_filter_test.c), scroll_anchor.c (scroll_anchor_test.c), tz_table.c (tz_table_test.c).
# settings_store (badge_fw NVS, with tz_table for its default zone) is exported until T5 (#84).
idf_component_register(SRCS "lichen_app.c" "menu.c" "menu_filter.c" "list_filter.c" "scroll_anchor.c" "fonts.c" "text_entry.c"
                            "settings_store.c" "tz_table.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES assets fw_console nvs_flash heap)
