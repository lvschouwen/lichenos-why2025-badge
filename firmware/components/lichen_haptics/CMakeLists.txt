# The vibration motor (#68): motor M1 behind Q4 on P4 GPIO8, PWM by LEDC (timer 3, channel 7). haptics_pattern.c is
# pure and host-tested (host_tests/haptics_pattern_test.c): pattern steps, strength duty, gating, rate limit.
# haptics.c is one esp_timer that runs while a buzz plays; lichen_os hooks haptics_notice into lichen_notify's
# feedback (#245: this L2 driver does not depend on notify); haptics_cmd.c is "fw vib".
# The app-facing header is lichen_api/lichen_haptics.h; lichen_os implements it over haptics.h (#221) and main
# requires this component.
idf_component_register(SRCS "haptics.c" "haptics_pattern.c" "haptics_cmd.c" "haptics_setting.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../shared/reg"
                       PRIV_REQUIRES fw_console esp_driver_gpio esp_driver_ledc esp_timer log)
