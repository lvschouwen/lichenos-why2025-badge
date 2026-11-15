// Host-test stubs for the getters and the setter that reg_p4_display.c points at (#312, #317, #468). Only the display agent edits this file.
#include "reg_p4.h"

bool display_dpi_hz(uint32_t *hz) {
    *hz = 36000000;
    return true;
}

bool display_refresh_dhz(uint32_t *dhz) {
    *dhz = 468;
    return true;
}

bool display_dsi_lanes(uint32_t *n) {
    *n = 2;
    return true;
}

bool display_dsi_lane_mbps(uint32_t *mbps) {
    *mbps = 1000;
    return true;
}

bool display_phy_ldo_mv(uint32_t *mv) {
    *mv = 2500;
    return true;
}

// disp.engine (#468): the stub display applies what it is given; reg_service_test reads disp_engine_sets
static uint32_t engine = 0;
unsigned        disp_engine_sets;

bool display_engine_get(uint32_t *id) {
    *id = engine;
    return true;
}

reg_status_t reg_p4_disp_engine_set(reg_entry_t const *e, reg_value_t const *v) {
    (void)e;
    engine = v->u;
    disp_engine_sets++;
    return REG_OK;
}
