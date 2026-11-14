// Crash attribution and safe mode (#232): the record in LP RAM, the panic hook, the boot step and "fw app background"
// / "fw app crash". The rules are safe_mode_policy.c (pure, host-tested).
#include "safe_mode.h"
#include "safe_mode_policy.h"
#include "os_cmd_rules.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/panic_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_sys_app.h"
#include "lichen_sys_ui.h"
#include "lichen_ui.h"
#include "settings_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const TAG[] = "safe_mode";

// A callback crash's verdict (#427): the count is the app's crashes of both kinds, toward SAFE_MODE_CB_LIMIT
#define CB_VERDICT "%s crashed %u times in the last %d boots, the last in a screen callback (no strike; %d switch it " \
                   "off)"

_Static_assert(SAFE_MODE_APP_ID_CAP == SAFE_MODE_ID_CAP && SAFE_MODE_APP_ID_CAP == LICHEN_PANIC_ID_CAP,
               "safe_mode.h's id buffer is the record's and the panic view's");
_Static_assert(SAFE_MODE_WINDOW_BOOTS == SAFE_MODE_WINDOW, "safe_mode.h's window is the policy's");

#define LIST_KEY "bg_off" // badge_fw: the apps whose background part is off; a factory reset clears it

// LP RAM (RTC no-init): survives esp_restart and a panic, lost at a power-off and the EN reset (#330). Validated by
// magic and checksum before use (safe_mode_policy.c): after a power-on it is noise.
static RTC_NOINIT_ATTR safe_mode_rec_t s_rec;
// Every writer but the panic handler (which has the other core stalled and must not wait): a critical section, in
// internal RAM and LP RAM only. A panic in the middle of a write leaves a bad checksum: the window is forgotten.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static safe_mode_boot_t s_boot;      // this boot's verdict, written once by safe_mode_boot_step
static atomic_bool      s_boot_done; // s_boot is set
static atomic_bool      s_switched;  // start_apps switched the crashed app's background off this boot
static char             s_off[SAFE_MODE_LIST_MAX]; // the saved list as read at boot and changed since, under s_mux

// ---- the panic hook ------------------------------------------------------------------------------------------------

// Every panic (exceptions, abort, assert, the interrupt watchdog) reaches esp_panic_handler from the port's handler;
// the linker sends that call here first. No allocation, no lock, no log, and nothing behind the PSRAM cache (XIP from
// PSRAM): this is IRAM_ATTR, safe_mode_rec.c is noflash (linker.lf), lichen_sys_panic_app_id is IRAM_ATTR and reads
// internal RAM only, the record is LP RAM. The guard keeps a fault in here from looping: the IDF then restarts.
// Only the core the panic is about blames its task: a cache error whose CPU the hardware cannot tell (info->core -1,
// handled by core 0) or a fault the IDF reports for the other core blames nobody.
void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
    static bool entered;
    if (!entered) {
        entered          = true;
        bool        mine = info && info->core == esp_cpu_get_core_id();
        char const *id   = mine ? lichen_sys_panic_app_id() : NULL;
        safe_mode_note_panic(&s_rec, id, id && lichen_sys_panic_in_ui_cb()); // #349, #427: the callback limit
    }
    __real_esp_panic_handler(info);
}

// ---- queries -------------------------------------------------------------------------------------------------------

// One change of the list at a time (#461): it spans a flash write, so a mutex, not s_mux. Static storage, created first
// in safe_mode_boot_step. Taken by the setter (console task, Apps page job: never the LVGL task) and by switch_off (the
// boot task, only once s_boot_done, which boot_step sets after creating it); the setter never calls switch_off and
// switch_off calls nothing that takes it, so neither can wait on itself. s_off reads stay on s_mux alone.
static StaticSemaphore_t s_set_buf;
static SemaphoreHandle_t s_set_lock;

static void set_take(void *arg) {
    (void)arg;
    if (s_set_lock) {
        xSemaphoreTake(s_set_lock, portMAX_DELAY);
    }
}

static void set_give(void *arg) {
    (void)arg;
    if (s_set_lock) {
        xSemaphoreGive(s_set_lock);
    }
}

static void switch_off(char const *id);

bool safe_mode_background_off(char const *id, bool may_switch_off) {
    portENTER_CRITICAL(&s_mux);
    bool off = safe_mode_list_has(s_off, id);
    portEXIT_CRITICAL(&s_mux);
    if (!off && atomic_load(&s_boot_done) && safe_mode_switch_off(&s_boot, id, may_switch_off)) {
        switch_off(id); // a list that did not save still keeps it off this boot
        atomic_store(&s_switched, true);
        off = true;
    }
    return off;
}

bool safe_mode_crashed_app(char *id, size_t cap, unsigned *crashes, bool *switched_off, bool *in_callback) {
    if (!atomic_load(&s_boot_done) || !s_boot.app[0] || cap < sizeof(s_boot.app)) {
        return false;
    }
    memcpy(id, s_boot.app, sizeof(s_boot.app));
    if (crashes) {
        *crashes = s_boot.in_callback ? s_boot.named_crashes : s_boot.app_crashes; // toward the limit that applies
    }
    if (switched_off) {
        *switched_off = atomic_load(&s_switched);
    }
    if (in_callback) {
        *in_callback = s_boot.in_callback;
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

// Only for an app not on the list yet (safe_mode_background_off asks first), so a boot that changes nothing writes nothing
static void switch_off(char const *id) {
    char list[SAFE_MODE_LIST_MAX];
    ESP_LOGW(TAG, "%s crashed %u times in %d boots%s: its background part is off until fw app background %s on", id,
             s_boot.named_crashes, SAFE_MODE_WINDOW, s_boot.in_callback ? " (in a screen callback)" : "", id);
    set_take(NULL); // a setter's save must not interleave with this add and save (#461)
    portENTER_CRITICAL(&s_mux);
    bool added = safe_mode_list_add(s_off, sizeof(s_off), id);
    memcpy(list, s_off, sizeof(list));
    portEXIT_CRITICAL(&s_mux);
    if (!added) {
        set_give(NULL);
        ESP_LOGE(TAG, "%s: no room in the list to switch its background off; it starts as usual", id);
        return;
    }
    save_list(list); // not saved: off this boot only
    set_give(NULL);
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
    } else if (s_boot.app[0] && s_boot.in_callback) {
        console_reply("app background: " CB_VERDICT, s_boot.app, s_boot.named_crashes, SAFE_MODE_WINDOW,
                      SAFE_MODE_CB_LIMIT);
    } else if (s_boot.app[0]) {
        console_reply("app background: this boot follows a crash of %s, %u of %d in %d boots", s_boot.app,
                      s_boot.app_crashes, SAFE_MODE_LIMIT, SAFE_MODE_WINDOW);
    } else {
        console_reply("app background: this boot follows %s", s_boot.crash ? "a crash of no app" : "a clean end");
    }
    console_reply("app background: degraded boot %s%s", safe_mode_degraded_boot() ? "yes" : "no",
                  valid ? "" : " (the window record is not valid)");
}

static bool save_cb(char const *next, void *arg) {
    (void)arg;
    return save_list(next);
}

bool safe_mode_background_listed(char const *id) {
    portENTER_CRITICAL(&s_mux);
    bool listed = safe_mode_list_has(s_off, id);
    portEXIT_CRITICAL(&s_mux);
    return listed;
}

static void list_enter(void *arg) {
    (void)arg;
    portENTER_CRITICAL(&s_mux);
}

static void list_leave(void *arg) {
    (void)arg;
    portEXIT_CRITICAL(&s_mux);
}

// The list in RAM only takes the change once it is saved: a switch that did not save is not shown as set (#233)
bool safe_mode_background_set(char const *id, bool allowed) {
    static safe_mode_sync_t const sync = {set_take, set_give, list_enter, list_leave, NULL};
    if (!safe_mode_list_set_shared(&sync, s_off, sizeof(s_off), id, !allowed, save_cb, NULL)) {
        return false;
    }
    if (allowed) {
        portENTER_CRITICAL(&s_mux);
        safe_mode_forget(&s_rec, id); // one more crash must not switch it off again at once
        portEXIT_CRITICAL(&s_mux);
    }
    return true;
}

unsigned safe_mode_app_crashes(char const *id) {
    portENTER_CRITICAL(&s_mux);
    unsigned n = safe_mode_crashes(&s_rec, id);
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static void background_on(char const *id) {
    bool was   = safe_mode_background_listed(id);
    bool saved = safe_mode_background_set(id, true);
    if (!was) {
        console_reply("app background: %s was not off", id);
    } else if (saved) {
        console_reply("app background: %s on again from the next boot, its crashes forgotten", id);
    } else {
        console_reply("app background: %s on, but not saved: it is off again at the next boot", id);
    }
}

#ifdef CONFIG_LICHEN_DEV_COMMANDS
static void crash_in_timer(lv_timer_t *t) {
    (void)t;
    abort();
}

// #427: a timer the app's context made, so it runs through the #349 trampoline on the LVGL task and the panic hook
// sees the callback mark, as a faulty app timer would
static void crash_in_callback(lichen_app_t const *app, lichen_ctx_t ctx) {
    if (!lichen_sys_ui_lock(1000)) {
        console_reply("app crash: the UI is busy, nothing done");
        return;
    }
    lichen_ctx_t previous = lichen_ctx_enter(ctx);
    lv_timer_t  *t        = lichen_ui_timer_create(crash_in_timer, 200, NULL);
    lichen_ctx_leave(previous);
    lichen_sys_ui_unlock();
    console_reply(t ? "app crash: %s crashes in a screen callback in 0.2 s, the badge restarts"
                    : "app crash: %s: no timer (no memory), nothing done",
                  app->id ? app->id : app->name);
}

// The faulting task runs as the app: what the panic hook reads is the real attribution path, not a shortcut.
// "<id> cb": the fault is in one of the app's LVGL timer callbacks instead (#427)
static void crash_as(char const *args) {
    // developer mode: its row in os_cmd_rules.h, checked before "fw app" runs (#464)
    char        name[SAFE_MODE_ID_CAP];
    char const *sp  = strchr(args, ' ');
    size_t      len = sp ? (size_t)(sp - args) : strlen(args);
    bool        cb  = sp && strcmp(sp, " cb") == 0;
    if ((sp && !cb) || len >= sizeof(name)) {
        console_reply("app crash: usage: app crash <id> [cb]");
        return;
    }
    memcpy(name, args, len);
    name[len]               = '\0';
    lichen_app_t const *app = lichen_app_by_id(name); // ids only: lichen_app_by_name is the shell's (layers.conf)
    lichen_ctx_t        ctx = app ? lichen_sys_task_ctx(app) : LICHEN_CTX_NONE;
    if (!app || ctx == LICHEN_CTX_NONE) {
        console_reply("app crash: no app %s", name);
        return;
    }
    if (cb) {
        crash_in_callback(app, ctx);
        return;
    }
    console_reply("app crash: %s crashes now (abort as the app), the badge restarts", app->id ? app->id : app->name);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    lichen_ctx_enter(ctx);
    abort();
}
#define USAGE_DEV " | crash <id> [cb] (developer mode: a panic as that app, or in its timer callback)"
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
    console_reply("usage: app ctx | caps | life | background [<id> on]" USAGE_DEV);
}

// ---- the boot step -------------------------------------------------------------------------------------------------

static void log_verdict(void) {
    if (!s_boot.counted) {
        ESP_LOGI(TAG, "this boot is the PSRAM re-arm restart after a crash: not counted again");
    } else if (s_boot.app[0] && s_boot.in_callback) {
        ESP_LOGW(TAG, CB_VERDICT, s_boot.app, s_boot.named_crashes, SAFE_MODE_WINDOW, SAFE_MODE_CB_LIMIT);
    } else if (s_boot.app[0]) {
        ESP_LOGW(TAG, "the crash is attributed to %s: %u of %d in the last %d boots", s_boot.app, s_boot.app_crashes,
                 SAFE_MODE_LIMIT, SAFE_MODE_WINDOW);
    } else if (s_boot.crash) {
        ESP_LOGI(TAG, "the crash is attributed to no app (an OS task, an ISR, a reset without a panic)");
    }
}

void safe_mode_boot_step(bool crashed) {
    char list[SAFE_MODE_LIST_MAX];
    s_set_lock = xSemaphoreCreateMutexStatic(&s_set_buf); // static: cannot fail
    lichen_sys_panic_probe(); // the panic hook blames nobody until its view of the task checked out
    settings_get_str(LIST_KEY, list, sizeof(list), "");
    portENTER_CRITICAL(&s_mux);
    memcpy(s_off, list, sizeof(s_off));
    s_boot = safe_mode_boot(&s_rec, crashed);
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_boot_done, true);
    log_verdict(); // the switch-off itself waits for start_apps: only then is it known whether the app has a background
    if (list[0]) {
        ESP_LOGW(TAG, "background part off (safe mode) for %s: fw app background <id> on", list);
    }
    lichen_sys_app_console_more(cmd_more, "app ctx | caps | life | background [<id> on]" USAGE_DEV
                                          " (contexts; capabilities, #229; lifecycle, #231; the apps whose background "
                                          "is off after crashes, #232)",
                                APP_RULES);
}
