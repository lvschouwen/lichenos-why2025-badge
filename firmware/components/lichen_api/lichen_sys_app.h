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

// The capability check of a gated call (#229, app_caps.h): true when ctx's app declared every CAP_* bit of need (or is
// a SYSTEM app, or ctx is the OS's or not valid: the call's own ctx check refuses that). False is counted and logged at
// most once per 10 s under what ("fw app caps"). Lock-free, heap-free, any task; never on an ISR or panic path.
bool lichen_sys_caps_ok(lichen_ctx_t ctx, uint32_t need, char const *what);
// lichen_sys_caps_ok for the context the caller runs for (lichen_ctx_self), for the gate calls that take no ctx
bool lichen_sys_caps_self_ok(uint32_t need, char const *what);

// main.c, once (the main task, not on the LVGL task): "fw app ctx" (every app's context, the refusals since boot
// and the last refused value) and "fw app caps" (every app's capabilities, budgets and capability refusals, #229)
void lichen_sys_app_console_register(void);
// More "fw app" subcommands (#232: safe mode's), set once before lichen_sys_app_console_register: every line that is
// not "ctx" goes to more, and usage (kept by pointer) is the command's help. NULL: only "ctx". Main task, at boot.
void lichen_sys_app_console_more(void (*more)(char const *args), char const *usage);

// For the panic handler (#232): the id of the app the task running on this core runs for (its entered context, as
// lichen_ctx_self would answer), NULL for the OS, an ISR, a context that is not valid, an app without an id, or before
// lichen_sys_panic_probe said yes. In internal RAM and reading internal RAM only (copies of the ids, the table, the
// TCB when it is in internal RAM): no lock, no log, no flash. The panic handler.
#define LICHEN_PANIC_ID_CAP 14 // an app id with its NUL
char const *lichen_sys_panic_app_id(void);
// The callback trampoline (lichen_ui_cb.c, #349): entering true with the context it is about to enter, before it
// enters it, returns the context marked before; false, after it left, with that returned value (put back when they
// nest; the return is 0). The LVGL task only (it holds the LVGL lock); no lock, no heap.
uint32_t lichen_sys_ui_cb_mark(bool entering, lichen_ctx_t ctx);
// For the panic handler: the task running on this core is inside an app's LVGL callback (lichen_sys_ui_cb_mark) and
// still runs as the context that callback entered (#427: not another app's, as when Settings runs an app's hook), so
// the shell, LVGL or a PSRAM fault it reached is named after the app and counts toward the callback limit, not as a
// strike (safe_mode_policy.h). In internal RAM, reads the mark (internal .bss), the current TCB pointer and its
// thread-local slot when the TCB is in internal RAM: no lock, no log, no flash. False before lichen_sys_panic_probe
// said yes. The panic handler.
bool lichen_sys_panic_in_ui_cb(void);
// Once at boot, from a task (main, safe_mode_boot_step): checks that the panic handler's view of the current task's
// context slot is the real one; false (logged) leaves attribution off. Not from an ISR.
bool lichen_sys_panic_probe(void);

#ifdef __cplusplus
}
#endif
