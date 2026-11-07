// Settings > Backup (#169): save the settings to /sd/lichen/settings.txt and load them back, e.g. after a factory reset or
// onto another badge. The NVS and SD work runs on a short-lived task on core 0 (as sd_screen.c's jobs do) and the answer
// comes back through lv_async_call. A load is two steps: the file is read and checked first and a careful dialog says
// how many settings will change; only a yes writes them. The rules and the list of settings are
// lichen_api/settings_backup.c; secrets are not in it, so they are neither saved nor loaded.
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_task.h"
#include "menu.h"
#include "screen_ids.h"
#include "settings_backup_io.h"
#include "shell_parts.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "backup";

#define JOB_STACK   6144
#define ASK_SECONDS 20
#define SHOW_SECONDS 20

typedef enum { JOB_NONE, JOB_SAVE, JOB_PLAN, JOB_APPLY } job_t;

// The running job and its answer, written by the task before it queues job_done. LVGL task reads them.
static job_t                  s_job;
static sbk_status_t           s_status;
static size_t                 s_applied;
static settings_backup_plan_t s_plan; // too big for the LVGL task's stack
static lv_obj_t              *s_save_row, *s_load_row;

static void tell(char const *text) {
    static char kept[220]; // the dialog copies it, but the call may outlive the caller's buffer
    snprintf(kept, sizeof(kept), "%s", text);
    shell_dialog_t const d = {
        .title         = kept,
        .hint          = "Enter: close",
        .seconds       = SHOW_SECONDS,
        .confirm_label = "close",
    };
    shell_dialog_countdown(&d, NULL);
}

static char const *status_words(sbk_status_t s) {
    switch (s) {
        case SBK_NO_CARD: return "No SD card in the badge.";
        case SBK_NO_FILE: return "No backup found on the SD card (" SETTINGS_BACKUP_PATH ").";
        case SBK_MEMORY: return "Not enough memory. Restart the badge and try again.";
        default: return "The SD card or the settings store could not be used.";
    }
}

static bool job_start(job_t kind);

static void apply_answer(bool yes) {
    if (s_job != JOB_NONE) {
        return;
    }
    if (!yes) {
        settings_backup_discard(); // the held PSRAM goes back now, not at the next plan
        return;
    }
    job_start(JOB_APPLY);
}

// The questions and results, from job_done
static void show_outcome(void) {
    char text[220];
    switch (s_job) {
        case JOB_SAVE:
            snprintf(text, sizeof(text), "%s", s_status == SBK_OK ? "Settings saved to " SETTINGS_BACKUP_PATH "." : status_words(s_status));
            tell(text);
            break;
        case JOB_PLAN:
            if (s_status == SBK_OK) {
                static char title[220];
                snprintf(title, sizeof(title), "%s Load them?", s_plan.summary);
                shell_dialog_t const d = {
                    .title = title, .seconds = ASK_SECONDS, .careful = true, .confirm_label = "load"};
                shell_dialog_countdown(&d, apply_answer);
            } else if (s_status == SBK_NOTHING) {
                tell("The backup matches the settings on the badge. Nothing to load.");
            } else if (s_status == SBK_INVALID) {
                char why[100];
                sb_error_text(&s_plan.result, why, sizeof(why));
                snprintf(text, sizeof(text), "Not loaded: %s Nothing was changed.", why);
                tell(text);
            } else {
                tell(status_words(s_status));
            }
            break;
        case JOB_APPLY:
            if (s_status == SBK_OK) {
                snprintf(text, sizeof(text), "Loaded %u setting%s. Restart the badge to apply all of them.", (unsigned)s_applied,
                         s_applied == 1 ? "" : "s");
            } else {
                snprintf(text, sizeof(text), "Only %u settings could be loaded. Try again.", (unsigned)s_applied);
            }
            tell(text);
            break;
        default: break;
    }
}

static void job_done(void *arg) {
    (void)arg;
    // The wait is over: "Saving..."/"Reading..." go (#175). NULL once the page is gone.
    if (s_save_row) {
        menu_set_value(s_save_row, "");
    }
    if (s_load_row) {
        menu_set_value(s_load_row, "");
    }
    show_outcome();
    s_job = JOB_NONE;
}

static void job_task(void *arg) {
    job_t kind = (job_t)(intptr_t)arg;
    if (kind == JOB_SAVE) {
        s_status = settings_backup_save();
    } else if (kind == JOB_PLAN) {
        s_status = settings_backup_plan(&s_plan);
    } else {
        s_status = settings_backup_apply(&s_applied);
    }
    ESP_LOGI(TAG, "job stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    lichen_sys_ui_call(job_done, NULL, 0); // waits for the LVGL lock as long as it takes
}

static bool job_start(job_t kind) {
    if (s_job != JOB_NONE) {
        return false;
    }
    s_job = kind;
    if (!lichen_task_start(NULL, "backupjob", job_task, (void *)(intptr_t)kind, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        s_job = JOB_NONE;
        tell("Not started: out of memory.");
        return false;
    }
    return true;
}

static void on_save(void *arg) {
    (void)arg;
    if (job_start(JOB_SAVE) && s_save_row) {
        menu_set_value(s_save_row, "Saving...");
    }
}

static void on_load(void *arg) {
    (void)arg;
    if (job_start(JOB_PLAN) && s_load_row) {
        menu_set_value(s_load_row, "Reading...");
    }
}

static void forget_rows(lv_event_t *e) {
    (void)e;
    s_save_row = s_load_row = NULL;
    if (s_job == JOB_NONE) {
        settings_backup_discard();
    }
}

static lv_obj_t *build_backup(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Settings backup");
    s_save_row       = menu_add(menu, "Save settings to SD card", "", on_save, NULL);
    s_load_row       = menu_add(menu, "Load settings from SD card", "", on_load, NULL);
    menu_note(menu, "The file is " SETTINGS_BACKUP_PATH ", plain text you can read and edit. It holds the display, time zone, "
                    "notification and weather settings.");
    menu_note(menu, "Wi-Fi passwords, the update token and the MeshCore identity and keys are never saved. A load checks "
                    "every line first, shows what changes and refuses a file with a wrong value. Restart the badge after "
                    "loading.");
    lv_obj_add_event_cb(screen, forget_rows, LV_EVENT_DELETE, NULL);
    return screen;
}

void backup_screen_open(void *arg) {
    (void)arg;
    shell_open(build_backup, SCREEN_BACKUP);
}
