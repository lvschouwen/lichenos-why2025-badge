# Lichen's OS services (L3, #216/#220): the implementations behind the gate's headers that need hardware or IDF
# services. They implement lichen_api's headers (hence the peer line in tools/layers.conf) and have none of their own.
# settings_store.c is Lichen's own NVS namespace (badge_fw, with tz_table for its default zone); lichen_settings.c an
# app's (T5); settings_backup_io.c reads and writes both for the backup file (#209). lichen_fs.c: an app's directory on
# the storage drive (T6) and on the SD card (#60); app_slot.c (pure, app_slot_test.c, #272) its table's slot lookup. lichen_crashlog.c is the Crash log app's view of crash.c's log (#72);
# lichen_badge.c: the tile badges home draws (#102).
# radio_state.c (pure, radio_state_test.c, #197) and lichen_radio.c: the radios' on/off state and LoRa borrowing.
# lichen_task.c / lichen_mem.c: tasks and internal memory booked per app (#181); dma_low.c (pure, dma_low_test.c) records
# when the DMA heap's low-water mark fell and who took the most then; dma_watch.c feeds it (heap alloc hook + esp_timer
# sample) for fw mem (#256). fonts.c loads the fonts from the
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
# lichen_sys_mem.c: the heap figures and the diagnostic walk for the shell and main (#228), and the C6 link's Wi-Fi
# frame pool figures for the stress run (#278, through c6link); lichen_task.c also gives it every task's stack.
# lichen_sys_storage.c / lichen_sys_update.c: lichen_sys_storage.h (SD card and storage drive), lichen_sys_update.h,
# lichen_sys_crash.h and lichen_sys_factory.h's request (#227); lichen_sys_pure.c their ESP-free parts (host-tested).
# lichen_sys_pure_view.c / lichen_sys_view.c (#244): the shell's clock, Wi-Fi, mDNS, boot progress, keyboard-hook and LED
# calls (lichen_sys_clock.h, _net.h, _boot.h, _keys.h, _led.h) forward to the services it may not include; the pure
# half (lichen_sys_pure_view_test.c) holds the type asserts.
# os_bind.c (ESP-free, os_bind_test.c, #245) wires the layers below the gate: settings for lichen_boot, lichen_clock and
# ota, notify's state and feedback for the LED and the motor.
# The watch services (#281, lichen_os.h lichen_watch_start; main starts them display or not): thermal.c is the thermal
# watchdog (#10; thermal_rules.c, pure, thermal_rules_test.c; the shell reads it through lichen_sys_thermal.h) and
# sys_notices.c raises Lichen's own notices (#62; sys_notice_rules.c, pure, sys_notice_rules_test.c). The load shed
# holds the radios through lichen_sys_radio.h (#282, never saved). lichen_sys_power.c is the power-off's last step.
# The settings registry service (#307/#310): lichen_reg.c over the pure reg_cmd.c (fw get/set rows and rules),
# reg_proxy.c (the C6 keys' cache, seq, caps, mismatch) and reg_p4_table.c (the P4 core table and the domain iteration; the P4-side domains 0x11-0x15 have a row file each, reg_p4_<domain>.c, #315), host-tested in
# reg_service_test.c; the core is shared/reg/reg.c (Apache-2.0, also in the C6 image). reg_wifi.c (pure,
# wifi_reg_test.c, #311) maps lichen_net's Wi-Fi read-back to the wifi.* keys.
idf_component_register(SRCS "settings_store.c" "lichen_settings.c" "settings_backup_io.c" "lichen_fs.c" "app_slot.c"
                            "lichen_crashlog.c" "lichen_badge.c" "radio_state.c" "lichen_radio.c"
                            "lichen_task.c" "lichen_mem.c" "fonts.c" "lichen_ui_call.c" "key_matrix.c"
                            "lichen_sensors.c" "lichen_haptics.c" "lichen_sys_display.c" "lichen_sys_mem.c" "lichen_sys_pure.c" "lichen_sys_storage.c" "lichen_sys_update.c" "settings_secret.c" "settings_secret_keys.c" "os_bind.c" "lichen_sys_pure_view.c" "lichen_sys_view.c"
                            "ui_call_queue.c" "ui_lock_owner.c" "ui_lock_hits.c" "ui_lock_check.c" "dma_low.c" "dma_watch.c"
                            "thermal.c" "thermal_rules.c" "sys_notices.c" "sys_notice_rules.c" "lichen_sys_power.c"
                            "lichen_reg.c" "reg_cmd.c" "reg_proxy.c" "reg_p4_table.c" "reg_p4_psram.c" "reg_p4_display.c" "reg_p4_sensors.c" "reg_p4_misc.c" "reg_p4_settings.c" "settings_fail.c" "settings_reg.c" "reg_wifi.c" "../../../shared/reg/reg.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../shared/reg"
                       PRIV_REQUIRES lichen_api freertos esp_hw_support esp_timer assets fw_console nvs_flash heap storage sdcard
                                     crash esp_lvgl_port esp_app_format board sensors lichen_notify lichen_led lichen_haptics lichen_net ota lichen_boot lichen_clock lichen_mdns c6link psram_guard lora)

# The debug lock-owner check (#225): LVGL's allocator and lv_inv_area go through ui_lock_check.c
if(CONFIG_LICHEN_UI_LOCK_CHECK)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_malloc" "-Wl,--wrap=lv_malloc_zeroed"
                                                     "-Wl,--wrap=lv_calloc" "-Wl,--wrap=lv_zalloc"
                                                     "-Wl,--wrap=lv_realloc" "-Wl,--wrap=lv_reallocf"
                                                     "-Wl,--wrap=lv_free" "-Wl,--wrap=lv_inv_area")
endif()
