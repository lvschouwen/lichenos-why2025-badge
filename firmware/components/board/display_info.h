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

// disp.engine (#468): what turns the strips now, a display_engine_id_t (display_engine.h); false without the direct
// flush (the port's own path then turns them on the CPU). Lock-free: one word the LVGL task writes.
bool display_engine_get(uint32_t *id);
// Switch the engine. The caller holds the LVGL lock (the UI service's, through the registry setter), so no refresh is
// running and the switch lands between frames. False: unknown id, no direct flush, a release image (the key is a bench
// switch), or the PPA has not finished a transfer that timed out.
bool display_engine_set(uint32_t id);
