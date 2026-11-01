#include "display_rot.h"

display_rect_t display_rot90_block(display_rect_t ui, int32_t ui_hres) {
    display_rect_t const panel = {
        .x1 = ui.y1,
        .y1 = ui_hres - 1 - ui.x2,
        .x2 = ui.y2,
        .y2 = ui_hres - 1 - ui.x1,
    };
    return panel;
}
