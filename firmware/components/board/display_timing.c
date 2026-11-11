#include "display_timing.h"

uint32_t disp_pclk_hz(uint32_t src_hz, uint32_t div_field) {
    return src_hz / (div_field + 1);
}

uint32_t disp_refresh_dhz(uint32_t pclk_hz, uint32_t htotal, uint32_t vtotal) {
    uint64_t const frame = (uint64_t)htotal * vtotal;
    if (frame == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)pclk_hz * 10 + frame / 2) / frame);
}
