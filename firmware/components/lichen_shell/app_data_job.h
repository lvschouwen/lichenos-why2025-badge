#pragma once
// Settings > Apps (#233): the file and flash work of one app's page (measuring its data, Clear data, saving the
// Background switch) on a short-lived job task, never on the LVGL task. One job at a time; the page starts one, polls
// app_data_job_take from an LVGL timer and reads the result. A job outlives the page that started it.

#include "lichen_sys_app_data.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum { ADJ_MEASURE, ADJ_CLEAR, ADJ_BACKGROUND } adj_kind_t;

typedef struct {
    adj_kind_t         kind;
    size_t             index;      // the app (the OS table's index) the job was for
    bool               ok;         // MEASURE: the figures are complete; CLEAR: everything went; BACKGROUND: it was saved
    lichen_app_data_t  data;       // MEASURE
    lichen_app_clear_t clear;      // CLEAR
} adj_result_t;

// Starts a job for the app at index; allowed is the wanted switch state of ADJ_BACKGROUND. False when another job runs
// or the task could not start (no memory); nothing runs then.
bool app_data_job_start(adj_kind_t kind, size_t index, bool allowed);
// True once per finished job; fills *out
bool app_data_job_take(adj_result_t *out);
bool app_data_job_busy(void);
