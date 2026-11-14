// An app's lifecycle (#231), see app_life.h. Pure: no ESP-IDF, no locks, no app calls.
#include "app_life.h"

#include <string.h>

// a reached b: wrap-safe for spans under 24 days
static bool reached(uint32_t now_ms, uint32_t at_ms) {
    return (int32_t)(now_ms - at_ms) >= 0;
}

uint32_t app_life_deadline_ms(life_reason_t reason) {
    switch (reason) {
    case LIFE_STOP_UPDATE:
        return LIFE_DEADLINE_UPDATE_MS;
    case LIFE_STOP_RESTART:
    case LIFE_STOP_POWER_OFF:
        return LIFE_DEADLINE_RESTART_MS;
    default:
        return 0;
    }
}

char const *app_life_state_name(life_state_t s) {
    static char const *const names[LIFE_STATE_COUNT] = {"registered", "background", "open",   "suspended",
                                                        "stopping",   "stopped",    "faulted"};
    return (unsigned)s < LIFE_STATE_COUNT ? names[s] : "?";
}

char const *app_life_reason_name(life_reason_t r) {
    static char const *const names[LIFE_STOP_REASON_COUNT] = {"update", "restart", "power off"};
    return (unsigned)r < LIFE_STOP_REASON_COUNT ? names[r] : "?";
}

char const *app_life_fault_name(life_fault_t f) {
    static char const *const names[LIFE_FAULT_COUNT] = {"-", "crashes", "stop refused", "stop late"};
    return (unsigned)f < LIFE_FAULT_COUNT ? names[f] : "?";
}

void app_life_init(app_life_t *r) {
    memset(r, 0, sizeof(*r));
    r->state = LIFE_REGISTERED;
    r->rest  = LIFE_REGISTERED;
}

bool app_life_started(app_life_t *r) {
    if (r->state != LIFE_REGISTERED) {
        return false;
    }
    r->state = LIFE_BACKGROUND;
    r->rest  = LIFE_BACKGROUND;
    return true;
}

bool app_life_fault_crashes(app_life_t *r) {
    if (r->state != LIFE_REGISTERED) {
        return false;
    }
    r->state = LIFE_FAULTED;
    r->rest  = LIFE_FAULTED;
    r->fault = LIFE_FAULT_CRASHES;
    return true;
}

// A stop began at some point this boot: its outcome stays what the record says
static bool stop_seen(app_life_t const *r) {
    return r->stops != 0;
}

bool app_life_show(app_life_t *r, bool shown, bool on_top) {
    if (stop_seen(r)) { // STOPPING included: a stop counts from its begin
        return false;
    }
    uint8_t next = !shown ? r->rest : on_top ? LIFE_OPEN : LIFE_SUSPENDED;
    if (next == r->state) {
        return false;
    }
    r->state = next;
    return true;
}

life_begin_t app_life_stop_begin(app_life_t *r, life_reason_t reason, uint32_t now_ms) {
    uint32_t span = app_life_deadline_ms(reason);
    if (!span) {
        return LIFE_BEGIN_BAD;
    }
    if (r->state == LIFE_STOPPING) {
        return LIFE_BEGIN_BUSY;
    }
    r->state       = LIFE_STOPPING;
    r->reason      = (uint8_t)reason;
    r->deadline_ms = now_ms + span;
    r->tasks_left  = 0;
    if (r->stops < UINT8_MAX) {
        r->stops++;
    }
    return LIFE_BEGIN_OK;
}

life_state_t app_life_stop_end(app_life_t *r, bool stopped, uint32_t now_ms) {
    if (r->state != LIFE_STOPPING) {
        return LIFE_STATE_COUNT;
    }
    uint32_t began = r->deadline_ms - app_life_deadline_ms((life_reason_t)r->reason);
    uint32_t took  = now_ms - began;
    r->took_ms     = took > UINT16_MAX ? UINT16_MAX : (uint16_t)took;
    // Exactly at the deadline is late: the deadline is the first moment the hook is over its time
    bool late = reached(now_ms, r->deadline_ms);
    if (stopped && !late) {
        r->state = LIFE_STOPPED;
        r->fault = LIFE_FAULT_NONE;
    } else {
        r->state = LIFE_FAULTED;
        r->fault = !stopped ? LIFE_FAULT_STOP_REFUSED : LIFE_FAULT_STOP_LATE;
    }
    return (life_state_t)r->state;
}

void app_life_note_tasks(app_life_t *r, uint32_t tasks_left) {
    r->tasks_left = tasks_left > UINT8_MAX ? UINT8_MAX : (uint8_t)tasks_left;
}

life_wait_t app_life_wait(app_life_t const *r, uint32_t now_ms) {
    switch (r->state) {
    case LIFE_STOPPING:
        return reached(now_ms, r->deadline_ms) ? LIFE_WAIT_OVERDUE : LIFE_WAIT_PENDING;
    case LIFE_STOPPED:
        return LIFE_WAIT_STOPPED;
    default:
        return LIFE_WAIT_FAILED;
    }
}

void app_life_view(int const *ids, int depth, int app_id, int app_lo, int app_hi, bool *shown, bool *on_top) {
    bool other_above = false;
    *shown           = false;
    *on_top          = false;
    for (int i = depth - 1; i >= 0; i--) {
        if (ids[i] == app_id) {
            *shown  = true;
            *on_top = !other_above; // the app's own pages and the OS's (Settings, a dialog page) above it do not count
            return;
        }
        other_above = other_above || (ids[i] >= app_lo && ids[i] <= app_hi);
    }
}
