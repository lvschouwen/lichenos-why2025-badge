#pragma once

// The app context's system surface (#230 E1a): switching the current app and resolving a context, for the shell, the
// UI service, the task service and main, never for an app (tools/layers.conf forbids these symbols in L6). Apps have
// lichen_ctx_self (lichen_app.h).
//
// The current app is in FreeRTOS thread-local slot 1 of the running task (slot 0 is IDF's pthread storage;
// CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2). None of these take a lock, the LVGL lock included, and none block.

#include "lichen_app.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Makes ctx the current app of the calling task and returns the one before it, for lichen_ctx_leave. Wrap every call
// into an app's hook: prev = lichen_ctx_enter(lichen_sys_app_ctx(app)); app->hook(...); lichen_ctx_leave(prev). They
// nest (a hook that opens another app's page). A ctx that is not valid is refused (logged, counted) and the OS is
// entered instead. Any task; in an ISR nothing changes.
lichen_ctx_t lichen_ctx_enter(lichen_ctx_t ctx);
// Puts back what lichen_ctx_enter returned. Any task (the one that entered).
void lichen_ctx_leave(lichen_ctx_t previous);

// The compat shim until #230 E1e: the context of a registered app's struct, the OS's for NULL, LICHEN_CTX_NONE for
// a struct that never registered (logged once per struct). Any task.
lichen_ctx_t lichen_sys_app_ctx(lichen_app_t const *app);
// The context a task lichen_task_start makes runs for: app's, or for app NULL the calling task's own (inherited); the
// OS's for a struct that never registered. Any task.
lichen_ctx_t lichen_sys_task_ctx(lichen_app_t const *app);
// The OS's own context (drawn at boot like the apps'). Any task.
lichen_ctx_t lichen_sys_ctx_os(void);
// The owner of ctx: true and *app (NULL for the OS) when ctx is valid; false when it is stale or made up, which is
// logged (rate-limited) and counted under what (the gate call's name), never an abort. Any task or ISR.
bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what);

// main.c, once (the main task, not on the LVGL task): "fw app ctx" (every app's context, the refusals since boot
// and the last refused value)
void lichen_sys_app_console_register(void);

#ifdef __cplusplus
}
#endif
