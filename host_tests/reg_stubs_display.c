// Host-test stubs for the getters that reg_p4_display.c points at (#312, #317). Only the display agent edits this file.
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
