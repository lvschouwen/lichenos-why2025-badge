#include "psram_regs.h"

#include "reg_ids.h"

#define SRAM_CLK_EQU_SYSCLK (1u << 31)
#define SRAM_CLKCNT_N_S     16
#define SRAM_CLKCNT_N_V     0xFFu
#define DL_IDX_ZERO         15u // idx without delay (psram_tuning.h PSRAM_IDX_ZERO)

uint32_t psram_regs_bus_div(uint32_t sram_clk) {
    if (sram_clk & SRAM_CLK_EQU_SYSCLK) {
        return 1;
    }
    return ((sram_clk >> SRAM_CLKCNT_N_S) & SRAM_CLKCNT_N_V) + 1;
}

uint32_t psram_regs_clk_hz(psram_regs_t const *r) {
    // Both dividers are at least 1 by construction (register field + 1)
    return r->src_hz / (r->core_div_num + 1) / psram_regs_bus_div(r->sram_clk);
}

bool psram_regs_dl_idx(uint8_t data, uint8_t dqs, uint32_t *idx) {
    if (data > DL_IDX_ZERO || dqs > DL_IDX_ZERO || (data && dqs)) {
        return false;
    }
    *idx = dqs ? DL_IDX_ZERO - dqs : DL_IDX_ZERO + data;
    return true;
}

bool psram_regs_value(psram_regs_t const *r, unsigned id, uint32_t *out) {
    switch (id) {
    case REG_ID_PSRAM_CLK:
        *out = psram_regs_clk_hz(r);
        return *out != 0;
    case REG_ID_PSRAM_CLK_SRC:
        *out = r->src_sel;
        return r->src_sel < PSRAM_CLK_SRCS;
    case REG_ID_PSRAM_SRC_CLK:
        *out = r->src_hz;
        return r->src_hz != 0;
    case REG_ID_PSRAM_CORE_DIV:
        *out = r->core_div_num + 1;
        return true;
    case REG_ID_PSRAM_BUS_DIV:
        *out = psram_regs_bus_div(r->sram_clk);
        return true;
    case REG_ID_PSRAM_WANT:
        *out = r->want_mhz;
        return r->want_mhz != 0;
    case REG_ID_PSRAM_DL_IDX:
        return psram_regs_dl_idx(r->dl_data, r->dl_dqs, out);
    case REG_ID_PSRAM_DL_DATA:
        *out = r->dl_data;
        return true;
    case REG_ID_PSRAM_DL_DQS:
        *out = r->dl_dqs;
        return true;
    case REG_ID_PSRAM_PHASE:
        *out = r->phase;
        return true;
    case REG_ID_PSRAM_BOOT_IDX:
        *out = r->boot_idx;
        return r->tuned;
    default:
        return false;
    }
}
