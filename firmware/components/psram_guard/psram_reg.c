#include "psram_reg.h"

#include "psram_boot.h"
#include "psram_regs.h"
#include "psram_speed.h"
#include "reg_ids.h"

#include "esp_clk_tree.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/soc.h"
#include "soc/spi_mem_s_reg.h"

#include <stdio.h>

// Indexed by psram_clk_src_sel, the order of psram_ctrlr_ll_select_clk_source
static soc_module_clk_t const SRC_CLK[PSRAM_CLK_SRCS] = {SOC_MOD_CLK_XTAL, SOC_MOD_CLK_MPLL, SOC_MOD_CLK_SPLL,
                                                         SOC_MOD_CLK_CPLL};
static uint32_t s_src_hz[PSRAM_CLK_SRCS]; // written once before the shell starts, read-only afterwards

void psram_reg_boot(void) {
    for (int i = 0; i < PSRAM_CLK_SRCS; i++) {
        uint32_t hz = 0;
        if (esp_clk_tree_src_get_freq_hz(SRC_CLK[i], ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &hz) != ESP_OK) {
            hz = 0; // the key then answers failed rather than a made-up clock
        }
        s_src_hz[i] = hz;
    }
}

static void regs_read(psram_regs_t *r) {
    r->src_sel      = HP_SYS_CLKRST.peri_clk_ctrl00.reg_psram_clk_src_sel;
    r->src_hz       = s_src_hz[r->src_sel % PSRAM_CLK_SRCS];
    r->core_div_num = HP_SYS_CLKRST.peri_clk_ctrl00.reg_psram_core_clk_div_num;
    r->sram_clk     = REG_READ(SPI_MEM_S_SRAM_CLK_REG); // MSPI2, the PSRAM's SPI0; MSPI3 is set to the same divider
    psram_boot_tuning_regs(&r->phase, &r->dl_dqs, &r->dl_data);
    r->tuned    = psram_boot_dl_choice(&r->boot_idx);
    r->want_mhz = psram_speed_want();
}

reg_status_t psram_reg_get(reg_entry_t const *e, reg_value_t *out) {
    psram_regs_t r;
    regs_read(&r);
    uint32_t v;
    if (!psram_regs_value(&r, e->id, &v)) {
        return REG_FAILED;
    }
    out->u = v;
    return REG_OK;
}

reg_status_t psram_reg_fail_get(reg_entry_t const *e, reg_value_t *out) {
    psram_speed_rec_t r;
    psram_speed_record(&r);
    bool none = r.last.kind == PSRAM_FAIL_NONE;
    switch (e->id) {
    case REG_ID_PSRAM_FAIL_WHY: out->u = r.last.kind; return REG_OK;
    case REG_ID_PSRAM_FAIL_RST:
        snprintf(out->s, sizeof(out->s), "%s", none ? "none" : psram_speed_reset_name(r.last.reset));
        return REG_OK;
    case REG_ID_PSRAM_FAIL_AT120: out->u = r.last.at120; return REG_OK;
    case REG_ID_PSRAM_FAIL_FW: snprintf(out->s, sizeof(out->s), "%s", none ? "none" : r.last.fw); return REG_OK;
    case REG_ID_PSRAM_ABORTS: out->u = r.arms; return REG_OK;
    default: return REG_FAILED;
    }
}
