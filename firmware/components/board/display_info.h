#pragma once

#include <stdbool.h>
#include <stdint.h>

// Own header, free of LVGL, so the registry row file and its host test can include it.
// Registry reads (#317): true and *out set, or false before display_start() succeeded (or when the clock is unreadable).
// dpi_hz and refresh come from the clock registers; lanes, lane rate and PHY LDO mV are the configured values.
bool display_dpi_hz(uint32_t *hz);
bool display_refresh_dhz(uint32_t *dhz); // tenths of a Hz
bool display_dsi_lanes(uint32_t *n);
bool display_dsi_lane_mbps(uint32_t *mbps);
bool display_phy_ldo_mv(uint32_t *mv);
