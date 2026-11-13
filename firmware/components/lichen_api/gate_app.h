#pragma once

// The gate's check of a caller's context for the calls that act for an app (#230 E1c): settings, files, the crash
// log's seen marker, the tile badge and the notification source. Resolve the ctx first, act only when it names what the
// call needs, and say no otherwise, before any of the caller's data (a key, a path, a buffer) is looked at. Pure (no
// ESP-IDF): the context table comes in through gate_app_env_t, host_tests/gate_app_test.c runs the decisions
// lichen_sys_ctx_app (lichen_app.c) and the callers make.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // True and the app's *id and *name (both NULL for the OS) when ctx is valid. False when it is stale, made up or 0:
    // the implementation logs (rate-limited) and counts it under what. Never blocks, takes no lock.
    bool (*resolve)(uint32_t ctx, char const **id, char const **name, char const *what);
    uint32_t (*os)(void); // the OS's context
    // Counts and logs (rate-limited) a refusal of a ctx that is valid but not allowed for the call: the OS's on an
    // app-facing call. Never blocks.
    void (*refuse)(uint32_t ctx, char const *what);
} gate_app_env_t;

typedef enum {
    GATE_APP_REFUSED, // stale, made up or 0: already logged and counted, the call does nothing
    GATE_APP_OS,      // the OS's own: it has no settings namespace, directory or tile
    GATE_APP_NO_ID,   // an app that registered without an id: the same, and the caller says so in its log
    GATE_APP_ID,      // an app with an id: *id is it
} gate_app_kind_t;

// What ctx stands for. *id and *name are set for GATE_APP_ID and GATE_APP_NO_ID (name), untouched otherwise. An id the
// OS keeps for itself is no id: "lichen" (the OS's notification source), "test" (the console's) and the drive folders
// of fs_path.h (c6, crash, lichen) give GATE_APP_NO_ID, so an app cannot post or file under the OS's names.
gate_app_kind_t gate_app_id(gate_app_env_t const *g, uint32_t ctx, char const *what, char const **id,
                            char const **name);

// For the app-facing calls with powers (notices, the crash log's rewrites, the SD batch): as gate_app_id, and the
// OS's own ctx is refused too (counted like a bad one, g->refuse), because an app reaches it through lichen_ctx_self()
// outside its hooks (an esp_timer or LVGL callback). The OS does these through lichen_sys_* calls instead.
gate_app_kind_t gate_app_caller(gate_app_env_t const *g, uint32_t ctx, char const *what, char const **id,
                                char const **name);

#define GATE_OS_SOURCE "lichen" // the notification source of the OS's own notices (lichen_sys_notify.h)

// The notification source an app-facing call raises under: its app's id (gate_app_caller). NULL for a refused ctx, the
// OS's or an app without an id: the notice is dropped, the source is never taken from the caller.
char const *gate_app_source(gate_app_env_t const *g, uint32_t ctx, char const *what);

// The settings namespace "a.<id>" (app_ns_name) behind ctx, in ns (cap at least 16). GATE_APP_ID when it was made;
// otherwise the reason (ns untouched), with *name set for any ctx that was not refused, for the caller's log.
gate_app_kind_t gate_app_settings_ns(gate_app_env_t const *g, uint32_t ctx, char const *what, char *ns, size_t cap,
                                     char const **name);

// Why an app with id and alias (alias may be NULL) may not join the apps already registered (#340), whose ids and
// aliases are the n strings of taken (NULL entries skipped): no id (NULL or empty: the shell lists apps by id), an id
// or alias the OS keeps or that is no app id (gate_app_id's rules), an alias equal to its own id, or a name another
// app has as its id or alias, so two apps never share a namespace,
// a directory, a notification source or a console name. NULL when it may.
char const *gate_app_names_refusal(char const *id, char const *alias, char const *const *taken, size_t n);

// An SD batch is ended by the ctx that began it: held is the ctx recorded at begin (0 = no batch), ctx the caller's
bool gate_batch_may_end(uint32_t held, uint32_t ctx);

#ifdef __cplusplus
}
#endif
