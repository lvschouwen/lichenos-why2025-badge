#pragma once

#include "reg.h"

// The psram.* registry keys (#316), the badge side of psram_regs.c. Only lichen_os's row file (reg_p4_psram.c) points
// at the getter; it declares it itself, so the host test links the rows against its stub.

// psram_boot_check, once: the clock sources' frequencies from their PLL registers (an analog-bus read, so not on every
// get). The PSRAM init holds the MPLL (esp_clk_tree_mpll_acquire), so it is not retuned afterwards.
void psram_reg_boot(void);

// Every psram.* key: the controller registers read now (memory-mapped reads, no PSRAM or analog-bus traffic, no lock),
// so any task may ask, the LVGL task included. Never writes a register.
reg_status_t psram_reg_get(reg_entry_t const *e, reg_value_t *out);

// psram.fail_* and psram.aborts (#329; aborts = the arm count since #353): the speed switch's record in RAM (a copy under its mutex), never NVS or a register
reg_status_t psram_reg_fail_get(reg_entry_t const *e, reg_value_t *out);
