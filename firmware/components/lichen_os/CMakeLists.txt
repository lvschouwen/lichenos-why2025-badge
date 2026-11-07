# Lichen's OS services (L3, #216/#220): the implementations behind the gate's headers that need hardware or IDF
# services. They implement lichen_api's headers (hence the peer line in tools/layers.conf) and have none of their own.
# settings_store.c is Lichen's own NVS namespace (badge_fw, with tz_table for its default zone); lichen_settings.c an
# app's (T5); settings_backup_io.c reads and writes both for the backup file (#209). lichen_fs.c: an app's directory on
# the storage drive (T6) and on the SD card (#60). lichen_crashlog.c is the Crash log app's view of crash.c's log (#72);
# lichen_legacy.c hands the shell's old blobs to their app. lichen_badge.c: the tile badges home draws (#102).
# radio_state.c (pure, radio_state_test.c, #197) and lichen_radio.c: the radios' on/off state and LoRa borrowing.
# lichen_task.c / lichen_mem.c: tasks and internal memory booked per app (#181). fonts.c loads the fonts from the
# assets partition at boot into the gate's slots (#16, font_slots.h); lichen_ui_call.c hands work to the LVGL task
# under the port lock (#84 T8a, #221 owns it next) and is the UI service: the only owner of the LVGL lock and the default
# group (#225; ui_call_queue.c, ui_lock_owner.c and ui_lock_hits.c are its pure parts, ui_call_queue_test.c;
# ui_lock_check.c the debug lock-owner check and `fw ui lockcheck`). key_matrix.c (pure, key_matrix_test.c) names the keyboard's
# codes over board's keymap headers (#180).
# lichen_sensors.c and lichen_haptics.c: lichen_sensors.h and lichen_haptics.h over the sensors and haptics drivers,
# so apps and the shell never link to a driver (#221). settings_secret.c is the settings' secrets class (#235: the Wi-Fi
# passwords, never backed up or kept by a reset; settings_secret_keys.c, pure, names the class) and settings_store.c
# binds it into lichen_net, which may not open NVS itself.
# lichen_sys_display.c: lichen_sys_display.h and lichen_sys_power.h (#226) over board's backlight driver.
idf_component_register(SRCS "settings_store.c" "lichen_settings.c" "settings_backup_io.c" "lichen_fs.c"
                            "lichen_crashlog.c" "lichen_legacy.c" "lichen_badge.c" "radio_state.c" "lichen_radio.c"
                            "lichen_task.c" "lichen_mem.c" "fonts.c" "lichen_ui_call.c" "key_matrix.c"
                            "lichen_sensors.c" "lichen_haptics.c" "lichen_sys_display.c" "settings_secret.c" "settings_secret_keys.c"
                            "ui_call_queue.c" "ui_lock_owner.c" "ui_lock_hits.c" "ui_lock_check.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_api freertos esp_hw_support assets fw_console nvs_flash heap storage sdcard
                                     crash esp_lvgl_port esp_app_format board sensors lichen_notify lichen_led lichen_haptics lichen_net)

# The debug lock-owner check (#225): LVGL's allocator and lv_inv_area go through ui_lock_check.c
if(CONFIG_LICHEN_UI_LOCK_CHECK)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_malloc" "-Wl,--wrap=lv_malloc_zeroed"
                                                     "-Wl,--wrap=lv_calloc" "-Wl,--wrap=lv_zalloc"
                                                     "-Wl,--wrap=lv_realloc" "-Wl,--wrap=lv_reallocf"
                                                     "-Wl,--wrap=lv_free" "-Wl,--wrap=lv_inv_area")
endif()
