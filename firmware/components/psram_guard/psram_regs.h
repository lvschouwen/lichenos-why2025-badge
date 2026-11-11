#pragma once

#include <stdbool.h>
#include <stdint.h>

// The psram.* registry keys (#316) from one read of the MSPI controller's registers. Pure, host-tested in
// host_tests/psram_regs_test.c; psram_reg.c fills the snapshot (controller registers only, never the chip's mode
// registers, which read back garbage while PSRAM runs).
//
// The PSRAM bus clock is source / core divider / bus divider: MPLL 320 MHz / 1 / 4 = 80 MHz, 480 / 1 / 4 = 120 MHz
// with the patched IDF's speed switch (tools/idf-patches/0003, docs/psram-speed-switch.md).

#define PSRAM_CLK_SRCS 4 // psram_clk_src_sel: 0 xtal, 1 mpll, 2 spll, 3 cpll

typedef struct {
    uint32_t src_sel;      // HP_SYS_CLKRST peri_clk_ctrl00.reg_psram_clk_src_sel
    uint32_t src_hz;       // that source's frequency, 0 = not known (read before the boot snapshot)
    uint32_t core_div_num; // reg_psram_core_clk_div_num (divider - 1)
    uint32_t sram_clk;     // SPI_MEM_S_SRAM_CLK_REG as read
    uint8_t  dl_data;      // pin D's delayline
    uint8_t  dl_dqs;       // DQS0's 90 degree delayline
    uint8_t  phase;        // DQS0 phase id
    bool     tuned;        // the boot tuning's delayline choice was captured
    uint8_t  boot_idx;     // that choice
    uint32_t want_mhz;     // the speed switch's record (80 / 120)
} psram_regs_t;

// SPI_MEM_S_SRAM_CLK_REG: 1 when SCLK_EQU_SYSCLK (bit 31) is set, else SCLKCNT_N (bits 23:16) + 1
uint32_t psram_regs_bus_div(uint32_t sram_clk);
// src_hz / core divider / bus divider; 0 while the source is not known
uint32_t psram_regs_clk_hz(psram_regs_t const *r);
// The idx psram_tuning_delays maps to (data, dqs): only one of the two lines is delayed, 0..15 each. False for any
// other pair (both delayed, or past 15), which no idx makes
bool psram_regs_dl_idx(uint8_t data, uint8_t dqs, uint32_t *idx);
// The value of key id (REG_ID_PSRAM_*); false for another id or a value the registers cannot tell
bool psram_regs_value(psram_regs_t const *r, unsigned id, uint32_t *out);
