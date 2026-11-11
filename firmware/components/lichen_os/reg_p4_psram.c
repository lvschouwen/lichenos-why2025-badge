#include "psram_reg.h"
#include "psram_speed_logic.h"
#include "reg_p4.h"

// The P4's registry keys of domain 0x11: psram.* (#316, row file split #315). All read-only: what the MSPI controller
// runs now, read from its registers by psram_guard (psram_reg.c), never the software speed global or the chip's mode
// registers. `fw psram dl` stays the only mutator of the delayline. psram.src_clk, and so psram.clk's source term, is
// a boot snapshot of the PLL frequency: PSRAM init acquires the MPLL, so it cannot change later. Keep ESP calls out of
// this file (the getter is psram_guard's, from psram_reg.h, stubbed in host_tests/reg_stubs_psram.c) so the host test
// can link it.

// psram_clk_src_sel, the order of psram_ctrlr_ll_select_clk_source
static char const *const clk_src_names[] = {"xtal", "mpll", "spll", "cpll"};

// psram_fail_kind_t, in its order (#329)
static char const *const fail_names[] = {"none", "boot_check", "dma_gate", "crash", "restart", "aborted", "gave_up",
                                         "crash_armed", "against_rec"};
_Static_assert(sizeof(fail_names) / sizeof(fail_names[0]) == PSRAM_FAIL_KINDS, "psram.fail_why names out of step");

#define PSRAM_FAIL_RO(id_, type_, name_, max_)                                                                         \
    {.id = (id_), .type = (type_), .flags = REG_F_RO, .name = (name_), .max = (max_), .get_applied = psram_reg_fail_get}

#define PSRAM_RO(id_, type_, name_, unit_, max_)                                                                       \
    {.id = (id_), .type = (type_), .flags = REG_F_RO, .name = (name_), .unit = (unit_), .max = (max_),                \
     .get_applied = psram_reg_get}

static reg_entry_t const rows[] = {
    PSRAM_RO(REG_ID_PSRAM_CLK, REG_T_U32, "psram.clk", "Hz", 0),
    {.id = REG_ID_PSRAM_CLK_SRC, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "psram.clk_src", .min = 0, .max = 3,
     .enum_names = clk_src_names, .get_applied = psram_reg_get},
    PSRAM_RO(REG_ID_PSRAM_SRC_CLK, REG_T_U32, "psram.src_clk", "Hz", 0),
    PSRAM_RO(REG_ID_PSRAM_CORE_DIV, REG_T_U32, "psram.core_div", NULL, 0),
    PSRAM_RO(REG_ID_PSRAM_BUS_DIV, REG_T_U32, "psram.bus_div", NULL, 0),
    PSRAM_RO(REG_ID_PSRAM_WANT, REG_T_U32, "psram.want", "MHz", 0),
    PSRAM_RO(REG_ID_PSRAM_DL_IDX, REG_T_U8, "psram.dl_idx", NULL, 30),
    PSRAM_RO(REG_ID_PSRAM_DL_DATA, REG_T_U8, "psram.dl_data", NULL, 0),
    PSRAM_RO(REG_ID_PSRAM_DL_DQS, REG_T_U8, "psram.dl_dqs", NULL, 0),
    PSRAM_RO(REG_ID_PSRAM_PHASE, REG_T_U8, "psram.phase", NULL, 3),
    PSRAM_RO(REG_ID_PSRAM_BOOT_IDX, REG_T_U8, "psram.boot_idx", NULL, 30),
    {.id = REG_ID_PSRAM_FAIL_WHY, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "psram.fail_why", .min = 0,
     .max = PSRAM_FAIL_KINDS - 1, .enum_names = fail_names, .get_applied = psram_reg_fail_get},
    PSRAM_FAIL_RO(REG_ID_PSRAM_FAIL_RST, REG_T_STR, "psram.fail_rst", REG_STR_MAX),
    PSRAM_FAIL_RO(REG_ID_PSRAM_FAIL_AT120, REG_T_BOOL, "psram.fail_at120", 0),
    PSRAM_FAIL_RO(REG_ID_PSRAM_FAIL_FW, REG_T_STR, "psram.fail_fw", REG_STR_MAX),
    PSRAM_FAIL_RO(REG_ID_PSRAM_ABORTS, REG_T_U8, "psram.aborts", PSRAM_SPEED_MAX_ABORTS),
};

reg_p4_dom_t const reg_p4_psram = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
