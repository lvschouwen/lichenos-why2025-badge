#pragma once

// The run-time capability check (#229): a gated call made for an app that did not declare the capability
// (lichen_app_t.caps, CAP_* in lichen_app.h) is refused, counted and logged (rate-limited). An accident guard, not a
// security boundary. Pure (no ESP-IDF), host-tested in host_tests/caps_test.c; lichen_app.c runs it over the real table.
//
// Lock-free and heap-free, safe from any task: one table lookup and a compare, atomics for the counters. Nothing on an
// ISR or panic path calls it (an ISR's context is the OS's, which always passes).

#include "app_ctx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t refused;                 // since boot
    uint32_t per_slot[APP_CTX_SLOTS]; // refusals per app slot (fw app caps)
    uint32_t last_need;               // the capabilities the last refused call needed
    uint32_t unlogged;                // refusals the rate limit kept out of the log since the last logged one
    uint32_t logged_ms;               // when the last refusal was logged
    uint32_t logged_any;              // 0 until the first was logged
} app_caps_refusals_t;

// A table owner's capabilities (lichen_app_t.caps); owner is never NULL
typedef uint32_t (*app_caps_of_fn)(void const *owner);

// True when have holds every bit of need; CAP_SYSTEM (the shell's own apps) holds all of them
bool app_caps_allows(uint32_t have, uint32_t need);

// May a call that needs need be made for ctx? True for need 0, the OS's context, and a ctx that is not valid (the
// call's own ctx check refuses that one, counted there once); otherwise app_caps_allows of the app's caps. A refusal
// is counted in r (total, the slot, last_need).
bool app_caps_check(app_caps_refusals_t *r, app_ctx_table_t const *t, uint32_t ctx, uint32_t need,
                    app_caps_of_fn caps_of);

// After a refusal: true when it may be logged at now_ms (the first one, then once per APP_CTX_LOG_GAP_MS); *skipped
// gets how many were not logged since the last logged one. False counts this one as not logged.
bool app_caps_log_due(app_caps_refusals_t *r, uint32_t now_ms, uint32_t *skipped);

// The name of capability bit (CAP_<name> without the prefix), NULL past CAP_COUNT
char const *app_caps_name(unsigned bit);
// The names of caps joined by spaces in out ("-" for none); returns the length written (cut to cap - 1)
size_t app_caps_format(uint32_t caps, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
