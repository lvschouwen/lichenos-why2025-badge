#include "board_i2c.h"

#include "esp_log.h"
#include "fw_console.h"

#include <string.h>

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

// Probes every 7-bit address a device may use (0x08-0x77); the keyboard chip (0x34) is the known one.
static void cmd_i2c(char const *args) {
    if (strcmp(args, "scan") != 0) {
        console_reply("i2c scan");
        return;
    }
    i2c_master_bus_handle_t b = board_i2c_bus();
    if (!b) {
        console_reply("i2c: no bus");
        return;
    }
    char line[96];
    int  n = 0, len = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(b, addr, 20) != ESP_OK) {
            continue;
        }
        n++;
        if (len < (int)sizeof line - 6) {
            len += snprintf(line + len, sizeof line - (size_t)len, " 0x%02x", addr);
        }
    }
    console_reply("i2c: %d device(s)%s", n, n ? line : "");
}

void board_i2c_register_console(void) {
    console_register("i2c", "i2c scan (addresses that answer on the badge bus)", cmd_i2c);
}
