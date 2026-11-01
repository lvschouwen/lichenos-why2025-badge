// Lichen as a notification source (#62): once a second a small task on core 0 reads the getters the OS already has
// (crash, PSRAM boot check, clock, update, C6 recovery, SD card, sensors, storage) into a snapshot; sys_notice_rules.c
// (pure) decides what is news and this raises it as source "lichen". Nothing here touches the radio or the C6 link
// beyond reading their status, and nothing touches LVGL: lichen_notify() is safe from any task. The storage drive's
// free space is read once a minute (FatFs may scan its table).
#include "crash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_clock.h"
#include "lichen_notify.h"
#include "lichen_sensors.h"
#include "psram_boot.h"
#include "sdcard.h"
#include "shell_parts.h"
#include "storage.h"
#include "sys_notice_rules.h"
#include "update_screen.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "sys_notices";

#define POLL_MS        1000
#define SETTLE_POLLS   3  // after home shows: the boot steps have all reported by then
#define STORAGE_POLLS  60 // free space once a minute
#define TASK_STACK     4096
#define BODY_CAP       120

static void body_for(sysn_event_t e, sysn_snapshot_t const *s, sd_info_t const *sd, char *out, size_t cap) {
    char a[24], b[24];
    out[0] = '\0';
    switch (e) {
    case SYSN_CRASH: snprintf(out, cap, "Lichen restarted after a crash. Open this for the Crash log."); break;
    case SYSN_PSRAM: psram_boot_bad(out, cap); break;
    case SYSN_CLOCK_IGNORED:
        snprintf(out, cap, "A time answer far from the clock was refused (%u since boot).", (unsigned)s->clock_refused);
        break;
    case SYSN_CLOCK_RECOVERED: snprintf(out, cap, "The clock was wrong and has been set again."); break;
    case SYSN_UPDATED:
    case SYSN_ROLLED_BACK: update_screen_boot_result(out, cap); break;
    case SYSN_C6_RESTARTED: {
        char reason[96] = "";
        if (shell_sources.c6_restarts) {
            shell_sources.c6_restarts(reason, sizeof(reason));
        }
        snprintf(out, cap, "%s (restarts this boot: %u)", reason[0] ? reason : "The radio chip came back",
                 (unsigned)s->c6_restarts);
        break;
    }
    case SYSN_SD_MOUNTED:
    case SYSN_SD_UNMOUNTED:
    case SYSN_SD_REMOVED_ERRORS:
    case SYSN_SD_NOT_FAT_CARD:
    case SYSN_SD_FAILED: sd_status_line(sd, out, cap); break;
    case SYSN_IMU_LOST: snprintf(out, cap, "The motion sensor (BMI270) stopped answering."); break;
    case SYSN_ENV_LOST: snprintf(out, cap, "The air sensor (BME690) stopped answering."); break;
    case SYSN_STORAGE_LOW:
        sd_fmt_size(s->storage_free, a, sizeof(a));
        sd_fmt_size(s->storage_total, b, sizeof(b));
        snprintf(out, cap, "%s free of %s on the storage drive.", a, b);
        break;
    default: break;
    }
}

static void read_snapshot(sysn_snapshot_t *s, sd_info_t *sd, unsigned poll) {
    static lichen_sensors_t sensors; // this task only; too big to keep twice on its stack
    s->crash_recovered        = crash_recovered();
    s->psram_bad              = psram_boot_bad(NULL, 0);
    clock_sync_info_t clock   = clock_sync_info();
    s->clock_refused          = clock.refused;
    s->clock_recovered        = clock.last_verdict == CLOCK_RECOVERED;
    s->update                 = (int)update_screen_boot_result(NULL, 0);
    char reason[4];
    s->c6_restarts = shell_sources.c6_restarts ? shell_sources.c6_restarts(reason, sizeof(reason)) : 0;
    sd_info(sd);
    s->sd_state        = (int)sd->state;
    s->sensors_running = lichen_sensors_running();
    lichen_sensors_get(&sensors);
    s->imu_valid = sensors.imu_valid;
    s->env_valid = sensors.env_valid;
    if (poll % STORAGE_POLLS == 0) {
        s->storage_known = storage_free_bytes(&s->storage_free, &s->storage_total);
    }
}

static void notices_task(void *arg) {
    (void)arg;
    static sysn_state_t    state;
    static sysn_snapshot_t snap;
    static sd_info_t       sd;
    sysn_event_t           events[SYSN_EVENT_COUNT];
    char                   body[BODY_CAP];
    unsigned               settled = 0;
    while (settled < SETTLE_POLLS) { // the boot screen first, then a moment for the last boot steps
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        settled = shell_home_shown() ? settled + 1 : 0;
    }
    for (unsigned poll = 0;; poll++) {
        read_snapshot(&snap, &sd, poll);
        size_t n = sysn_step(&state, &snap, events, SYSN_EVENT_COUNT);
        for (size_t i = 0; i < n; i++) {
            sysn_info_t const info = sysn_info(events[i]);
            body_for(events[i], &snap, &sd, body, sizeof(body));
            ESP_LOGW(TAG, "%s: %s", info.title, body);
            lichen_notify_to("lichen", (ln_level_t)info.level, info.target, info.title, body, info.flags);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void sys_notices_start(void) {
    if (xTaskCreatePinnedToCore(notices_task, "sys_notices", TASK_STACK, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory: Lichen raises no notifications of its own");
    }
}
