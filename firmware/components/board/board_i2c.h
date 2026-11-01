#pragma once
// The badge's one I2C bus (i2c0: the TCA8418 keyboard, and the BMI270 and BME690 of #69). The OS creates it once and
// drivers adopt the handle (#84, #87): a driver that creates its own bus made the IDF log an error on every boot
// when i2c_bus checked for an existing one.

#include "driver/i2c_master.h"

#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 20

// Created on first use, NULL when the driver refused it (logged)
i2c_master_bus_handle_t board_i2c_bus(void);

// `fw i2c scan`: which addresses answer on the bus (#11: is the IP5306 the I2C variant at 0x75?)
void board_i2c_register_console(void);
