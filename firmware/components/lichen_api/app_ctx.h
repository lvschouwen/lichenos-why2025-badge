#pragma once

// The app context table (#230 E1a): the handle behind lichen_ctx_t. A ctx is 8 bits of slot and 24 bits of
// generation, not a pointer, so a gate call can check it instead of trusting a lichen_app_t pointer the caller made
// up. Slot 0 is the OS, slots 1.. the registered apps in registration order (lichen_app_at's index + 1). Every slot
// draws its generation at random when it is taken, so knowing one ctx says nothing about another; generation 0 is
// never used, so ctx 0 (APP_CTX_NONE) is never valid. Pure (no ESP-IDF), host-tested in host_tests/app_ctx_test.c.
//
// Not locked: the slots are written while the apps register at boot, before anything reads them from another task;
// the refusal counters are atomic.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_CTX_SLOTS      25u // the OS + LICHEN_MAX_APPS (lichen_app.c asserts it)
#define APP_CTX_NONE       0u
#define APP_CTX_SLOT_BITS  8u
#define APP_CTX_GEN_MASK   0xffffffu
#define APP_CTX_LOG_GAP_MS 10000u // a refusal is logged at most once per gap; the rest are only counted

typedef struct {
    void const *owner[APP_CTX_SLOTS]; // [0] the OS (NULL), [1..apps] the registered apps
    uint32_t    gen[APP_CTX_SLOTS];   // 0 = slot free (or, for [0], the table not set up yet)
    uint8_t     apps;                 // app slots taken
    uint32_t    refused;              // ctx values refused since boot
    uint32_t    last_refused;         // the last one
    uint32_t    unlogged;             // refusals the rate limit kept out of the log since the last logged one
    uint32_t    logged_ms;            // when the last refusal was logged
    bool        logged_any;
} app_ctx_table_t;

// Sets up the OS slot with a generation drawn from draw (any 32-bit value; the table keeps 24 bits, never 0)
void app_ctx_init(app_ctx_table_t *t, uint32_t draw);
bool app_ctx_ready(app_ctx_table_t const *t);

// Takes the next slot for owner (not NULL) with a generation from draw. Returns its ctx, APP_CTX_NONE when the table
// is full or not set up.
uint32_t app_ctx_add(app_ctx_table_t *t, void const *owner, uint32_t draw);

uint32_t    app_ctx_os(app_ctx_table_t const *t); // APP_CTX_NONE before app_ctx_init
size_t      app_ctx_apps(app_ctx_table_t const *t);
void const *app_ctx_app_at(app_ctx_table_t const *t, size_t index); // NULL past the end
uint32_t    app_ctx_at(app_ctx_table_t const *t, size_t index);     // the app's ctx, APP_CTX_NONE past the end
// The ctx of owner: the OS's for NULL, APP_CTX_NONE for a pointer no app registered (the compat shim's lookup)
uint32_t    app_ctx_of(app_ctx_table_t const *t, void const *owner);

// A ctx of a taken slot with that slot's generation. Counts nothing.
bool app_ctx_valid(app_ctx_table_t const *t, uint32_t ctx);
// As app_ctx_valid, and sets *owner (NULL for the OS) when valid. A refused ctx is counted (refused, last_refused).
bool app_ctx_resolve(app_ctx_table_t *t, uint32_t ctx, void const **owner);
// Counts ctx as refused (app_ctx_resolve does it itself)
void app_ctx_count_refusal(app_ctx_table_t *t, uint32_t ctx);
// After a refusal: true when it may be logged at now_ms (the first one, then once per APP_CTX_LOG_GAP_MS); *skipped
// gets how many were not logged since the last logged one. False counts this one as not logged.
bool app_ctx_log_due(app_ctx_table_t *t, uint32_t now_ms, uint32_t *skipped);
