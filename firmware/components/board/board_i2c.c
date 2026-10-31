#include "board_i2c.h"

#include "esp_log.h"

static char const TAG[] = "board";

static i2c_master_bus_handle_t bus;

i2c_master_bus_handle_t board_i2c_bus(void) {
    if (bus) {
        return bus;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = BOARD_I2C_SDA,
        .scl_io_num                   = BOARD_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true, // as the keyboard driver had it
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c0 (sda %d, scl %d): %s", BOARD_I2C_SDA, BOARD_I2C_SCL, esp_err_to_name(err));
        bus = NULL;
    }
    return bus;
}
