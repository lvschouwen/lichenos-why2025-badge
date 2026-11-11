#include "reg_p4.h"

#include "display_info.h"

// The P4's registry keys of domain 0x12: disp.* (#317), all read-only. The getters live in board/display.c (clock
// registers, no bus traffic), declared in board/display_info.h and stubbed in host_tests/reg_stubs_display.c so the host test can
// link this file. Keep ESP calls out of it.
//
// What each key reads: pclk and refresh come from the DPI clock source select and divider the driver programmed, so
// they show what the clock tree gave the panel, not the 36 MHz asked for. cfg_lanes, cfg_lane_rate and cfg_phy_mv are the values
// display_start() configured: the IDF has no read-back for them.

static reg_status_t disp_get(reg_entry_t const *e, reg_value_t *out) {
    bool (*const fn)(uint32_t *) = e->id == REG_ID_DISP_PCLK      ? display_dpi_hz
                                  : e->id == REG_ID_DISP_REFRESH   ? display_refresh_dhz
                                  : e->id == REG_ID_DISP_LANES     ? display_dsi_lanes
                                  : e->id == REG_ID_DISP_LANE_RATE ? display_dsi_lane_mbps
                                                                   : display_phy_ldo_mv;
    uint32_t v = 0;
    if (!fn(&v)) {
        return REG_FAILED;
    }
    out->u = v;
    return REG_OK;
}

#define DISP_RO(id_, type_, name_, unit_) \
    {.id = (id_), .type = (type_), .flags = REG_F_RO, .name = (name_), .unit = (unit_), .get_applied = disp_get}

static reg_entry_t const rows[] = {
    DISP_RO(REG_ID_DISP_PCLK, REG_T_U32, "disp.pclk", "Hz"),
    DISP_RO(REG_ID_DISP_REFRESH, REG_T_U32, "disp.refresh", "dHz"), // tenths of a Hz: 468 = 46.8 Hz
    DISP_RO(REG_ID_DISP_LANES, REG_T_U8, "disp.cfg_lanes", NULL),
    DISP_RO(REG_ID_DISP_LANE_RATE, REG_T_U32, "disp.cfg_lane_rate", "Mbps"),
    DISP_RO(REG_ID_DISP_PHY_MV, REG_T_U32, "disp.cfg_phy_mv", "mV"),
};

reg_p4_dom_t const reg_p4_display = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
