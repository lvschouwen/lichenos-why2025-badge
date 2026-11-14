#pragma once
// An app's lifecycle (#231), the pure part: one small record per app and the transitions the OS may make on it. The
// record is the OS's view of the app, kept next to its context (lichen_app_life.c, under a spinlock); nothing here
// calls an app or waits. ESP-free, host-tested in host_tests/app_life_test.c.
//
//   REGISTERED  registered, nothing of it runs (no background part, or not started yet)
//   BACKGROUND  its background part runs (start_background said yes), no screen of it is open
//   OPEN        its screen is open and no other app's screen is above it
//   SUSPENDED   its screen is open below another app's
//   STOPPING    stop(reason) called its stop hook; the hook has until the deadline
//   STOPPED     the hook said it stopped, in time
//   FAULTED     safe mode switched its background part off (#232), or its last stop was refused or late
//
// Where a close returns (the rest state) is REGISTERED, BACKGROUND or FAULTED: the background part runs on (or stays
// off) whether a screen is open or not. A stop is cooperative: the OS calls the app's hook and records what came of
// it; a hook that overruns its deadline is reported (FAULTED, late), never interrupted, and its tasks are never
// deleted by the OS. Once a stop began, the screens no longer move the state (a closing update screen is no resume).
// A stop may be asked again after one ended (an update, then the restart's save): the hook runs again, as it did
// before the lifecycle existed. Times are uint32_t milliseconds that may wrap.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LIFE_REGISTERED, // 0: a zeroed record is a registered app
    LIFE_BACKGROUND,
    LIFE_OPEN,
    LIFE_SUSPENDED,
    LIFE_STOPPING,
    LIFE_STOPPED,
    LIFE_FAULTED,
    LIFE_STATE_COUNT,
} life_state_t;

// Why the OS stops the apps; each is final for this boot (the badge restarts or goes off afterwards)
typedef enum {
    LIFE_STOP_UPDATE,    // a firmware image is coming (the OTA task, #76)
    LIFE_STOP_RESTART,   // a deliberate restart: the restart's save (#123) or the Power app
    LIFE_STOP_POWER_OFF, // the Power app or a headless power off (#281)
    LIFE_STOP_REASON_COUNT,
} life_reason_t;

typedef enum {
    LIFE_FAULT_NONE,
    LIFE_FAULT_CRASHES,      // safe mode: 2 attributed crashes in 3 boots, background off (#232)
    LIFE_FAULT_STOP_REFUSED, // the stop hook said it is still running
    LIFE_FAULT_STOP_LATE,    // the stop hook returned after the deadline
    LIFE_FAULT_COUNT,
} life_fault_t;

// Under 16 B per app (#231's budget): 25 records are 300 B of internal .bss
typedef struct {
    uint8_t  state;       // life_state_t
    uint8_t  rest;        // where a close returns: LIFE_REGISTERED, LIFE_BACKGROUND or LIFE_FAULTED
    uint8_t  reason;      // the last stop's life_reason_t
    uint8_t  fault;       // life_fault_t, why it is FAULTED
    uint32_t deadline_ms; // STOPPING: the hook is late from this moment on
    uint16_t took_ms;     // how long the last stop hook ran, capped at 65535
    uint8_t  tasks_left;  // the app's booked tasks still running when its last stop ended (reported, never deleted)
    uint8_t  stops;       // stops begun this boot, capped at 255
} app_life_t;

// How long an app's stop hook may take for reason before it counts as late: the update waits behind a MeshCore start
// for up to 15 s (mesh_stop); a restart's save is bounded at 8 s in all (SYSTEM_RESTART_SAVE_MS)
#define LIFE_DEADLINE_UPDATE_MS  20000u
#define LIFE_DEADLINE_RESTART_MS 8000u
uint32_t app_life_deadline_ms(life_reason_t reason); // 0 for an unknown reason

char const *app_life_state_name(life_state_t s);   // "registered" ... "faulted"; "?" for an unknown one
char const *app_life_reason_name(life_reason_t r); // "update", "restart", "power off"; "?"
char const *app_life_fault_name(life_fault_t f);   // "-", "crashes", "stop refused", "stop late"; "?"

// The record of a newly registered app (REGISTERED)
void app_life_init(app_life_t *r);
// Boot: the background part started. REGISTERED -> BACKGROUND; false (nothing changes) from any other state.
bool app_life_started(app_life_t *r);
// Boot: safe mode switched the background part off (#232's 2-in-3 rule; an exempt app never gets here). REGISTERED
// -> FAULTED (crashes); false from any other state.
bool app_life_fault_crashes(app_life_t *r);
// The shell's stack changed: shown says one of the app's screens is on it, on_top that no other app's is above it.
// OPEN, SUSPENDED or the rest state follow; nothing changes during or after a stop. True when the state changed.
bool app_life_show(app_life_t *r, bool shown, bool on_top);

typedef enum {
    LIFE_BEGIN_OK,   // STOPPING now, the deadline set: call the hook, then app_life_stop_end
    LIFE_BEGIN_BUSY, // another caller is stopping it: wait for the verdict (app_life_wait)
    LIFE_BEGIN_BAD,  // an unknown reason: nothing changes
} life_begin_t;

// stop(reason) begins at now_ms: from any state but STOPPING
life_begin_t app_life_stop_begin(app_life_t *r, life_reason_t reason, uint32_t now_ms);
// The hook returned at now_ms saying stopped: STOPPED when it did, in time; else FAULTED (refused, or late when it
// returned after the deadline). Returns the new state; LIFE_STATE_COUNT (nothing changes) when no stop was under way.
life_state_t app_life_stop_end(app_life_t *r, bool stopped, uint32_t now_ms);
// The app's booked tasks still running after its stop (capped at 255): reported in "fw app life"
void app_life_note_tasks(app_life_t *r, uint32_t tasks_left);

typedef enum {
    LIFE_WAIT_PENDING, // STOPPING, before the deadline: ask again
    LIFE_WAIT_OVERDUE, // STOPPING past the deadline: the hook hangs; give up waiting, report it
    LIFE_WAIT_STOPPED, // the stop ended in STOPPED
    LIFE_WAIT_FAILED,  // the stop ended in FAULTED, or no stop was asked
} life_wait_t;

// For a second caller of stop(reason) while the first one's hook runs
life_wait_t app_life_wait(app_life_t const *r, uint32_t now_ms);

// Which of an app's screens the shell's stack shows (#231): ids[0..depth) bottom to top; the app's own screen is
// app_id, and every id in app_lo..app_hi is some app's. *on_top: no other app's screen is above the app's.
void app_life_view(int const *ids, int depth, int app_id, int app_lo, int app_hi, bool *shown, bool *on_top);

#ifdef __cplusplus
}
#endif
