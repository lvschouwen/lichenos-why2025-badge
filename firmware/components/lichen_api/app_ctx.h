#pragma once

// The app context table (#230 E1a): the handle behind lichen_ctx_t. A ctx is 8 bits of slot and 24 bits of
// generation, not a pointer, so a gate call can check it instead of trusting a lichen_app_t pointer the caller made
// up. Slot 0 is the OS, slots 1..255 the registered apps. A slot stays with its app until the app is removed (#520), so
// other apps' contexts never change; a freed slot is reused by the next app with a new generation (never the old
// one). Every slot draws its generation at random when it is taken, so knowing one ctx says nothing about another;
// generation 0 is never used, so ctx 0 (APP_CTX_NONE) is never valid. Pure (no ESP-IDF), host-tested in
// host_tests/app_ctx_test.c.
//
// The apps' index (lichen_app_at) is a dense view in registration order, kept in step on remove (later apps move up
// by one); a ctx never moves. Storage grows in chunks of 16 slots from the injected allocator (PSRAM on the badge)
// and never moves or shrinks, so a reader of a slot never sees storage go away.
//
// Locking: the slot reads (app_ctx_valid, _resolve, _slot_owner) take no lock and are safe against a concurrent add
// or remove (owner is published before the generation, and cleared after it). Writers (add, remove) must be
// serialised by the caller, and so must the index view (app_ctx_apps, _app_at, _at, _of) against them: lichen_app.c
// holds a spinlock for both. The refusal counters are atomic.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_CTX_NONE       0u
#define APP_CTX_SLOT_BITS  8u
#define APP_CTX_SLOTS      (1u << APP_CTX_SLOT_BITS) // what the handle can name: the OS and 255 apps
#define APP_CTX_MAX_APPS   (APP_CTX_SLOTS - 1u)
#define APP_CTX_GEN_MASK   0xffffffu
#define APP_CTX_LOG_GAP_MS 10000u // a refusal is logged at most once per gap; the rest are only counted
#define APP_CTX_CHUNK      16u    // slots per storage chunk
#define APP_CTX_CHUNKS     (APP_CTX_SLOTS / APP_CTX_CHUNK)

typedef struct {
    void const *owner[APP_CTX_CHUNK]; // NULL: free (and [0] of chunk 0, the OS)
    uint32_t    gen[APP_CTX_CHUNK];   // 0 = slot free
    uint32_t    last[APP_CTX_CHUNK];  // the generation the slot had last, so a reuse never repeats it
    uint8_t     order[APP_CTX_CHUNK]; // the dense index view: order[i] (i = chunk * 16 + position) is app i's slot
    void       *state[APP_CTX_CHUNK]; // the app's own block (state_size bytes, zeroed), made with the slot, freed with it
} app_ctx_chunk_t;

// Where chunks and the apps' blocks come from: calloc-like (zeroed) and its free (NULL members mean calloc and free);
// and the lock the caller's readers of the index view hold (lock/unlock, NULL for none), taken here around the few
// stores of an add or a remove that move the view and never around an allocation. The storage is PSRAM, which no
// critical section may touch (#271), so the lock is a mutex.
typedef struct {
    void *(*alloc)(size_t size);
    void (*release)(void *p);
    void (*lock)(void);
    void (*unlock)(void);
} app_ctx_mem_t;

typedef struct {
    app_ctx_chunk_t *chunk[APP_CTX_CHUNKS]; // allocated on demand, never moved or freed
    app_ctx_mem_t    mem;
    uint32_t         os_gen;  // 0 = the table not set up yet
    uint16_t         apps;    // registered apps (the dense view's length)
    uint16_t         limit;   // most apps the table takes (<= APP_CTX_MAX_APPS)
    size_t           state_size; // bytes of each app's own block, 0 for none
    uint32_t    refused;              // ctx values refused since boot
    uint32_t    last_refused;         // the last one
    uint32_t    unlogged;             // refusals the rate limit kept out of the log since the last logged one
    uint32_t    logged_ms;            // when the last refusal was logged
    bool        logged_any;
} app_ctx_table_t;

// Sets up the OS slot with a generation drawn from draw (any 32-bit value; the table keeps 24 bits, never 0). The
// first form takes up to APP_CTX_MAX_APPS apps from calloc and keeps no block per app; the second takes at most limit
// apps (clamped) from mem and gives each a zeroed block of state_size bytes (an add fails when it cannot be had).
void app_ctx_init(app_ctx_table_t *t, uint32_t draw);
void app_ctx_init_with(app_ctx_table_t *t, uint32_t draw, app_ctx_mem_t const *mem, size_t limit, size_t state_size);
// The block of the app ctx stands for, NULL for a ctx that is not valid, the OS's, or a table without blocks. Valid
// until that app is removed (app_ctx_remove frees it): the OS keeps its readers off an app that is going. No lock.
void *app_ctx_state(app_ctx_table_t const *t, uint32_t ctx);
bool app_ctx_ready(app_ctx_table_t const *t);
// Gives the storage back (tests; the badge's table lives for the whole run). The table is not usable until app_ctx_init.
void app_ctx_destroy(app_ctx_table_t *t);

// Takes the lowest free slot for owner (not NULL) with a generation from draw and appends it to the dense view. Returns
// its ctx, APP_CTX_NONE when the table is full (the limit), out of memory or not set up.
uint32_t app_ctx_add(app_ctx_table_t *t, void const *owner, uint32_t draw);
// An owner's id and alias (either may come back NULL), for app_ctx_add_app
typedef void (*app_ctx_names_fn)(void const *owner, char const **id, char const **alias);
// As app_ctx_add, for an app (#340): refused, the table untouched and *why set (gate_app_names_refusal's reason), when
// its id or alias is no usable name or one an app already registered has as its id or alias. APP_CTX_NONE with *why
// NULL: the table is full or not set up.
uint32_t app_ctx_add_app(app_ctx_table_t *t, void const *owner, uint32_t draw, app_ctx_names_fn names, char const **why);

// Frees the app slot of ctx: the generation goes first (the ctx and every copy of it are refused from then on), then
// the owner, and the dense view closes the gap. False (nothing changed) for a ctx that is not valid or is the OS's.
bool app_ctx_remove(app_ctx_table_t *t, uint32_t ctx);

uint32_t    app_ctx_os(app_ctx_table_t const *t); // APP_CTX_NONE before app_ctx_init
size_t      app_ctx_apps(app_ctx_table_t const *t);
void const *app_ctx_app_at(app_ctx_table_t const *t, size_t index); // NULL past the end
uint32_t    app_ctx_at(app_ctx_table_t const *t, size_t index);     // the app's ctx, APP_CTX_NONE past the end
// The ctx of owner: the OS's for NULL, APP_CTX_NONE for a pointer no app registered (lichen_task_start's owner)
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

// The current context (#230): where the calling task keeps it (a FreeRTOS thread-local slot in lichen_app.c, a variable
// in the host test). get returns what set stored last, 0 when nothing was; in_isr says the caller is an interrupt,
// which has no slot of its own.
typedef struct {
    uint32_t (*get)(void);
    void (*set)(uint32_t ctx);
    bool (*in_isr)(void);
} app_ctx_tls_t;

// The context the caller runs for: what was entered, else (nothing entered, or an ISR) the OS's
uint32_t app_ctx_self(app_ctx_table_t const *t, app_ctx_tls_t const *tls);
// Makes ctx current and returns the previous value for app_ctx_leave. A ctx that is not valid is counted, *refused set
// (may be NULL) and the OS's entered instead. In an ISR nothing changes (returns 0).
uint32_t app_ctx_enter(app_ctx_table_t *t, app_ctx_tls_t const *tls, uint32_t ctx, bool *refused);
void     app_ctx_leave(app_ctx_tls_t const *tls, uint32_t previous);
// The context a new task runs for: owner's when owner is not NULL, else the caller's own (a task an app's task starts
// inherits the app). An owner that never registered gives the OS's, with *unregistered set (may be NULL).
uint32_t app_ctx_for_task(app_ctx_table_t const *t, app_ctx_tls_t const *tls, void const *owner, bool *unregistered);
// The owner of ctx's slot whatever its generation (what a refused ctx claims to be): NULL for the OS slot, a free slot
// or one past the table's storage
void const *app_ctx_slot_owner(app_ctx_table_t const *t, uint32_t ctx);
