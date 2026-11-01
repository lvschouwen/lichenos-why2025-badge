// Settings > SD card (#99): the card's state, Mount/Unmount and Format. sd_mount, sd_unmount and sd_format wait for
// the card's lock and talk to the card, so they run on a short-lived task on core 0 (like the Wi-Fi scan and MeshCore's
// settings) and the answer comes back through lv_async_call. The page itself only reads sd_info(), the cached state,
// once a second (a "fw sd mount" on the console shows up here too). Format asks twice, each time with a careful
// dialog whose countdown cancels: never an erase without two deliberate Enters.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "sd_settings_model.h"
#include "sdcard.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "sdpage";

#define SCREEN_SD   106
#define JOB_STACK   4096 // "fw sd format" runs on the console's 4 KB task; f_mkfs's work buffer is on the heap
#define REFRESH_MS  1000
#define ASK_SECONDS 20

// LVGL task only. The job and its result outlive the page: leaving during a format does not stop it.
static sds_job_t job;
static char      result[64];
static struct {
    lv_obj_t *status, *mount, *format, *result;
} page;                      // all NULL while the page is closed
static sds_actions_t shown; // what the rows show now; its strings are the model's literals

static void set_label(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

// Makes the page match sd_info() and the job; cheap when nothing changed
static void refresh(void) {
    if (!page.status) {
        return;
    }
    sd_info_t info;
    sd_info(&info);
    char status[64];
    sds_status(&info, job, status, sizeof(status));
    set_label(page.status, status);
    sds_actions_t a = sds_actions(info.state, job);
    if (a.mount_label != shown.mount_label) {
        lv_label_set_text(lv_obj_get_child(page.mount, 0), a.mount_label); // a menu row is [label, value]
    }
    if (a.mount_value != shown.mount_value) {
        menu_set_value(page.mount, a.mount_value);
    }
    if (a.format_value != shown.format_value) {
        menu_set_value(page.format, a.format_value);
    }
    shown = a;
    set_label(page.result, result);
}

static sds_actions_t actions_now(sd_info_t *info) {
    sd_info(info);
    return sds_actions(info->state, job);
}

// ---- the job, off the LVGL task ----------------------------------------------------------------------------------

static void job_done(void *arg) {
    sd_info_t info;
    sd_info(&info);
    sds_result(job, (bool)(intptr_t)arg, &info, result, sizeof(result));
    ESP_LOGW(TAG, "%s", result);
    job = SDS_IDLE;
    refresh();
}

static void job_task(void *arg) {
    bool ok;
    switch ((sds_job_t)(intptr_t)arg) {
    case SDS_FORMATTING: ok = sd_format(); break;
    case SDS_UNMOUNTING:
        sd_unmount();
        ok = !sd_mounted(); // it returns nothing: still mounted means the card's lock stayed busy
        break;
    default: ok = sd_mount(); break;
    }
    // lv_async_call is not thread safe: take the LVGL lock like the shell does
    lvgl_port_lock(0);
    lv_async_call(job_done, (void *)(intptr_t)ok);
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void job_start(sds_job_t kind) {
    if (job != SDS_IDLE) {
        return;
    }
    job       = kind;
    result[0] = '\0';
    if (xTaskCreatePinnedToCore(job_task, "sdjob", JOB_STACK, (void *)(intptr_t)kind, 4, NULL, 0) != pdPASS) {
        job = SDS_IDLE;
        snprintf(result, sizeof(result), "Not started: out of memory");
        ESP_LOGW(TAG, "%s", result);
    }
    refresh();
}

// ---- rows --------------------------------------------------------------------------------------------------------

static void on_mount(void *arg) {
    (void)arg;
    sd_info_t info;
    if (actions_now(&info).mount_enabled) {
        job_start(sds_mount_job(info.state));
    }
}

static void format_cancelled(void) {
    snprintf(result, sizeof(result), "Format cancelled");
    refresh();
}

static void format_second_answer(bool yes) {
    sd_info_t info;
    if (!yes || !actions_now(&info).format_enabled) { // the card may have gone during the question
        format_cancelled();
        return;
    }
    job_start(SDS_FORMATTING);
}

static void format_first_answer(bool yes) {
    shell_dialog_t const d = {
        .title        = "Sure? All files on the card are lost.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: format now · Esc: cancel",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    if (!yes || !shell_dialog_countdown(&d, format_second_answer)) {
        format_cancelled();
    }
}

static void on_format(void *arg) {
    (void)arg;
    sd_info_t            info;
    shell_dialog_t const d = {
        .title        = "Erases everything on the card. Format as LICHEN?",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: continue · Esc: cancel",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    if (actions_now(&info).format_enabled && !shell_dialog_countdown(&d, format_first_answer)) {
        format_cancelled();
    }
}

// ---- the page ----------------------------------------------------------------------------------------------------

static void on_refresh(lv_timer_t *t) {
    (void)t;
    refresh();
}

static void on_page_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(&page, 0, sizeof(page));
    memset(&shown, 0, sizeof(shown));
}

static lv_obj_t *build_sd(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "SD card");
    page.status      = menu_note(menu, "");
    lv_obj_set_style_text_color(page.status, lv_color_hex(THEME_TEXT), 0);
    page.mount  = menu_add(menu, "Mount", "", on_mount, NULL);
    page.format = menu_add(menu, "Format", "", on_format, NULL);
    page.result = menu_note(menu, "");
    lv_obj_set_style_text_color(page.result, lv_color_hex(THEME_AMBER), 0);
    menu_note(menu, "Apps' files: " SD_LICHEN_DIR);
    menu_note(menu, "No card detect: after inserting a card, mount it here.");
    if (job == SDS_IDLE) {
        result[0] = '\0'; // an old outcome is news no more
    }
    refresh();
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, lv_timer_create(on_refresh, REFRESH_MS, NULL));
    return screen;
}

void sd_screen_open(void) {
    shell_open(build_sd, SCREEN_SD);
}
