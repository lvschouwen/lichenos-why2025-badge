#pragma once
// #558 spike, developer builds only: the PPA scale + rotate throughput of the planned DOOM video path (#527), measured
// by "fw vbench". display_vbench.c is the command, display.c the one frame (it owns the PPA client and the flip state).

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS

#define VBENCH_SRC_W 320
#define VBENCH_SRC_H 200
// The source in 1/16 steps (ppa_ll.h: sr_scal_x_frag is 4 bits): 320 -> 720 is exactly 36/16, 200 -> 537 is 43/16
#define VBENCH_SCALE_X (36.0f / 16.0f)
#define VBENCH_SCALE_Y (43.0f / 16.0f)

typedef enum {
    VBENCH_OK,
    VBENCH_NO_PPA,  // no direct flush, the PPA client is not there, or an earlier transfer is still hung
    VBENCH_REFUSED, // the driver refused the transfer
    VBENCH_TIMEOUT, // no done: the engine is on the CPU now and the source must not be freed under a late transfer
} vbench_status_t;

typedef struct {
    uint32_t        call_us;  // before the driver call to its return: the source write-back and the output invalidate
    uint32_t        total_us; // before the driver call to the done interrupt
    uint16_t const *fb;       // the frame buffer written (the back one), 720 px per row
    int32_t         x1, x2;   // its output window: panel columns x1..x2 (all 720 rows)
} vbench_frame_t;

// One scaled and rotated frame into the back buffer. The caller holds the LVGL lock and src is a 64-byte aligned PSRAM
// block of VBENCH_SRC_W x VBENCH_SRC_H RGB565. Nothing is shown: no flip, no dirty rectangle, so the caller must
// invalidate the whole screen afterwards.
vbench_status_t display_vbench_frame(void const *src, vbench_frame_t *out);

// display_start: registers "fw vbench"
void display_vbench_register(void);

#else
static inline void display_vbench_register(void) {
}
#endif
