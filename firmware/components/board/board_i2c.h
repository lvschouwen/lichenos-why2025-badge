#pragma once
// The badge's main I2C bus (i2c0: the TCA8418 keyboard, and the BMI270 and BME690 of #69). The OS creates it once and
// drivers adopt the handle (#84, #87): a driver that creates its own bus made the IDF log an error on every boot
// when i2c_bus checked for an existing one.

#include "driver/i2c_master.h"

#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 20

// The camera header's bus (#11): J7 pin 7 = SDA (P4 GPIO22), pin 8 = SCL (P4 GPIO9), pin 1 = +3V3, pin 10 = GND,
// 4.7 kOhm pull-ups R7/R8 on the carrier ("I2C2" in the schematic; the P4's second HP controller, i2c1). Nothing is
// on it on a stock badge: it carries the optional MAX17048 fuel gauge (0x36) and TMP102 (0x48) bodges of #11. It is
// opened only to look for them and closed again when nothing answers, so the pins go back to plain inputs.
#define BOARD_I2C2_PORT I2C_NUM_1
#define BOARD_I2C2_SDA  22
#define BOARD_I2C2_SCL  9

// Created on first use, NULL when the driver refused it (logged)
i2c_master_bus_handle_t board_i2c_bus(void);

// `fw i2c scan`: which addresses answer on the bus (#11: is the IP5306 the I2C variant at 0x75?)
void board_i2c_register_console(void);

// Opens the camera header's bus (100 kHz devices; no internal pull-ups, R7/R8 do that) or returns the open one.
// ESP_ERR_INVALID_STATE without touching the controller when SDA or SCL reads low as a plain input: no pull-up means
// nothing is wired (or the breakout is unpowered), and an I2C transfer on such a line only runs into timeouts. Any task;
// the caller owns the bus (one user today, the battery service). Logs nothing: the caller logs once.
esp_err_t board_i2c2_open(i2c_master_bus_handle_t *out);
// Deletes the bus when it is open (every device must be removed first) and leaves SDA/SCL as inputs with the output
// drivers off. Safe to call when it is not open.
void board_i2c2_close(void);
