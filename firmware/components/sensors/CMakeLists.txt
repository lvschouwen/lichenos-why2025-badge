# The sensors service (#69): the BMI270 IMU and BME690 environment sensor on the shared badge I2C bus and the P4's
# temperature sensor, sampled by one task and exposed to apps through lichen_api/lichen_sensors.h (#84). The chips are
# driven by Bosch's SensorAPIs (git submodules under third_party/, BSD-3-Clause): only the BMI270 variant of the
# BMI2 family (bmi2.c, bmi270.c) and bme69x.c are compiled. sensors_format.c is pure and host-tested
# (host_tests/sensors_format_test.c).
set(bmi ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/BMI270_SensorAPI)
set(bme ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/BME69x_SensorAPI)

idf_component_register(SRCS "sensors.c" "sensors_bus.c" "sensors_imu.c" "sensors_env.c" "sensors_cmd.c"
                            "sensors_format.c" "${bmi}/bmi2.c" "${bmi}/bmi270.c" "${bme}/bme69x.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${bmi}" "${bme}"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES board lichen_api fw_console esp_driver_tsens esp_timer esp_rom freertos log)
