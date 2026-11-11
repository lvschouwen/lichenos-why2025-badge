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
} gate_app_env_t;

typedef enum {
    GATE_APP_REFUSED, // stale, made up or 0: already logged and counted, the call does nothing
    GATE_APP_OS,      // the OS's own: it has no settings namespace, directory or tile
    GATE_APP_NO_ID,   // an app that registered without an id: the same, and the caller says so in its log
    GATE_APP_ID,      // an app with an id: *id is it
} gate_app_kind_t;

// What ctx stands for. *id and *name are set for GATE_APP_ID and GATE_APP_NO_ID (name), untouched otherwise.
gate_app_kind_t gate_app_id(gate_app_env_t const *g, uint32_t ctx, char const *what, char const **id,
                            char const **name);

#define GATE_OS_SOURCE "lichen" // the notification source of the OS's own notices

// The notification source ctx raises under: its app's id, "lichen" for the OS. NULL for a refused ctx or an app
// without an id: the notice is dropped, the source is never taken from the caller.
char const *gate_app_source(gate_app_env_t const *g, uint32_t ctx, char const *what);

// The settings namespace "a.<id>" (app_ns_name) behind ctx, in ns (cap at least 16). False when ctx is refused, is
// the OS's, has no usable id: ns is then untouched.
bool gate_app_settings_ns(gate_app_env_t const *g, uint32_t ctx, char const *what, char *ns, size_t cap);

#ifdef __cplusplus
}
#endif
