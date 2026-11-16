#include "update_mode.h"

static void (*stop_hook)(bool apps_stopped);

static unsigned enter(update_state_t *s) {
    if (s->mode == UM_UPDATING) {
        return UM_ACT_NONE;
    }
    s->mode = UM_UPDATING;
    return UM_ACT_STOP_APPS | UM_ACT_CLOSE_SCREENS;
}

unsigned update_mode_on_prepare(update_state_t *s, int64_t now_ms) {
    unsigned act    = enter(s);
    s->prepared     = true;
    s->prepared_ms  = now_ms;
    s->reboot       = false;
    return act;
}

unsigned update_mode_on_begin(update_state_t *s, bool firmware) {
    if (!firmware) {
        return s->mode == UM_UPDATING ? UM_ACT_REFUSE : UM_ACT_NONE;
    }
    s->prepared = false;
    s->reboot   = false;
    return enter(s);
}

unsigned update_mode_on_end(update_state_t *s, bool firmware, bool ok) {
    if (s->mode != UM_UPDATING) {
        return UM_ACT_NONE;
    }
    if (!firmware) {
        return UM_ACT_REFUSE;
    }
    if (ok) {
        return UM_ACT_NONE;
    }
    s->reboot = true;
    return UM_ACT_REBOOT;
}

unsigned update_mode_tick(update_state_t *s, int64_t now_ms) {
    if (s->prepared && now_ms - s->prepared_ms >= UPDATE_PREPARE_TIMEOUT_MS) {
        s->prepared = false;
        s->reboot   = true;
        return UM_ACT_REBOOT;
    }
    return UM_ACT_NONE;
}

void update_mode_on_restart_stall(update_state_t *s) {
    if (s->mode == UM_UPDATING) {
        s->reboot = true;
    }
}

bool update_mode_file_allowed(update_state_t const *s) {
    return s->mode != UM_UPDATING;
}

bool update_mode_restart_due(update_state_t const *s, bool message_done) {
    return s->reboot && message_done;
}

void update_mode_set_stop_hook(void (*hook)(bool apps_stopped)) {
    stop_hook = hook;
}

void update_mode_run_stop_hook(bool apps_stopped) {
    if (stop_hook) {
        stop_hook(apps_stopped);
    }
}
