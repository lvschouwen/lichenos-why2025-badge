// Host test: the psram.* registry keys from the MSPI register snapshot (#316): clock dividers, the delayline idx the
// live delays make (the inverse of psram_tuning_delays), and which keys answer failed
#include "psram_regs.h"
#include "psram_tuning.h"
#include "reg_ids.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// SPI_MEM_S_SRAM_CLK_REG as psram_ctrlr_ll_set_bus_clock writes it for divider div
static uint32_t sram_clk_for(uint32_t div) {
    if (div == 1) {
        return 1u << 31;
    }
    return ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1);
}

static void test_clock(void) {
    CHECK(psram_regs_bus_div(sram_clk_for(1)) == 1);
    CHECK(psram_regs_bus_div(sram_clk_for(4)) == 4);
    CHECK(psram_regs_bus_div(sram_clk_for(2)) == 2);
    CHECK(psram_regs_bus_div(0) == 1); // N = 0 without the bit: still one, never a zero divider
    // The two speeds of the switch (docs/psram-speed-switch.md): MPLL 320 or 480 / core 1 / bus 4
    psram_regs_t r = {.src_sel = 1, .src_hz = 320000000u, .core_div_num = 0, .sram_clk = sram_clk_for(4)};
    CHECK(psram_regs_clk_hz(&r) == 80000000u);
    r.src_hz = 480000000u;
    CHECK(psram_regs_clk_hz(&r) == 120000000u);
    r.core_div_num = 1; // a core divider of 2 halves it
    CHECK(psram_regs_clk_hz(&r) == 60000000u);
    r.src_hz = 0; // before the boot snapshot
    CHECK(psram_regs_clk_hz(&r) == 0);
}

static void test_dl_idx(void) {
    for (uint8_t idx = 0; idx < PSRAM_DELAYLINES; idx++) {
        uint8_t  data, dqs;
        uint32_t back = 99;
        psram_tuning_delays(idx, &data, &dqs);
        CHECK(psram_regs_dl_idx(data, dqs, &back) && back == idx);
    }
    uint32_t idx = 99;
    CHECK(!psram_regs_dl_idx(2, 3, &idx) && idx == 99); // both lines delayed: no idx makes that
    CHECK(!psram_regs_dl_idx(16, 0, &idx));
    CHECK(!psram_regs_dl_idx(0, 16, &idx));
}

static void test_values(void) {
    psram_regs_t r = {.src_sel = 1, .src_hz = 320000000u, .core_div_num = 0, .sram_clk = sram_clk_for(4),
                      .dl_data = 2, .dl_dqs = 0, .phase = 2, .tuned = true, .boot_idx = 17, .want_mhz = 120};
    uint32_t v = 0;
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_CLK, &v) && v == 80000000u);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_CLK_SRC, &v) && v == 1);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_SRC_CLK, &v) && v == 320000000u);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_CORE_DIV, &v) && v == 1);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_BUS_DIV, &v) && v == 4);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_WANT, &v) && v == 120); // asked 120, runs 80: the two keys differ
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_DL_IDX, &v) && v == 17);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_DL_DATA, &v) && v == 2);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_DL_DQS, &v) && v == 0);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_PHASE, &v) && v == 2);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_BOOT_IDX, &v) && v == 17);
    // `fw psram dl 15 yes`: the live idx follows, the boot choice stays
    r.dl_data = 0;
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_DL_IDX, &v) && v == 15);
    CHECK(psram_regs_value(&r, REG_ID_PSRAM_BOOT_IDX, &v) && v == 17);
    // What the registers cannot tell answers failed, never a made-up value
    r.dl_data = 3, r.dl_dqs = 1;
    CHECK(!psram_regs_value(&r, REG_ID_PSRAM_DL_IDX, &v));
    r.tuned = false;
    CHECK(!psram_regs_value(&r, REG_ID_PSRAM_BOOT_IDX, &v));
    r.src_hz = 0;
    CHECK(!psram_regs_value(&r, REG_ID_PSRAM_CLK, &v) && !psram_regs_value(&r, REG_ID_PSRAM_SRC_CLK, &v));
    r.want_mhz = 0;
    CHECK(!psram_regs_value(&r, REG_ID_PSRAM_WANT, &v));
    CHECK(!psram_regs_value(&r, REG_ID_BL_IDLE, &v)); // not a psram key
}

int main(void) {
    test_clock();
    test_dl_idx();
    test_values();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_regs_test: ok\n");
    return 0;
}
