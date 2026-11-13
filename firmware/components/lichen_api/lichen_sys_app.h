#pragma once

// The app context's system surface (#230 E1a): switching the current app and resolving a context, for the shell, the
// UI service, the task service and main, never for an app (tools/layers.conf forbids these symbols in L6). Apps have
// lichen_ctx_self (lichen_app.h).
//
// The current app is in FreeRTOS thread-local slot 1 of the running task (slot 0 is IDF's pthread storage;
// CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2). None of these take a lock, the LVGL lock included, and none block.

#include "gate_app.h"
#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Makes ctx the current app of the calling task and returns the one before it, for lichen_ctx_leave. Wrap every call
// into an app's hook: prev = lichen_ctx_enter(lichen_sys_app_ctx_at(i)); app->hook(...); lichen_ctx_leave(prev). They
// nest (a hook that opens another app's page). A ctx that is not valid is refused (logged, counted) and the OS is
// entered instead. Any task; in an ISR nothing changes.
lichen_ctx_t lichen_ctx_enter(lichen_ctx_t ctx);
// Puts back what lichen_ctx_enter returned. Any task (the one that entered).
void lichen_ctx_leave(lichen_ctx_t previous);

// The registered apps (#66), in registration order: home, Settings and the console list them. The OS's view of the
// table (#340): tools/layers.conf keeps these from the apps, which know only their own context. The table is filled
// at boot before anything reads it and never changes afterwards. Any task.
size_t lichen_app_count(void);
// The app at index, NULL past the end. Any task.
lichen_app_t const *lichen_app_at(size_t index);
// The context lichen_register_app gave the app at index (lichen_app_at's), LICHEN_CTX_NONE past the end: what the OS
// enters around that app's hooks. Any task.
lichen_ctx_t lichen_sys_app_ctx_at(size_t index);
// The registered app with that id, NULL when there is none. Any task.
lichen_app_t const *lichen_app_by_id(char const *id);
// The same, and an app's alias ("mesh") matches too: for what a person types. *ctx (may be NULL) gets the app's
// context, LICHEN_CTX_NONE when there is no such app. Any task.
lichen_app_t const *lichen_app_by_name(char const *name, lichen_ctx_t *ctx);
// The context a task lichen_task_start makes runs for: app's, or for app NULL the calling task's own (inherited); the
// OS's for a struct that never registered. Any task.
lichen_ctx_t lichen_sys_task_ctx(lichen_app_t const *app);
// The OS's own context (drawn at boot like the apps'). Any task.
lichen_ctx_t lichen_sys_ctx_os(void);
// The owner of ctx: true and *app (NULL for the OS) when ctx is valid; false when it is stale or made up, which is
// logged (rate-limited) and counted under what (the gate call's name), never an abort. Any task or ISR.
bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what);

// What a gate call that acts for an app (settings, files, the crash log's marker, badges, notices) does for ctx, by
// gate_app.c's rules (#230 E1c): the kind and, for an app, its id and name; a refused ctx is logged (rate-limited)
// and counted under what, never an abort. Any task or ISR.
gate_app_kind_t lichen_sys_ctx_app(lichen_ctx_t ctx, char const *what, char const **id, char const **name);
// As lichen_sys_ctx_app for the app-facing calls with powers (notices, crash log rewrites, the SD batch): the OS's own
// ctx is refused too (counted), since an app can reach it through lichen_ctx_self() outside its hooks
gate_app_kind_t lichen_sys_ctx_caller(lichen_ctx_t ctx, char const *what, char const **id, char const **name);
// The notification source an app-facing call raises under: its app's id; NULL (dropped) for a refused ctx, the OS's or
// an app without an id
char const *lichen_sys_ctx_source(lichen_ctx_t ctx, char const *what);
// The settings namespace "a.<id>" of ctx's app in ns (at least 16): GATE_APP_ID when made, else why not (*name is set
// for a ctx that was not refused)
gate_app_kind_t lichen_sys_ctx_settings_ns(lichen_ctx_t ctx, char const *what, char *ns, size_t cap,
                                           char const **name);

// main.c, once (the main task, not on the LVGL task): "fw app ctx" (every app's context, the refusals since boot
// and the last refused value)
void lichen_sys_app_console_register(void);

#ifdef __cplusplus
}
#endif
