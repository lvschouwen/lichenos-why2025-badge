// Lichen as a notification source (#62): once a second a small task on core 0 reads the getters the OS already has
// (crash, PSRAM boot check, clock, update, C6 recovery, SD card, sensors, storage) into a snapshot; sys_notice_rules.c
// (pure) decides what is news and this raises it as source "lichen". Nothing here touches the radio or the C6 link
// beyond reading their status, and nothing touches LVGL: lichen_notify() is safe from any task. The storage drive's
// free space is read every 10 min, at idle priority (LittleFS walks its blocks, #300, #357: sys_notice_rules.h). Each
// step is timed; a slow one is a warning. An OS service since #281 (lichen_watch_start, display or not): the update
// result and "home is shown" come in as hooks.
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_notify.h"
#include "lichen_os.h"
#include "lichen_sensors.h"
#include "lichen_sys_c6.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_crash.h"
#include "lichen_sys_storage.h"
#include "lichen_task.h"
#include "lichen_sys_app.h"
#include "lichen_sys_notify.h"
#include "psram_boot.h"
#include "psram_speed.h"
#include "safe_mode.h"
#include "storage.h"
#include "sys_notice_rules.h"

#include <stdatomic.h>
#include <stdio.h>

#include <string.h>

static char const TAG[] = "sys_notices";

#define POLL_MS        1000
#define SETTLE_POLLS   3  // after home shows: the boot steps have all reported by then
#define TASK_STACK     4096 // peak 1.2 KB measured (#107)
#define STACK_LOG_POLL 120 // the stack high-water mark is logged once, two minutes in
#define BODY_CAP       120

static lichen_watch_hooks_t hooks; // set once before the task starts
static atomic_bool          s_boot_told;

bool lichen_watch_boot_told(void) {
    return atomic_load(&s_boot_told);
}

// Names the app the crash is attributed to (#232), by its name when it is still registered
static void crash_body(char *out, size_t cap) {
    char     id[SAFE_MODE_APP_ID_CAP];
    unsigned crashes = 0;
    bool     off     = false;
    bool     in_cb   = false;
    if (!safe_mode_crashed_app(id, sizeof(id), &crashes, &off, &in_cb)) {
        snprintf(out, cap, "Lichen restarted after a crash. Open this for the Crash log.");
        return;
    }
    lichen_app_t const *app  = lichen_app_by_id(id);
    char const         *name = app && app->name ? app->name : id;
    if (off) { // #427: a callback crash loop is switched off too
        snprintf(out, cap, "%s crashed again: its background part is off until switched on (fw app background %s on).",
                 name, id);
    } else if (in_cb && crashes > 1) {
        snprintf(out, cap, "%s crashed in a screen callback %u times in the last %d boots. Open this for the Crash log.",
                 name, crashes, SAFE_MODE_WINDOW_BOOTS);
    } else if (in_cb) {
        snprintf(out, cap, "%s crashed in a screen callback and Lichen restarted. Open this for the Crash log.", name);
    } else if (crashes > 1) {
        snprintf(out, cap, "%s crashed %u times in the last %d boots. Open this for the Crash log.", name, crashes,
                 SAFE_MODE_WINDOW_BOOTS);
    } else {
        snprintf(out, cap, "%s crashed and Lichen restarted. Open this for the Crash log.", name);
    }
}

static void body_for(sysn_event_t e, sysn_snapshot_t const *s, lichen_sd_info_t const *sd, char *out, size_t cap) {
    char a[24], b[24];
    out[0] = '\0';
    switch (e) {
    case SYSN_CRASH: crash_body(out, cap); break;
    case SYSN_PSRAM: psram_boot_bad(out, cap); break;
    case SYSN_PSRAM_80: psram_speed_fell(out, cap); break;
    case SYSN_CLOCK_IGNORED:
        snprintf(out, cap, "A time answer far from the clock was refused (%u since boot).", (unsigned)s->clock_refused);
        break;
    case SYSN_CLOCK_RECOVERED: snprintf(out, cap, "The clock was wrong and has been set again."); break;
    case SYSN_UPDATED:
    case SYSN_ROLLED_BACK:
        if (hooks.update_result) {
            hooks.update_result(out, cap);
        }
        break;
    case SYSN_C6_RESTARTED: {
        char reason[96] = "";
        lichen_sys_c6_restarts(reason, sizeof(reason));
        snprintf(out, cap, "%s (restarts this boot: %u)", reason[0] ? reason : "The radio chip came back",
                 (unsigned)s->c6_restarts);
        break;
    }
    case SYSN_SD_MOUNTED:
    case SYSN_SD_UNMOUNTED:
    case SYSN_SD_REMOVED_ERRORS:
    case SYSN_SD_NOT_FAT_CARD:
    case SYSN_SD_FAILED: lichen_sys_sd_status_line(sd, out, cap); break;
    case SYSN_IMU_LOST: snprintf(out, cap, "The motion sensor (BMI270) stopped answering."); break;
    case SYSN_ENV_LOST: snprintf(out, cap, "The air sensor (BME690) stopped answering."); break;
    case SYSN_STORAGE_LOW:
        lichen_sys_sd_fmt_size(s->storage_free, a, sizeof(a));
        lichen_sys_sd_fmt_size(s->storage_total, b, sizeof(b));
        snprintf(out, cap, "%s free of %s on the internal storage.", a, b);
        break;
    case SYSN_STORAGE_UNMOUNTED:
        snprintf(out, cap, "The internal storage did not mount and was not formatted, so nothing on it is lost. MeshCore stays off.");
        break;
    default: break;
    }
}

static sysn_timing_t timing; // this task only
// fw soak (#419): the warnings' counts since boot (timing's own are per warning); this task writes, any task reads
static atomic_uint s_slow_steps, s_slow_max_ms, s_count_n, s_count_last_ms, s_count_max_ms;

void lichen_watch_slow_counts(lichen_watch_slow_t *out) {
    out->storage_known     = atomic_load(&s_count_n) > 0;
    out->storage_last_ms   = atomic_load(&s_count_last_ms);
    out->storage_max_ms    = atomic_load(&s_count_max_ms);
    out->steps_slow        = atomic_load(&s_slow_steps);
    out->steps_slow_max_ms = atomic_load(&s_slow_max_ms);
}

static int64_t step_done(sysn_step_id_t step, int64_t since_us) {
    int64_t     now_us = esp_timer_get_time();
    uint32_t    ms     = (uint32_t)((now_us - since_us) / 1000);
    sysn_slow_t slow;
    if (ms > SYSN_SLOW_MS) { // the same limit as the warning, which is rate-limited per step
        atomic_fetch_add(&s_slow_steps, 1);
        if (ms > atomic_load(&s_slow_max_ms)) {
            atomic_store(&s_slow_max_ms, ms);
        }
    }
    if (sysn_time_step(&timing, step, ms, (uint64_t)now_us / 1000, &slow)) {
        ESP_LOGW(TAG, "%s took %u ms (%u slow since the last warning, longest %u ms; over %u ms is slow)",
                 sysn_step_name(step), (unsigned)ms, (unsigned)slow.count, (unsigned)slow.max_ms, SYSN_SLOW_MS);
    }
    return now_us;
}

// The OS's one regular count of /storage (#300): it also refreshes the value About, Files and MeshCore read. The walk
// takes seconds on a full drive (#357) and this task's priority is above IDLE0's, so it runs at idle priority: IDLE0
// shares core 0 by time slice and feeds the task watchdog. The drive's lock is a mutex: a task that waits on it
// (MeshCore, priority 3) lends this one its priority for the rest of the walk, and IDLE0 starves again until the walk
// ends. #362 (LittleFS's caches in internal RAM) removes the per-read heap walk that makes the count slow.
static bool count_storage(uint64_t *free_bytes, uint64_t *total_bytes) {
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    int64_t t  = esp_timer_get_time();
    bool    ok = storage_free_bytes(free_bytes, total_bytes);
    vTaskPrioritySet(NULL, prio);
    // Every count (every 10 min): the soak shows how the walk grows with the drive
    unsigned const ms = (unsigned)((esp_timer_get_time() - t) / 1000);
    atomic_store(&s_count_last_ms, ms);
    if (ms > atomic_load(&s_count_max_ms)) {
        atomic_store(&s_count_max_ms, ms);
    }
    atomic_fetch_add(&s_count_n, 1);
    ESP_LOGI(TAG, "storage count: %u ms wall time at idle priority (%s)", ms, ok ? "known" : "unknown");
    return ok;
}

static void read_snapshot(sysn_snapshot_t *s, lichen_sd_info_t *sd, unsigned poll) {
    static lichen_sensors_t sensors; // this task only; too big to keep twice on its stack
    int64_t t                 = esp_timer_get_time();
    s->crash_recovered        = lichen_sys_crash_recovered();
    s->storage_unmounted      = !lichen_sys_storage_mounted();
    s->psram_bad              = psram_boot_bad(NULL, 0);
    s->psram_fell             = psram_speed_fell(NULL, 0);
    lichen_clock_sync_t clock = lichen_sys_clock_sync_info();
    s->clock_refused          = clock.refused;
    s->clock_recovered        = clock.last_verdict == LICHEN_CLOCK_RECOVERED;
    s->update                 = hooks.update_result ? hooks.update_result(NULL, 0) : SYSN_UPD_NONE;
    char reason[4];
    s->c6_restarts = lichen_sys_c6_restarts(reason, sizeof(reason));
    t              = step_done(SYSN_STEP_GETTERS, t);
    lichen_sys_sd_info(sd);
    s->sd_state = (int)sd->state;
    t           = step_done(SYSN_STEP_SD, t);
    s->sensors_running = lichen_sensors_running();
    lichen_sensors_get(&sensors);
    s->imu_valid = sensors.imu_valid;
    s->env_valid = sensors.env_valid;
    t            = step_done(SYSN_STEP_SENSORS, t);
    if (sysn_storage_due(poll)) {
        s->storage_known = count_storage(&s->storage_free, &s->storage_total);
        step_done(SYSN_STEP_STORAGE, t);
    } else { // a fresher count by OTA or "fw storage" reaches the notices at once; never touches the drive
        s->storage_known = storage_free_bytes_cached(&s->storage_free, &s->storage_total);
    }
}

// The log line has the notice's level (#113)
static esp_log_level_t log_level(ln_level_t level) {
    return level == LN_ERROR ? ESP_LOG_ERROR : level == LN_WARN ? ESP_LOG_WARN : ESP_LOG_INFO;
}

static void notices_task(void *arg) {
    (void)arg;
    static sysn_state_t     state;
    static sysn_snapshot_t  snap;
    static lichen_sd_info_t sd;
    sysn_event_t            events[SYSN_EVENT_COUNT];
    char                    body[BODY_CAP];
    unsigned                settled = 0;
    while (settled < SETTLE_POLLS) { // the boot screen first (when there is one), then a moment for the last boot steps
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        settled = !hooks.ui_ready || hooks.ui_ready() ? settled + 1 : 0;
    }
    for (unsigned poll = 0;; poll++) {
        read_snapshot(&snap, &sd, poll);
        size_t n = sysn_step(&state, &snap, events, SYSN_EVENT_COUNT);
        for (size_t i = 0; i < n; i++) {
            sysn_info_t const info = sysn_info(events[i]);
            body_for(events[i], &snap, &sd, body, sizeof(body));
            ESP_LOG_LEVEL(log_level((ln_level_t)info.level), TAG, "%s: %s", info.title, body);
            lichen_sys_notify_to((ln_level_t)info.level, info.target, info.title, body, info.flags);
        }
        atomic_store(&s_boot_told, true); // the first step raised the boot's facts
        if (poll == STACK_LOG_POLL) {
            ESP_LOGI(TAG, "stack: %u bytes never used; longest getters %u ms, SD info %u ms, sensors %u ms, "
                          "storage count %u ms",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)timing.max_ms[SYSN_STEP_GETTERS],
                     (unsigned)timing.max_ms[SYSN_STEP_SD], (unsigned)timing.max_ms[SYSN_STEP_SENSORS],
                     (unsigned)timing.max_ms[SYSN_STEP_STORAGE]);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void sys_notices_start(lichen_watch_hooks_t const *h) {
    if (h) {
        hooks = *h;
    }
    // LP RAM (#269): once a second, and it reads /storage free space (LittleFS), so internal-capable but never DMA
    if (!lichen_task_start_in(lichen_sys_ctx_os(), "sys_notices", notices_task, NULL, TASK_STACK,
                              LICHEN_PRIO_BACKGROUND, LICHEN_STACK_LP_RAM, NULL)) {
        ESP_LOGE(TAG, "no memory: Lichen raises no notifications of its own");
    }
}

void lichen_watch_start(lichen_watch_hooks_t const *h) {
    thermal_start(h); // the safety half first: it must run even if the notices' task finds no memory
    sys_notices_start(h);
}
