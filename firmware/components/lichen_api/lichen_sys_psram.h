#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the shell shows and asks of the PSRAM guard (#284): the Bench and Settings pages and the stress run go through
// here, never to psram_guard's headers. System surface: tools/layers.conf forbids lichen_sys_* in L6. The speed switch
// is the guard's (#121); without the patched IDF everything reports 80 MHz, not supported.

// The speed PSRAM init chose this boot, MHz. Any task.
unsigned lichen_sys_psram_mhz(void);
// True when the IDF can run 120 MHz. Any task.
bool lichen_sys_psram_speed_supported(void);
// Records the speed for the next boot (the caller restarts); false: unsupported or not saved. Any task.
bool lichen_sys_psram_speed_choice(unsigned target_mhz);
// The Settings row's value ("120 MHz failed, back at 80"); returns the length. Any task.
size_t lichen_sys_psram_speed_value(char *out, size_t cap);
// The speed the Settings row would switch to, MHz. Any task.
unsigned lichen_sys_psram_speed_other(void);
// A 120 MHz trial is running (MeshCore does not start meanwhile). Any task.
bool lichen_sys_psram_trial_pending(void);
// The tuning line ("phase 67.50 (run 4/4), delayline idx 15 ..."); returns the length. Any task.
size_t lichen_sys_psram_tuning_text(char *out, size_t cap);

// One DMA gate round over PSRAM and the SD card (3 MB of PSRAM and 8 KB of internal DMA RAM for a few seconds): BUSY =
// another gate holds the engine, NOT_RUN = no memory or no AXI-GDMA. Not on the LVGL task.
typedef enum {
    LICHEN_SYS_PSRAM_ROUND_PASS,
    LICHEN_SYS_PSRAM_ROUND_FAIL,
    LICHEN_SYS_PSRAM_ROUND_BUSY,
    LICHEN_SYS_PSRAM_ROUND_NOT_RUN
} lichen_sys_psram_round_t;
typedef struct {
    uint32_t bad_bytes;
    unsigned errors;
    bool     sd_skipped;
} lichen_sys_psram_round_info_t;
// info may be NULL; zeros unless the round ran.
lichen_sys_psram_round_t lichen_sys_psram_round(lichen_sys_psram_round_info_t *info);
