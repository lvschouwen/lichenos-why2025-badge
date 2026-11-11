#pragma once

#include "lvgl.h"

#include <stdbool.h>

// The 720x720 panel driven through LVGL (esp_lvgl_port); draw buffers in PSRAM (#61)
bool display_start(lv_display_t **out);

// Switches the panel off (display-off command; a later display_start() resets and re-inits it). Safe from a shutdown
// handler; does nothing before display_start() succeeded.
void display_blank(void);

// Registry reads (#317): true and *out set, or false before display_start() succeeded (or when the clock is unreadable).
// dpi_hz and refresh come from the clock registers; lanes, lane rate and PHY LDO mV are the configured values.
bool display_dpi_hz(uint32_t *hz);
bool display_refresh_dhz(uint32_t *dhz); // tenths of a Hz
bool display_dsi_lanes(uint32_t *n);
bool display_dsi_lane_mbps(uint32_t *mbps);
bool display_phy_ldo_mv(uint32_t *mv);
