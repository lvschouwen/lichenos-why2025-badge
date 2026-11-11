// The app table (#66) behind lichen_app.h: apps register before shell_home(), home and Settings list them. Since #230
// each app has a context (app_ctx.c) and the current one lives in the task's thread-local slot (lichen_sys_app.h).
#include "lichen_app.h"
#include "lichen_sys_app.h"

#include "app_ctx.h"
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
_Static_assert(configNUM_THREAD_LOCAL_STORAGE_POINTERS > CTX_TLS_INDEX,
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

lichen_ctx_t lichen_register_app(lichen_app_t const *app) {
    app_ctx_table_t *t   = table();
    lichen_ctx_t     ctx = app ? app_ctx_add(t, app, lichen_random_u32()) : LICHEN_CTX_NONE;
    if (ctx == LICHEN_CTX_NONE) {
        ESP_LOGE(TAG, "no room for app %s: LICHEN_MAX_APPS (%d) apps are registered already", app ? app->name : "?",
                 LICHEN_MAX_APPS);
    }
    return ctx;
}

size_t lichen_app_count(void) {
    return app_ctx_apps(&s_table);
}

lichen_app_t const *lichen_app_at(size_t index) {
    return app_ctx_app_at(&s_table, index);
}

lichen_app_t const *lichen_app_by_id(char const *id) {
    for (size_t i = 0; id && i < app_ctx_apps(&s_table); i++) {
        lichen_app_t const *app = app_ctx_app_at(&s_table, i);
        if (app->id && strcmp(app->id, id) == 0) {
            return app;
        }
    }
    return NULL;
}

// ---- the current app -----------------------------------------------------------------------------------------------

static lichen_ctx_t tls_get(void) {
    return (lichen_ctx_t)(uintptr_t)pvTaskGetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX);
}

lichen_ctx_t lichen_ctx_self(void) {
    lichen_ctx_t ctx = xPortInIsrContext() ? LICHEN_CTX_NONE : tls_get();
    return ctx != LICHEN_CTX_NONE ? ctx : app_ctx_os(table()); // a task nobody entered runs for the OS
}

// A refusal the table has counted: logged when the rate limit lets it (never from an ISR)
static void log_refusal(lichen_ctx_t ctx, char const *what) {
    uint32_t skipped = 0;
    if (!xPortInIsrContext() && app_ctx_log_due(&s_table, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()), &skipped)) {
        ESP_LOGW(TAG, "%s: context 0x%08lx refused (stale or made up; %lu more not logged since the last)",
                 what ? what : "?", (unsigned long)ctx, (unsigned long)skipped);
    }
}

lichen_ctx_t lichen_ctx_enter(lichen_ctx_t ctx) {
    if (xPortInIsrContext()) {
        return LICHEN_CTX_NONE;
    }
    lichen_ctx_t previous = tls_get();
    if (!app_ctx_resolve(table(), ctx, NULL)) {
        log_refusal(ctx, "enter");
        ctx = app_ctx_os(&s_table);
    }
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)ctx);
    return previous;
}

void lichen_ctx_leave(lichen_ctx_t previous) {
    if (!xPortInIsrContext()) {
        vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)previous);
    }
}

lichen_ctx_t lichen_sys_app_ctx(lichen_app_t const *app) {
    return app_ctx_of(table(), app);
}

lichen_ctx_t lichen_sys_ctx_os(void) {
    return app_ctx_os(table());
}

bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what) {
    void const *owner = NULL;
    if (!app_ctx_resolve(table(), ctx, &owner)) {
        log_refusal(ctx, what);
        return false;
    }
    if (app) {
        *app = owner;
    }
    return true;
}

// ---- fw app ctx ----------------------------------------------------------------------------------------------------

static void cmd_app(char const *args) {
    if (strcmp(args, "ctx") != 0) {
        console_reply("usage: app ctx");
        return;
    }
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

void lichen_sys_app_console_register(void) {
    console_register("app", "app ctx (every app's context, the refusals of stale or made-up contexts since boot)",
                     cmd_app);
}
