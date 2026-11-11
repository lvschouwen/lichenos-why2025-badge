#pragma once

#include <stdint.h>

// Pure display clock math (host_tests/display_timing_test.c); the ESP reads live in display.c (#317)

// DPI pixel clock in Hz: the divider field of the clock register holds div - 1 (mipi_dsi_ll_set_dpi_clock_div)
uint32_t disp_pclk_hz(uint32_t src_hz, uint32_t div_field);

// Refresh in tenths of a Hz, rounded: pclk / (htotal * vtotal); 0 when a total is 0
uint32_t disp_refresh_dhz(uint32_t pclk_hz, uint32_t htotal, uint32_t vtotal);
