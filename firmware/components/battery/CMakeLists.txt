# Battery model (#11): pure logic only for now (voltage to percent, warn/off thresholds, MAX17048 register decode,
# ADC divider scaling), host-tested in host_tests/battery_model_test.c. No component requires it yet, so the image
# does not contain it; the drivers (task 2D) add the I2C/ADC glue and the first REQUIRES.
idf_component_register(SRCS "battery_model.c"
                       INCLUDE_DIRS ".")
