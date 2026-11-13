#pragma once
// The MAX17048 fuel gauge on the camera header's bus (#11): register reads only, nothing written. The battery
// service's task is the only caller. Every value goes through battery_regs_check before anyone uses it.
#include "battery_svc.h"
#include "driver/i2c_master.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX17048_SCL_HZ     100000 // the chip does 400 kHz; 100 kHz leaves margin for flying wires to a breakout
#define MAX17048_TIMEOUT_MS 20     // per transfer: four 16-bit reads are about 2 ms at 100 kHz

esp_err_t max17048_attach(i2c_master_bus_handle_t bus);
// Removes the device from the bus (so the bus can be closed); safe when not attached
void      max17048_detach(void);
bool      max17048_read_version(uint16_t *version);
// VERSION, VCELL, SOC and CRATE in one go; r->ok false when any transfer failed
void      max17048_read(bat_regs_t *r);
