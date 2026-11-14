#include "mesh_run.h"

#include <stdio.h>

// One handler per event, in the event's order; each returns whether r changed

static bool on_start(mesh_run_t *r) {
    if (r->state != MESH_RUN_STOPPED || r->why == MESH_WHY_UPDATE || r->paused) {
        return false;
    }
    r->state = MESH_RUN_STARTING;
    return true;
}

static bool on_stop(mesh_run_t *r) {
    if (r->state != MESH_RUN_RUNNING) {
        return false;
    }
    r->state = MESH_RUN_STOPPING;
    return true;
}

static bool on_done_ok(mesh_run_t *r) {
    if (r->state == MESH_RUN_STARTING) {
        r->state = MESH_RUN_RUNNING;
        return true;
    }
    if (r->state == MESH_RUN_STOPPING) {
        r->state = MESH_RUN_STOPPED; // paused stays: this stop may be the pause's
        r->why   = MESH_WHY_USER;
        return true;
    }
    return false;
}

static bool on_done_fail(mesh_run_t *r) {
    if (r->state == MESH_RUN_STARTING) {
        *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_FAILED, r->paused};
        return true;
    }
    if (r->state == MESH_RUN_STOPPING) {
        r->state = MESH_RUN_RUNNING;
        return true;
    }
    return false;
}

static bool on_update(mesh_run_t *r) {
    if (r->state == MESH_RUN_STOPPED && r->why == MESH_WHY_UPDATE) {
        return false;
    }
    *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_UPDATE, false};
    return true;
}

static bool on_seen_up(mesh_run_t *r) {
    // Only a settled state is corrected: a start or stop under way reports for itself
    if (r->state != MESH_RUN_STOPPED || r->why == MESH_WHY_UPDATE) {
        return false;
    }
    r->state = MESH_RUN_RUNNING;
    return true;
}

static bool on_seen_down(mesh_run_t *r) {
    if (r->state != MESH_RUN_RUNNING) {
        return false;
    }
    *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_FAILED, false};
    return true;
}

static bool on_pause(mesh_run_t *r) {
    if ((r->state == MESH_RUN_STOPPED && r->why == MESH_WHY_UPDATE) || r->paused) {
        return false;
    }
    r->paused = true;
    return true;
}

static bool on_unpause(mesh_run_t *r) {
    if (!r->paused) {
        return false;
    }
    r->paused = false;
    return true;
}

bool mesh_run_event(mesh_run_t *r, mesh_run_ev_t ev) {
    switch (ev) {
        case MESH_EV_START: return on_start(r);
        case MESH_EV_STOP: return on_stop(r);
        case MESH_EV_DONE_OK: return on_done_ok(r);
        case MESH_EV_DONE_FAIL: return on_done_fail(r);
        case MESH_EV_UPDATE: return on_update(r);
        case MESH_EV_SEEN_UP: return on_seen_up(r);
        case MESH_EV_SEEN_DOWN: return on_seen_down(r);
        case MESH_EV_PAUSE: return on_pause(r);
        case MESH_EV_UNPAUSE: return on_unpause(r);
    }
    return false;
}

bool mesh_run_busy(mesh_run_t r) {
    return r.state == MESH_RUN_STARTING || r.state == MESH_RUN_STOPPING;
}

bool mesh_run_tile_text(mesh_run_t r, bool lora_ok, char *line1, size_t c1, char *line2, size_t c2) {
    char const *l1 = "Not running";
    char const *l2 = "Enter to start";
    switch (r.state) {
        case MESH_RUN_RUNNING: return false;
        case MESH_RUN_STARTING: l1 = "Starting..."; l2 = ""; break;
        case MESH_RUN_STOPPING: l1 = "Stopping..."; l2 = ""; break;
        case MESH_RUN_STOPPED:
            if (r.why == MESH_WHY_UPDATE) {
                l2 = "Stopped for update";
            } else if (r.paused) {
                l1 = "Paused";
                l2 = "LoRa spectrum is open";
            } else if (!lora_ok) {
                l2 = "Needs the LoRa radio";
            }
            break;
    }
    snprintf(line1, c1, "%s", l1);
    snprintf(line2, c2, "%s", l2);
    return true;
}

char const *mesh_run_grid_text(mesh_run_t r, bool lora_ok) {
    switch (r.state) {
        case MESH_RUN_RUNNING: return "";
        case MESH_RUN_STARTING: return "Starting MeshCore...";
        case MESH_RUN_STOPPING: return "Stopping MeshCore...";
        case MESH_RUN_STOPPED: break;
    }
    if (r.why == MESH_WHY_UPDATE) {
        return "MeshCore stopped for update.";
    }
    if (r.paused) {
        return "MeshCore is paused: LoRa spectrum is open.";
    }
    if (!lora_ok) {
        return "MeshCore is not running: it needs the LoRa radio.";
    }
    return r.why == MESH_WHY_FAILED ? "MeshCore did not start. Enter starts it again." :
                                      "MeshCore is not running. Enter starts it.";
}

char const *mesh_run_action_label(mesh_run_t r) {
    switch (r.state) {
        case MESH_RUN_RUNNING: return "Stop MeshCore";
        case MESH_RUN_STARTING: return "Starting...";
        case MESH_RUN_STOPPING: return "Stopping...";
        case MESH_RUN_STOPPED: break;
    }
    if (r.why != MESH_WHY_UPDATE && r.paused) {
        return "Paused";
    }
    return r.why == MESH_WHY_UPDATE ? "Stopped for update" : "Start MeshCore";
}

bool mesh_run_enter_starts(mesh_run_t r, bool on_top) {
    return on_top && r.state == MESH_RUN_STOPPED && r.why != MESH_WHY_UPDATE;
}

char const *mesh_run_start_refusal(mesh_run_t r, bool lora_ok, key_cause_t cause) {
    if (r.state == MESH_RUN_RUNNING) {
        return "already running";
    }
    if (r.why == MESH_WHY_UPDATE) {
        return "stopped for update";
    }
    if (!lora_ok) {
        return "no LoRa radio";
    }
    return key_cause_hand_only_ok(cause) ? NULL : MESH_RUN_HAND_ONLY_REFUSAL;
}

char const *mesh_run_action_value(mesh_run_t r) {
    if (mesh_run_busy(r) || (r.state == MESH_RUN_STOPPED && r.why == MESH_WHY_UPDATE)) {
        return "";
    }
    return ""; // the row's label says what Enter does; the value slot is for state (R3)
}

char const *mesh_run_autostart_text(bool on) {
    return on ? "On" : "Off";
}

char const *mesh_run_console_text(mesh_run_t r) {
    switch (r.state) {
        case MESH_RUN_RUNNING: return NULL;
        case MESH_RUN_STARTING: return "starting...";
        case MESH_RUN_STOPPING: return "stopping...";
        case MESH_RUN_STOPPED: break;
    }
    if (r.paused && r.why != MESH_WHY_UPDATE) {
        return "paused (LoRa spectrum is open)";
    }
    switch (r.why) {
        case MESH_WHY_AUTOSTART_OFF: return "stopped (autostart off)";
        case MESH_WHY_USER: return "stopped";
        case MESH_WHY_FAILED: return "not running (did not start, or stopped by itself)";
        case MESH_WHY_UPDATE: return "stopped for update";
    }
    return "stopped";
}

bool mesh_run_wants_radio(mesh_run_t r) {
    // Paused by the OS counts as by choice: the spectrum has the radio, nothing is broken
    return !(r.state == MESH_RUN_STOPPED && (r.why == MESH_WHY_AUTOSTART_OFF || r.why == MESH_WHY_USER || r.paused));
}
