#pragma once

#include "stress_calc.h"

#include <stdbool.h>
#include <stdint.h>

// The Stress app's run (#278): the workers, the 10 s monitor and the report. Lines go out through ESP_LOG with the tag
// "stress" only (UART and the log store alike, from the tile or from `fw stress`); console replies never repeat them.

typedef struct {
    bool            running;  // a run is on, or its workers are still stopping
    bool            ended;    // a run ended since boot: run and counts are its result
    bool            sd, fs;   // the SD and /storage workers run (or ran)
    unsigned        minutes;
    uint32_t        t_s;      // seconds into the run (its length once it ended)
    stress_sample_t last;     // the newest 10 s sample (zeros before the first)
    stress_run_t    run;
    stress_counts_t counts;
} stress_view_t;

// Starts a run of minutes (1..STRESS_MIN_MAX) on the OS's tasks. NULL when it started, else why not (a short phrase).
// Any task, the LVGL task included (the file work starts on the monitor's task).
char const *stress_start(unsigned minutes);
// Asks the run to stop; the first reason wins. Nothing when no run is on. Any task.
void stress_stop(stress_reason_t why);
// A copy of the run's state, for the screen and `fw stress status`. Any task.
void stress_view(stress_view_t *out);
bool stress_running(void);
