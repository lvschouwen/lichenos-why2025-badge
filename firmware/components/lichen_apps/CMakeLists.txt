# Lichen's plain apps (#180): the apps that need nothing from the OS but lichen_api (see docs/app-api.md and the L6
# profile in tools/layers.conf). Clock (app_clock.c, #122), Timer (app_timer.c, #152; timer_service.c keeps the timers
# running, timer_calc.c is pure), Calculator (app_calc.c, #158; calc_eval.c and calc_units.c are pure), Weather
# (weather_app.c samples and logs, weather_screen.c draws, weather_series.c is pure, #155), Notepad (app_notes.c, #154:
# notes and to dos as text files on the SD card; notes_model.c rules and notes_store.c file work are pure), QR code
# (app_qr.c, #153), Keyboard test (app_keytest.c) and LoRa spectrum (app_lora_spectrum.c, #161; lora_spectrum.c is
# pure). lichen_apps.c registers them all (main calls lichen_apps_register()). The release notes app stays in the shell.
# Pure parts are host-tested (calc_eval_test, calc_units_test, timer_calc_test, weather_series_test, notes_model_test,
# notes_store_test, lora_spectrum_test).
idf_component_register(SRCS "lichen_apps.c" "app_clock.c" "app_timer.c" "timer_service.c" "timer_calc.c" "app_calc.c" "calc_eval.c" "calc_units.c"
                            "weather_app.c" "weather_screen.c" "weather_series.c"
                            "app_notes.c" "notes_model.c" "notes_store.c" "app_qr.c" "app_keytest.c"
                            "app_lora_spectrum.c" "lora_spectrum.c" "app_testcard.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_api lichen_clock lora fw_console freertos esp_timer heap)
