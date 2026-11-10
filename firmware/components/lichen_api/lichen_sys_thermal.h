#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The thermal watchdog's state for the shell (#10, #281): the status bar's HOT, Settings > Battery, the attention light
// and the stress run read it. The watchdog itself is an OS service (lichen_os/thermal.c, started by main whether or
// not a display came up); its rules are lichen_os/thermal_rules.h. System surface: tools/layers.conf forbids
// lichen_sys_* in L6.

typedef enum { TH_SRC_BOARD, TH_SRC_DIE, TH_SRC_COUNT } th_src_t;
typedef enum { TH_OK, TH_WARN, TH_SHUTDOWN } th_level_t;

typedef struct {
    th_level_t level;
    bool       blind;          // no source has answered for TH_BLIND_S
    bool       simulated;      // `fw thermal test` is feeding the board source
    bool       valid[TH_SRC_COUNT];
    float      c[TH_SRC_COUNT]; // what the rules see (the board value has the heater bias taken off)
    float      bme_raw_c;
    uint32_t   implausible[TH_SRC_COUNT];
    th_src_t   cause;
    bool       cause_rise;
    float      cause_c;
} lichen_sys_thermal_info_t;

// The last poll's view; all zero (TH_OK) before the first one. Any task.
void lichen_sys_thermal_info(lichen_sys_thermal_info_t *out);

#ifdef __cplusplus
}
#endif
