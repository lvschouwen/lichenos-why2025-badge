// See app_data_job.h (#233). A flash write or a walk over the drive stalls whoever waits for it, so it runs on a task of
// its own (LICHEN_PRIO_JOB, core 0), like the Files app's jobs.
#include "app_data_job.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_task.h"

#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "app_data";

#define JOB_STACK 6144 // as the Files app's jobs: the walk is 7 frames deep through FAT, and an NVS erase and the log need the rest
enum { IDLE, RUNNING, DONE };

static atomic_int   s_state;
static adj_result_t s_job; // the request while RUNNING, the result once DONE
static bool         s_allowed;

static void job_task(void *arg) {
    (void)arg;
    switch (s_job.kind) {
    case ADJ_MEASURE:
        s_job.ok = lichen_sys_app_data_measure(s_job.index, &s_job.data) && s_job.data.ok;
        break;
    case ADJ_CLEAR:
        s_job.ok = lichen_sys_app_data_clear(s_job.index, &s_job.clear);
        break;
    case ADJ_BACKGROUND:
        s_job.ok = lichen_sys_app_background_set(s_job.index, s_allowed);
        break;
    }
    ESP_LOGI(TAG, "job %d for app %u -> %s, stack margin %u", (int)s_job.kind, (unsigned)s_job.index,
             s_job.ok ? "ok" : "failed", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store(&s_state, DONE);
}

bool app_data_job_start(adj_kind_t kind, size_t index, bool allowed) {
    int expected = IDLE;
    // A finished job nobody took (its page was left) must not block the next one: its result is dropped
    if (!atomic_compare_exchange_strong(&s_state, &expected, RUNNING) &&
        !(expected == DONE && atomic_compare_exchange_strong(&s_state, &expected, RUNNING))) {
        return false;
    }
    memset(&s_job, 0, sizeof(s_job));
    s_job.kind  = kind;
    s_job.index = index;
    s_allowed   = allowed;
    if (!lichen_task_start(lichen_ctx_self(), "appdata", job_task, NULL, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        ESP_LOGW(TAG, "no memory for an app data job");
        atomic_store(&s_state, IDLE);
        return false;
    }
    return true;
}

bool app_data_job_take(adj_result_t *out) {
    if (atomic_load(&s_state) != DONE) {
        return false;
    }
    *out = s_job;
    atomic_store(&s_state, IDLE);
    return true;
}

bool app_data_job_busy(void) {
    return atomic_load(&s_state) == RUNNING;
}
