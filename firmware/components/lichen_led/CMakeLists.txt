# The status LED (#74): the blue LED D1 on the M.2 card (P4 GPIO51). led_pattern.c is pure and host-tested
# (host_tests/led_pattern_test.c): inputs (setting, do-not-disturb, unread, update, console override) -> pattern -> level.
# led.c drives the pin from one esp_timer (no task); led_cmd.c is "fw led". Do-not-disturb and unread are pushed in by
# lichen_notify through hooks lichen_os binds (#245), so this driver (L2) does not depend on notify (L3).
idf_component_register(SRCS "led.c" "led_pattern.c" "led_cmd.c" "led_setting.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../shared/reg"
                       PRIV_REQUIRES fw_console esp_driver_gpio soc esp_timer log)
