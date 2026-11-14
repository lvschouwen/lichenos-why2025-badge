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

// A stop's outcome holds the record: STOPPING, STOPPED, or FAULTED by the stop (not by safe mode)
static bool stop_holds(app_life_t const *r) {
    return r->state == LIFE_STOPPING || r->state == LIFE_STOPPED ||
           (r->state == LIFE_FAULTED && (r->fault == LIFE_FAULT_STOP_REFUSED || r->fault == LIFE_FAULT_STOP_LATE));
}

bool app_life_started(app_life_t *r) {
    bool restart = stop_holds(r) && r->state != LIFE_STOPPING;
    if ((r->state != LIFE_REGISTERED && !restart) || r->rest == LIFE_FAULTED) {
        return false; // safe mode keeps a crashed app's background off whatever starts it
    }
    r->state = LIFE_BACKGROUND;
    r->rest  = LIFE_BACKGROUND;
    r->fault = LIFE_FAULT_NONE;
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

bool app_life_show(app_life_t *r, bool shown, bool on_top) {
    if (stop_holds(r)) {
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
        r->fault = r->rest == LIFE_FAULTED ? LIFE_FAULT_CRASHES : LIFE_FAULT_NONE; // safe mode's verdict outlives a stop
    } else {
        r->state = LIFE_FAULTED;
        r->fault = !stopped ? LIFE_FAULT_STOP_REFUSED : LIFE_FAULT_STOP_LATE;
    }
    return (life_state_t)r->state;
}

void app_life_note_tasks(app_life_t *r, uint32_t tasks_left) {
    r->tasks_left = tasks_left > UINT8_MAX ? UINT8_MAX : (uint8_t)tasks_left;
}

life_wait_t app_life_wait_verdict(app_life_t const *r, life_reason_t own, uint32_t since_ms, uint32_t now_ms) {
    switch (r->state) {
    case LIFE_STOPPING:
        // The waiter's own bound, from when it began to wait: a restart behind a hung update hook still gives that
        // hook its 8 s, as the restart did when it ran the hook itself
        return reached(now_ms, since_ms + app_life_deadline_ms(own)) ? LIFE_WAIT_OVERDUE : LIFE_WAIT_PENDING;
    case LIFE_STOPPED:
        return LIFE_WAIT_STOPPED;
    case LIFE_FAULTED:
        // Late is still stopped: the hook said so, only after its deadline
        return r->fault == LIFE_FAULT_STOP_LATE ? LIFE_WAIT_STOPPED : LIFE_WAIT_FAILED;
    default:
        return LIFE_WAIT_FAILED;
    }
}

int app_life_screen_owner(int id, int pushed_slot, int app_lo, int app_hi, int settings_base) {
    if (id >= app_lo && id <= app_hi) {
        return id - app_lo + 1;
    }
    if (id >= settings_base && id <= settings_base + (app_hi - app_lo)) {
        return id - settings_base + 1;
    }
    return pushed_slot > 0 ? pushed_slot : 0;
}

void app_life_view(int const *owners, int depth, int slot, bool *shown, bool *on_top) {
    bool other_above = false;
    *shown           = false;
    *on_top          = false;
    for (int i = depth - 1; slot > 0 && i >= 0; i--) {
        if (owners[i] == slot) {
            *shown  = true;
            *on_top = !other_above; // the OS's pages above it (Settings, a dialog page) do not count
            return;
        }
        other_above = other_above || owners[i] > 0;
    }
}
