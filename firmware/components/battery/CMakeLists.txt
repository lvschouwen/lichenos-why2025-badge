# Battery (#11). battery_model.c (voltage to percent, warn/off thresholds, MAX17048 register decode, ADC divider
# scaling) and battery_svc.c (what one poll means: present/absent/stale/lost, register plausibility, the actions) are
# pure, host-tested in host_tests/battery_model_test.c and battery_svc_test.c. max17048.c reads the gauge on the camera
# header's bus (board_i2c.h), battery_service.c is the task main starts, battery_cmd.c is "fw battery".
idf_component_register(SRCS "battery_model.c" "battery_svc.c" "max17048.c" "battery_service.c" "battery_cmd.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES board fw_console esp_timer freertos log)
