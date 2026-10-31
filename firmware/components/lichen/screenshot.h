// A picture of what the panel shows (#30), for tools/badge.sh screenshot. Called from the update server's task.
#pragma once

#include <stdint.h>

// Render the active screen plus the status bar and banner into a PSRAM buffer, as RGB565 in the logical orientation
// (before the panel's 90 degree turn). Returns 0, or the HTTP status to answer with: 503 when the UI lock isn't free,
// 404 without a screen, 500 when memory or rendering fails. The buffer stays valid until screenshot_free().
int screenshot_take(uint8_t const **pixels, uint32_t *width, uint32_t *height, uint32_t *stride);

// Release the buffer of the last screenshot_take
void screenshot_free(void);
