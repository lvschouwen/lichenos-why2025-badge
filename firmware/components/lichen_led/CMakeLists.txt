# The status LED (#74): the blue LED D1 on the M.2 card (P4 GPIO51). led_pattern.c is pure and host-tested
# (host_tests/led_pattern_test.c): inputs (setting, do-not-disturb, unread, update, console override) -> pattern -> level.
# led.c drives the pin from one esp_timer (no task); led_cmd.c is "fw led".
idf_component_register(SRCS "led.c" "led_pattern.c" "led_cmd.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_notify fw_console esp_driver_gpio esp_timer nvs_flash log)
