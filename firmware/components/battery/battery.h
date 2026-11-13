#pragma once
// The battery service (#11): finds the optional MAX17048 fuel gauge on the camera header's bus (board_i2c.h, J7) and
// polls it. A stock badge has none: one probe at start, one log line, then the task sleeps until "fw battery probe".
// It never powers the badge off: the 3.30 V power-off of battery_model.c is only reported (off_due) until the readings
// are calibrated on the bench (BATTERY_AUTO_OFF in battery_service.c).
#include "battery_svc.h"

#include <stdbool.h>
#include <stdint.h>

// 3 KB in LP RAM (main starts it with LICHEN_STACK_LP_RAM): the deepest path is an ESP_LOG line with a few numbers
// on top of an I2C transfer. The high-water mark is in "fw battery" (stack free) and logged once after the first probe.
#define BATTERY_STACK_BYTES 3072

typedef struct {
    bat_gauge_t        gauge;
    char const        *why;    // ABSENT: what the probe found (a constant string), NULL otherwise
    bool               have;   // a reading passed the checks since the probe
    uint32_t           mv;     // cell voltage
    uint8_t            percent; // the chip's state of charge, rounded
    uint8_t            curve_percent; // battery_model.c's guess from mv, for the calibration comparison
    int32_t            crate_centipct_h;
    bat_trend_t        trend;
    bat_level_t        level;
    bool               off_due; // the model would power off now (never acted on in this build)
    uint32_t           age_ms;  // of the reading, at the last poll
    bool               tmp102_seen; // 0x48 answered at the probe (mod 5; nothing reads it yet)
    uint32_t           polls, io_errors, rejected;
    bat_regs_verdict_t last_reject;
    uint32_t           probe_ms;   // how long the last probe took
    uint32_t           stack_free; // bytes of the task's stack never used
} battery_snap_t;

// The service's loop, run by the task main starts once (lichen_task_start_in, LP RAM). Never returns.
void battery_service_run(void);
// The latest state; any task (a copy under a spinlock, no I/O)
void battery_get(battery_snap_t *out);
// Called on the battery task when the level changes (OK, WARN, OFF), for the UI of a later wave; NULL removes it
void battery_set_level_hook(void (*fn)(bat_level_t level));
// Asks the task to probe the bus again ("fw battery probe"); false when the service is not running
bool battery_service_reprobe(void);
// "fw battery [probe]", developer mode only
void battery_register_console(void);
