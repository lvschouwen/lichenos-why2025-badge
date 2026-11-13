// Crash attribution and safe mode (#232): the record in LP RAM, the panic hook, the boot step and "fw app background"
// / "fw app crash". The rules are safe_mode_policy.c (pure, host-tested).
#include "safe_mode.h"
#include "safe_mode_policy.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_sys_app.h"
#include "settings_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const TAG[] = "safe_mode";

_Static_assert(SAFE_MODE_APP_ID_CAP == SAFE_MODE_ID_CAP, "safe_mode.h's id buffer is the record's");

#define LIST_KEY "bg_off" // badge_fw: the apps whose background part is off; a factory reset clears it

// LP RAM (RTC no-init): survives esp_restart and a panic, lost at a power-off and the EN reset (#330). Validated by
// magic and checksum before use (safe_mode_policy.c): after a power-on it is noise.
static RTC_NOINIT_ATTR safe_mode_rec_t s_rec;
// Every writer but the panic handler (which has the other core stalled and must not wait): a critical section, in
// internal RAM and LP RAM only. A panic in the middle of a write leaves a bad checksum: the window is forgotten.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static safe_mode_boot_t s_boot;      // this boot's verdict, written once by safe_mode_boot_step
static atomic_bool      s_boot_done; // s_boot is set
static char             s_off[SAFE_MODE_LIST_MAX]; // the saved list as read at boot and changed since, under s_mux

// ---- the panic hook ------------------------------------------------------------------------------------------------

// Every panic (exceptions, abort, assert, the interrupt watchdog) reaches esp_panic_handler from the port's handler;
// the linker sends that call here first. No allocation, no lock, no log: only reads (the task's context slot, the app
// table, the app's const id) and LP RAM writes. The guard keeps a fault in here from looping: the IDF then restarts.
void __real_esp_panic_handler(void *info);
void __wrap_esp_panic_handler(void *info);
void __wrap_esp_panic_handler(void *info) {
    static bool entered;
    if (!entered) {
        entered = true;
        safe_mode_note_panic(&s_rec, lichen_sys_panic_app_id());
    }
    __real_esp_panic_handler(info);
}

// ---- queries -------------------------------------------------------------------------------------------------------

bool safe_mode_background_off(char const *id) {
    portENTER_CRITICAL(&s_mux);
    bool off = safe_mode_list_has(s_off, id);
    portEXIT_CRITICAL(&s_mux);
    return off;
}

bool safe_mode_crashed_app(char *id, size_t cap, bool *switched_off) {
    if (!atomic_load(&s_boot_done) || !s_boot.app[0] || cap < sizeof(s_boot.app)) {
        return false;
    }
    memcpy(id, s_boot.app, sizeof(s_boot.app));
    if (switched_off) {
        *switched_off = s_boot.background_off;
    }
    return true;
}

bool safe_mode_degraded_boot(void) {
    if (!atomic_load(&s_boot_done)) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    bool degraded = safe_mode_degraded(&s_rec) || s_boot.degraded; // forget may have cleared the window since
    portEXIT_CRITICAL(&s_mux);
    return degraded;
}

void safe_mode_rearm_mark(bool restarting) {
    portENTER_CRITICAL(&s_mux);
    safe_mode_note_rearm(&s_rec, restarting);
    portEXIT_CRITICAL(&s_mux);
}

// ---- the list in the settings --------------------------------------------------------------------------------------

static bool save_list(char const *list) {
    bool ok = list[0] ? settings_set_str(LIST_KEY, list) : settings_erase(LIST_KEY);
    if (!ok) {
        ESP_LOGE(TAG, "the list of apps with their background off (%s) was not saved", list[0] ? list : "empty");
    }
    return ok;
}

static void switch_off(char const *id) {
    char list[SAFE_MODE_LIST_MAX];
    portENTER_CRITICAL(&s_mux);
    bool added = safe_mode_list_add(s_off, sizeof(s_off), id);
    memcpy(list, s_off, sizeof(list));
    portEXIT_CRITICAL(&s_mux);
    if (!added) {
        ESP_LOGE(TAG, "%s: no room in the list to switch its background off; it starts as usual", id);
        return;
    }
    save_list(list); // not saved: off this boot only
}

// ---- fw app background / crash -------------------------------------------------------------------------------------

static void show(void) {
    char list[SAFE_MODE_LIST_MAX];
    portENTER_CRITICAL(&s_mux);
    memcpy(list, s_off, sizeof(list));
    bool valid = safe_mode_valid(&s_rec);
    portEXIT_CRITICAL(&s_mux);
    console_reply("app background: off for %s", list[0] ? list : "no app");
    if (!s_boot.counted) {
        console_reply("app background: this boot is the PSRAM re-arm restart after a crash (not counted again)");
    } else if (s_boot.app[0]) {
        console_reply("app background: this boot follows a crash of %s, %u of %d in %d boots", s_boot.app,
                      s_boot.app_crashes, SAFE_MODE_LIMIT, SAFE_MODE_WINDOW);
    } else {
        console_reply("app background: this boot follows %s", s_boot.crash ? "a crash of no app" : "a clean end");
    }
    console_reply("app background: degraded boot %s%s", safe_mode_degraded_boot() ? "yes" : "no",
                  valid ? "" : " (the window record is not valid)");
}

static void background_on(char const *id) {
    char const *key = id; // an app that is gone since (an update) can still be taken off the list
    char                list[SAFE_MODE_LIST_MAX];
    portENTER_CRITICAL(&s_mux);
    bool was = safe_mode_list_remove(s_off, sizeof(s_off), key);
    safe_mode_forget(&s_rec, key);
    memcpy(list, s_off, sizeof(list));
    portEXIT_CRITICAL(&s_mux);
    if (!was) {
        console_reply("app background: %s was not off", key);
    } else if (save_list(list)) {
        console_reply("app background: %s on again from the next boot, its crashes forgotten", key);
    } else {
        console_reply("app background: %s on, but not saved: it is off again at the next boot", key);
    }
}

#ifdef CONFIG_LICHEN_DEV_COMMANDS
// The faulting task runs as the app: what the panic hook reads is the real attribution path, not a shortcut
static void crash_as(char const *name) {
    if (!console_dev_gate("app crash")) {
        return;
    }
    lichen_app_t const *app = lichen_app_by_id(name); // ids only: lichen_app_by_name is the shell's (layers.conf)
    lichen_ctx_t        ctx = app ? lichen_sys_task_ctx(app) : LICHEN_CTX_NONE;
    if (!app || ctx == LICHEN_CTX_NONE) {
        console_reply("app crash: no app %s", name);
        return;
    }
    console_reply("app crash: %s crashes now (abort as the app), the badge restarts", app->id ? app->id : app->name);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    lichen_ctx_enter(ctx);
    abort();
}
#define USAGE_DEV " | crash <id> (developer mode: a panic as that app)"
#else
#define USAGE_DEV ""
#endif

// "background <id> on" exactly; id copied (cap with the NUL)
static bool parse_on(char const *args, char *id, size_t cap) {
    static char const pre[] = "background ";
    if (strncmp(args, pre, sizeof(pre) - 1) != 0) {
        return false;
    }
    char const *p  = args + sizeof(pre) - 1;
    char const *sp = strchr(p, ' ');
    if (!sp || sp == p || strcmp(sp, " on") != 0 || (size_t)(sp - p) >= cap) {
        return false;
    }
    memcpy(id, p, (size_t)(sp - p));
    id[sp - p] = '\0';
    return true;
}

static void cmd_more(char const *args) {
    char id[SAFE_MODE_ID_CAP];
    if (strcmp(args, "background") == 0) {
        show();
        return;
    }
    if (parse_on(args, id, sizeof(id))) {
        background_on(id);
        return;
    }
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    if (strncmp(args, "crash ", 6) == 0 && args[6]) {
        crash_as(args + 6);
        return;
    }
#endif
    console_reply("usage: app ctx | background [<id> on]" USAGE_DEV);
}

// ---- the boot step -------------------------------------------------------------------------------------------------

static void log_verdict(void) {
    if (!s_boot.counted) {
        ESP_LOGI(TAG, "this boot is the PSRAM re-arm restart after a crash: not counted again");
    } else if (s_boot.app[0]) {
        ESP_LOGW(TAG, "the crash is attributed to %s: %u of %d in the last %d boots%s", s_boot.app, s_boot.app_crashes,
                 SAFE_MODE_LIMIT, SAFE_MODE_WINDOW, s_boot.background_off ? ", its background part is switched off" : "");
    } else if (s_boot.crash) {
        ESP_LOGI(TAG, "the crash is attributed to no app (an OS task, an ISR, a reset without a panic)");
    }
}

void safe_mode_boot_step(bool crashed) {
    char list[SAFE_MODE_LIST_MAX];
    settings_get_str(LIST_KEY, list, sizeof(list), "");
    portENTER_CRITICAL(&s_mux);
    memcpy(s_off, list, sizeof(s_off));
    s_boot = safe_mode_boot(&s_rec, crashed);
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_boot_done, true);
    log_verdict();
    if (s_boot.background_off) {
        switch_off(s_boot.app);
    }
    if (list[0]) {
        ESP_LOGW(TAG, "background part off (safe mode) for %s: fw app background <id> on", list);
    }
    lichen_sys_app_console_more(cmd_more, "app ctx | background [<id> on]" USAGE_DEV
                                          " (contexts; the apps whose background is off after crashes, #232)");
}
