#include "max17048.h"

static i2c_master_dev_handle_t dev; // the battery task's only

esp_err_t max17048_attach(i2c_master_bus_handle_t bus) {
    if (dev) {
        return ESP_OK;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MAX17048_ADDR,
        .scl_speed_hz    = MAX17048_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        dev = NULL;
    }
    return err;
}

void max17048_detach(void) {
    if (dev) {
        (void)i2c_master_bus_rm_device(dev);
        dev = NULL;
    }
}

// A register is a 16-bit word, most significant byte first, read with a repeated start after the register address
static bool read16(uint8_t reg, uint16_t *out) {
    uint8_t b[2];
    if (!dev || i2c_master_transmit_receive(dev, &reg, 1, b, sizeof b, MAX17048_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *out = bat_be16(b[0], b[1]);
    return true;
}

bool max17048_read_version(uint16_t *version) {
    return read16(MAX17048_REG_VERSION, version);
}

void max17048_read(bat_regs_t *r) {
    *r    = (bat_regs_t){0};
    r->ok = read16(MAX17048_REG_VERSION, &r->version) && read16(MAX17048_REG_VCELL, &r->vcell) &&
            read16(MAX17048_REG_SOC, &r->soc) && read16(MAX17048_REG_CRATE, &r->crate);
}
