# Lichen's OS services (L3, #216/#220): the implementations behind the gate's headers that need hardware or IDF
# services. They implement lichen_api's headers (hence the peer line in tools/layers.conf) and have none of their own.
# settings_store.c is Lichen's own NVS namespace (badge_fw, with tz_table for its default zone); lichen_settings.c an
# app's (T5); settings_backup_io.c reads and writes both for the backup file (#209). lichen_fs.c: an app's directory on
# the storage drive (T6) and on the SD card (#60). lichen_crashlog.c is the Crash log app's view of crash.c's log (#72);
# lichen_legacy.c hands the shell's old blobs to their app. lichen_badge.c: the tile badges home draws (#102).
# radio_state.c (pure, radio_state_test.c, #197) and lichen_radio.c: the radios' on/off state and LoRa borrowing.
# lichen_task.c / lichen_mem.c: tasks and internal memory booked per app (#181). fonts.c loads the fonts from the
# assets partition at boot into the gate's slots (#16, font_slots.h); lichen_ui_call.c hands work to the LVGL task
# under the port lock (#84 T8a, #221 owns it next). key_matrix.c (pure, key_matrix_test.c) names the keyboard's
# codes over board's keymap headers (#180).
idf_component_register(SRCS "settings_store.c" "lichen_settings.c" "settings_backup_io.c" "lichen_fs.c"
                            "lichen_crashlog.c" "lichen_legacy.c" "lichen_badge.c" "radio_state.c" "lichen_radio.c"
                            "lichen_task.c" "lichen_mem.c" "fonts.c" "lichen_ui_call.c" "key_matrix.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_api freertos esp_hw_support assets fw_console nvs_flash heap storage sdcard
                                     crash esp_lvgl_port esp_app_format board sensors)
