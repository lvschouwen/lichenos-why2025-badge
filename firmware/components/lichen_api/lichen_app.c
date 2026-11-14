// The app table (#66) behind lichen_app.h: apps register before shell_home(), home and Settings list them. Since #230
// each app has a context (app_ctx.c) and the current one lives in the task's thread-local slot (lichen_sys_app.h).
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"

#include "app_caps.h"
#include "app_ctx.h"
#include "app_ns.h"
#include "app_secret.h"
#include "ui_cb.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_random.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "shell";

// Slot 0 is IDF's pthread thread-local storage (pthread_local_storage.c); 1 is Lichen's current app
#define CTX_TLS_INDEX 1
// The Kconfig value, not configNUM_THREAD_LOCAL_STORAGE_POINTERS: that one is doubled when deletion callbacks are on,
// and index 1 must be a pointer slot, never a callback's
_Static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > CTX_TLS_INDEX,
               "CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS must be 2 or more (firmware/sdkconfig.defaults)");
_Static_assert(APP_CTX_SLOTS == LICHEN_MAX_APPS + 1, "app_ctx.h's table is the OS plus LICHEN_MAX_APPS apps");

// Internal .bss (~0.3 KB): read from any task and ISR, so never PSRAM
static app_ctx_table_t s_table;
static portMUX_TYPE    s_init_mux = portMUX_INITIALIZER_UNLOCKED;

// The OS slot is set up on first use, whichever comes first (a registration, or a task asking for its context)
static app_ctx_table_t *table(void) {
    if (!app_ctx_ready(&s_table)) {
        uint32_t draw = lichen_random_u32(); // outside the spinlock
        portENTER_CRITICAL_SAFE(&s_init_mux);
        if (!app_ctx_ready(&s_table)) {
            app_ctx_init(&s_table, draw);
        }
        portEXIT_CRITICAL_SAFE(&s_init_mux);
    }
    return &s_table;
}

// An app's backup keys join the SD backup as one group, and only when every key is in the app's own namespace (a group
// naming badge_fw or another app's would let an app export or overwrite what is not its). Feature lost: logged at E (#113).
static void register_backup_keys(lichen_app_t const *app) {
    for (size_t i = 0; i < app->backup_key_count; i++) {
        if (!app_ns_owns(app->id, app->backup_keys[i].ns)) {
            ESP_LOGE(TAG, "app %s: backup key %s is not in its namespace: no backup for the app", app->name,
                     app->backup_keys[i].key);
            return;
        }
    }
    if (!app_secret_keys_plain(app->secret_prefix, app->backup_keys, app->backup_key_count)) {
        ESP_LOGE(TAG, "app %s: a backup key is one of its secrets (#288): no backup for the app", app->name);
        return;
    }
    if (!sb_add_app_keys(app->backup_keys, app->backup_key_count)) {
        ESP_LOGE(TAG, "app %s: its backup keys do not fit the allow-list (SB_KEYS_MAX): no backup for the app", app->name);
    }
}

static void app_names(void const *owner, char const **id, char const **alias) {
    lichen_app_t const *app = owner;
    *id                     = app->id;
    *alias                  = app->alias;
}

static void panic_id_keep(lichen_ctx_t ctx, char const *id); // the panic handler's copy of the id (below)

lichen_ctx_t lichen_register_app(lichen_app_t const *app) {
    app_ctx_table_t *t = table();
    if (!app) {
        ESP_LOGE(TAG, "no app to register");
        return LICHEN_CTX_NONE;
    }
    // A secret prefix the OS cannot enforce would leave the app's secrets as plain settings (#288): not registered
    if (!app_secret_prefix_ok(app->secret_prefix)) {
        ESP_LOGE(TAG, "app %s not registered: its secret prefix is not 1..%d of a-z 0-9 _", app->name ? app->name : "?",
                 APP_SECRET_PREFIX_MAX);
        return LICHEN_CTX_NONE;
    }
    // Two apps sharing an id would share a settings namespace, a directory and a notification source (#340): the
    // second is skipped, never an abort, so the badge still boots with the others
    char const  *refusal = NULL;
    lichen_ctx_t ctx     = app_ctx_add_app(t, app, lichen_random_u32(), app_names, &refusal);
    if (refusal) {
        ESP_LOGE(TAG, "app %s (id %s, alias %s) not registered: %s", app->name ? app->name : "?",
                 app->id ? app->id : "-", app->alias ? app->alias : "-", refusal);
    } else if (ctx == LICHEN_CTX_NONE) {
        ESP_LOGE(TAG, "no room for app %s: LICHEN_MAX_APPS (%d) apps are registered already", app->name,
                 LICHEN_MAX_APPS);
    } else {
        panic_id_keep(ctx, app->id);
        if (app->backup_keys) {
            register_backup_keys(app);
        }
    }
    return ctx;
}

size_t lichen_app_count(void) {
    return app_ctx_apps(&s_table);
}

lichen_app_t const *lichen_app_at(size_t index) {
    return app_ctx_app_at(&s_table, index);
}

lichen_ctx_t lichen_sys_app_ctx_at(size_t index) {
    return app_ctx_at(&s_table, index);
}

// The index of the app named name by its id (alias false) or its alias, -1 for none
static int find_app(char const *name, bool alias) {
    for (size_t i = 0; name && i < app_ctx_apps(&s_table); i++) {
        lichen_app_t const *app  = app_ctx_app_at(&s_table, i);
        char const         *mine = alias ? app->alias : app->id;
        if (mine && strcmp(mine, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

lichen_app_t const *lichen_app_by_id(char const *id) {
    int i = find_app(id, false);
    return i >= 0 ? app_ctx_app_at(&s_table, (size_t)i) : NULL;
}

lichen_app_t const *lichen_app_by_name(char const *name, lichen_ctx_t *ctx) {
    int i = find_app(name, false);
    if (i < 0) {
        i = find_app(name, true);
    }
    if (ctx) {
        *ctx = i >= 0 ? app_ctx_at(&s_table, (size_t)i) : LICHEN_CTX_NONE;
    }
    return i >= 0 ? app_ctx_app_at(&s_table, (size_t)i) : NULL;
}

// ---- the current app -----------------------------------------------------------------------------------------------

static uint32_t tls_get(void) {
    return (uint32_t)(uintptr_t)pvTaskGetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX);
}

static void tls_set(uint32_t ctx) {
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)ctx);
}

static bool in_isr(void) {
    return xPortInIsrContext();
}

static app_ctx_tls_t const s_tls = {.get = tls_get, .set = tls_set, .in_isr = in_isr};

lichen_ctx_t lichen_ctx_self(void) {
    return app_ctx_self(table(), &s_tls);
}

// ---- the panic handler's view (#232) ---------------------------------------------------------------------------
// The panic hook runs with XIP from PSRAM: a flash function or rodata is fetched through the PSRAM cache, which may
// be what faulted. So lichen_sys_panic_app_id is IRAM_ATTR and reads internal RAM only: the table (internal .bss), a
// copy of each app's id made at registration, and the current task's TCB through FreeRTOS's public layout mirror
// (StaticTask_t, whose pvDummy15 is the TCB's thread-local slots). lichen_sys_panic_probe checks that mirror against
// the real call once at boot; until it said yes the hook blames nobody.
extern void *volatile pxCurrentTCBs[]; // FreeRTOS's current task per core (internal .bss)

#define PANIC_PROBE 0x5AFE2320u

static char s_panic_ids[APP_CTX_SLOTS][LICHEN_PANIC_ID_CAP]; // internal .bss, written at registration
static bool s_panic_mirror_ok;

static void panic_id_keep(lichen_ctx_t ctx, char const *id) {
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    if (slot == 0 || slot >= APP_CTX_SLOTS || !id) {
        return;
    }
    strlcpy(s_panic_ids[slot], id, sizeof(s_panic_ids[slot])); // a longer id never registers (app_ns.h)
}

static void *IRAM_ATTR tcb_slot(void const *tcb) {
    return ((StaticTask_t const *)tcb)->pvDummy15[CTX_TLS_INDEX];
}

bool lichen_sys_panic_probe(void) {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    void        *saved = pvTaskGetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX);
    portENTER_CRITICAL(&mux); // no switch, no migration between the reads
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)PANIC_PROBE);
    void const *tcb = pxCurrentTCBs[esp_cpu_get_core_id()];
    bool        ok  = tcb == (void const *)xTaskGetCurrentTaskHandle() && esp_ptr_in_dram(tcb) &&
             (uintptr_t)tcb_slot(tcb) == PANIC_PROBE;
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, saved);
    portEXIT_CRITICAL(&mux);
    s_panic_mirror_ok = ok;
    if (!ok) {
        ESP_LOGE(TAG, "crash attribution off: the task layout is not the one this build expects (#232)");
    }
    return ok;
}

// The table's rules inline (app_ctx_valid lives in flash): slot 1..apps, generation not 0 and the slot's own
char const *IRAM_ATTR lichen_sys_panic_app_id(void) {
    int core = esp_cpu_get_core_id();
    if (!s_panic_mirror_ok || core < 0 || core >= portNUM_PROCESSORS || xPortInterruptedFromISRContext()) {
        return NULL; // an ISR's fault is not the interrupted task's app's
    }
    void const *tcb = pxCurrentTCBs[core];
    if (!esp_ptr_in_dram(tcb) || ((uintptr_t)tcb & 3u)) {
        return NULL; // a TCB in PSRAM (ALLOW_EXT_MEM) or a wild pointer is never read here
    }
    uint32_t ctx  = (uint32_t)(uintptr_t)tcb_slot(tcb);
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    uint32_t gen  = ctx >> APP_CTX_SLOT_BITS;
    if (slot == 0 || slot >= APP_CTX_SLOTS || slot > s_table.apps || gen == 0 || s_table.gen[slot] != gen) {
        return NULL;
    }
    return s_panic_ids[slot][0] ? s_panic_ids[slot] : NULL;
}

// The task inside an app's LVGL callback and the context it entered (#349, #427; ui_cb.h has the store order), for
// the panic hook: internal .bss, written only by the LVGL task under the LVGL lock
static ui_cb_mark_t s_ui_cb_mark;

uint32_t lichen_sys_ui_cb_mark(bool entering, lichen_ctx_t ctx) {
    if (entering) {
        return ui_cb_mark_enter(&s_ui_cb_mark, xTaskGetCurrentTaskHandle(), ctx);
    }
    ui_cb_mark_leave(&s_ui_cb_mark, ctx);
    return 0;
}

// The same reads as lichen_sys_panic_app_id: the TCB only when it is in internal RAM and the mirror checked out
bool IRAM_ATTR lichen_sys_panic_in_ui_cb(void) {
    int core = esp_cpu_get_core_id();
    if (!s_panic_mirror_ok || core < 0 || core >= portNUM_PROCESSORS || xPortInterruptedFromISRContext()) {
        return false;
    }
    void const *tcb = pxCurrentTCBs[core];
    if (!esp_ptr_in_dram(tcb) || ((uintptr_t)tcb & 3u)) {
        return false;
    }
    return ui_cb_mark_holds(&s_ui_cb_mark, tcb, (uint32_t)(uintptr_t)tcb_slot(tcb));
}

static char const *app_name(void const *owner) {
    lichen_app_t const *app = owner;
    return app ? (app->id ? app->id : app->name) : "the OS";
}

// A refusal the table has counted: logged when the rate limit lets it (never from an ISR), with the app the ctx
// claims to be (its slot's) and the one the caller runs for
static void log_refusal(lichen_ctx_t ctx, char const *what, lichen_ctx_t caller_ctx) {
    uint32_t skipped = 0;
    if (in_isr() || !app_ctx_log_due(&s_table, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()), &skipped)) {
        return;
    }
    void const *claimed = app_ctx_slot_owner(&s_table, ctx);
    void const *caller  = app_ctx_slot_owner(&s_table, caller_ctx);
    char const *claims = claimed || (ctx & 0xffu) == 0 ? app_name(claimed) : "no app";
    ESP_LOGW(TAG, "%s: context 0x%08lx refused (claims %s, called for %s; stale or made up; %lu more not logged "
                  "since the last)", what ? what : "?", (unsigned long)ctx, claims, app_name(caller),
             (unsigned long)skipped);
}

lichen_ctx_t lichen_ctx_enter(lichen_ctx_t ctx) {
    bool         refused  = false;
    lichen_ctx_t previous = app_ctx_enter(table(), &s_tls, ctx, &refused);
    if (refused) {
        log_refusal(ctx, "enter", previous); // the caller's, not the OS's entered instead
    }
    return previous;
}

void lichen_ctx_leave(lichen_ctx_t previous) {
    app_ctx_leave(&s_tls, previous);
}

// An app struct that never registered is an ordering slip in the OS: said once per struct (a few remembered)
#define UNREGISTERED_SEEN 4
static void const  *s_unregistered[UNREGISTERED_SEEN];
static portMUX_TYPE s_seen_mux = portMUX_INITIALIZER_UNLOCKED;

static void warn_unregistered(lichen_app_t const *app) {
    bool first = true;
    portENTER_CRITICAL_SAFE(&s_seen_mux);
    for (size_t i = 0; i < UNREGISTERED_SEEN && first; i++) {
        if (s_unregistered[i] == app) {
            first = false;
        } else if (!s_unregistered[i]) {
            s_unregistered[i] = app;
            break;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_seen_mux);
    if (first && !in_isr()) {
        ESP_LOGW(TAG, "app %s is not registered: it runs for the OS", app_name(app));
    }
}

lichen_ctx_t lichen_sys_task_ctx(lichen_app_t const *app) {
    bool         unregistered = false;
    lichen_ctx_t ctx          = app_ctx_for_task(table(), &s_tls, app, &unregistered);
    if (unregistered) {
        warn_unregistered(app);
    }
    return ctx;
}

lichen_ctx_t lichen_sys_ctx_os(void) {
    return app_ctx_os(table());
}

bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what) {
    void const *owner = NULL;
    if (!app_ctx_resolve(table(), ctx, &owner)) {
        log_refusal(ctx, what, lichen_ctx_self());
        return false;
    }
    if (app) {
        *app = owner;
    }
    return true;
}

// The settings, file, badge and notice calls (#230 E1c): gate_app.c's decisions over the real table
static bool env_resolve(uint32_t ctx, char const **id, char const **name, char const *what) {
    lichen_app_t const *app = NULL;
    if (!lichen_sys_ctx_resolve(ctx, &app, what)) {
        return false;
    }
    *id   = app ? app->id : NULL;
    *name = app ? app->name : NULL;
    return true;
}

// A valid ctx an app-facing call may not use (the OS's): counted and logged like any refusal
static void env_refuse(uint32_t ctx, char const *what) {
    app_ctx_count_refusal(&s_table, ctx);
    log_refusal(ctx, what, lichen_ctx_self());
}

static gate_app_env_t const s_app_env = {.resolve = env_resolve, .os = lichen_sys_ctx_os, .refuse = env_refuse};

gate_app_kind_t lichen_sys_ctx_app(lichen_ctx_t ctx, char const *what, char const **id, char const **name) {
    return gate_app_id(&s_app_env, ctx, what, id, name);
}

gate_app_kind_t lichen_sys_ctx_caller(lichen_ctx_t ctx, char const *what, char const **id, char const **name) {
    return gate_app_caller(&s_app_env, ctx, what, id, name);
}

char const *lichen_sys_ctx_source(lichen_ctx_t ctx, char const *what) {
    return gate_app_source(&s_app_env, ctx, what);
}

gate_app_kind_t lichen_sys_ctx_settings_ns(lichen_ctx_t ctx, char const *what, char *ns, size_t cap,
                                           char const **name) {
    return gate_app_settings_ns(&s_app_env, ctx, what, ns, cap, name);
}

// ---- capabilities (#229) --------------------------------------------------------------------------------------------

// Internal .bss (~0.1 KB): counted from any task, so never PSRAM
static app_caps_refusals_t s_caps_refused;

static uint32_t app_caps(void const *owner) {
    return ((lichen_app_t const *)owner)->caps;
}

bool lichen_sys_caps_ok(lichen_ctx_t ctx, uint32_t need, char const *what) {
    if (app_caps_check(&s_caps_refused, table(), ctx, need, app_caps)) {
        return true;
    }
    uint32_t skipped = 0;
    if (!in_isr() && app_caps_log_due(&s_caps_refused, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()), &skipped)) {
        char needs[48];
        app_caps_format(need, needs, sizeof(needs));
        ESP_LOGW(TAG, "%s refused for app %s: it needs %s, which the app did not declare (#229; %lu more not logged "
                      "since the last)", what ? what : "?", app_name(app_ctx_slot_owner(&s_table, ctx)), needs,
                 (unsigned long)skipped);
    }
    return false;
}

bool lichen_sys_caps_self_ok(uint32_t need, char const *what) {
    return lichen_sys_caps_ok(lichen_ctx_self(), need, what);
}

// ---- fw app ctx ----------------------------------------------------------------------------------------------------

static void (*s_more)(char const *args); // lichen_sys_app_console_more, set at boot before "app" is registered
static char const *s_more_usage;

void lichen_sys_app_console_more(void (*more)(char const *args), char const *usage) {
    s_more       = more;
    s_more_usage = usage;
}

static void cmd_app_ctx(void) {
    app_ctx_table_t *t = table();
    console_reply("app ctx os 0x%08lx current 0x%08lx", (unsigned long)app_ctx_os(t), (unsigned long)lichen_ctx_self());
    for (size_t i = 0; i < app_ctx_apps(t); i++) {
        lichen_app_t const *app = app_ctx_app_at(t, i);
        console_reply("app ctx %s 0x%08lx", app->id ? app->id : app->name, (unsigned long)app_ctx_at(t, i));
    }
    console_reply("app ctx: %u apps, %lu refused since boot, last refused 0x%08lx", (unsigned)app_ctx_apps(t),
                  (unsigned long)__atomic_load_n(&t->refused, __ATOMIC_RELAXED),
                  (unsigned long)__atomic_load_n(&t->last_refused, __ATOMIC_RELAXED));
}

// #229: what each app declared (tools/layers.conf mirrors it), its budgets and the calls refused for it since boot
static void cmd_app_caps(void) {
    app_ctx_table_t *t = table();
    char             names[160];
    for (size_t i = 0; i < app_ctx_apps(t); i++) {
        lichen_app_t const *app = app_ctx_app_at(t, i);
        app_caps_format(app->caps, names, sizeof(names));
        console_reply("app caps %s: %s; budget internal %u KiB, PSRAM %u KiB; %lu refused", app->id ? app->id : app->name,
                      names, (unsigned)app->internal_ram_kib, (unsigned)app->psram_kib,
                      (unsigned long)__atomic_load_n(&s_caps_refused.per_slot[i + 1], __ATOMIC_RELAXED));
    }
    app_caps_format(__atomic_load_n(&s_caps_refused.last_need, __ATOMIC_RELAXED), names, sizeof(names));
    console_reply("app caps: %lu refused since boot (last needed %s)",
                  (unsigned long)__atomic_load_n(&s_caps_refused.refused, __ATOMIC_RELAXED), names);
}

static void cmd_app(char const *args) {
    if (strcmp(args, "ctx") == 0) {
        cmd_app_ctx();
    } else if (strcmp(args, "caps") == 0) {
        cmd_app_caps();
    } else if (strcmp(args, "life") == 0) {
        lichen_sys_life_console(); // #231
    } else if (s_more) {
        s_more(args);
    } else {
        console_reply("usage: app ctx | app caps | app life");
    }
}

void lichen_sys_app_console_register(void) {
    console_register("app", s_more_usage ? s_more_usage
                                         : "app ctx (every app's context, the refusals of stale or made-up contexts "
                                           "since boot) | app caps (each app's capabilities, budgets and refused calls) | app life",
                     cmd_app);
}
