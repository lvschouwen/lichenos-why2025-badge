#include "mesh_run.h"

#include <stdio.h>

bool mesh_run_event(mesh_run_t *r, mesh_run_ev_t ev) {
    mesh_run_state_t s = r->state;
    switch (ev) {
        case MESH_EV_START:
            if (s != MESH_RUN_STOPPED || r->why == MESH_WHY_UPDATE) {
                return false;
            }
            r->state = MESH_RUN_STARTING;
            return true;
        case MESH_EV_STOP:
            if (s != MESH_RUN_RUNNING) {
                return false;
            }
            r->state = MESH_RUN_STOPPING;
            return true;
        case MESH_EV_DONE_OK:
            if (s == MESH_RUN_STARTING) {
                r->state = MESH_RUN_RUNNING;
                return true;
            }
            if (s == MESH_RUN_STOPPING) {
                *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_USER};
                return true;
            }
            return false;
        case MESH_EV_DONE_FAIL:
            if (s == MESH_RUN_STARTING) {
                *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_FAILED};
                return true;
            }
            if (s == MESH_RUN_STOPPING) {
                r->state = MESH_RUN_RUNNING;
                return true;
            }
            return false;
        case MESH_EV_UPDATE:
            if (s == MESH_RUN_STOPPED && r->why == MESH_WHY_UPDATE) {
                return false;
            }
            *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_UPDATE};
            return true;
        case MESH_EV_SEEN_UP:
            // Only a settled state is corrected: a start or stop under way reports for itself
            if (s != MESH_RUN_STOPPED || r->why == MESH_WHY_UPDATE) {
                return false;
            }
            r->state = MESH_RUN_RUNNING;
            return true;
        case MESH_EV_SEEN_DOWN:
            if (s != MESH_RUN_RUNNING) {
                return false;
            }
            *r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_FAILED};
            return true;
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
    return r.why == MESH_WHY_UPDATE ? "Stopped for update" : "Start MeshCore";
}

char const *mesh_run_action_value(mesh_run_t r) {
    if (mesh_run_busy(r) || (r.state == MESH_RUN_STOPPED && r.why == MESH_WHY_UPDATE)) {
        return "";
    }
    return "Enter";
}

char const *mesh_run_autostart_text(bool on) {
    return on ? "on" : "off";
}

char const *mesh_run_console_text(mesh_run_t r) {
    switch (r.state) {
        case MESH_RUN_RUNNING: return NULL;
        case MESH_RUN_STARTING: return "starting...";
        case MESH_RUN_STOPPING: return "stopping...";
        case MESH_RUN_STOPPED: break;
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
    return !(r.state == MESH_RUN_STOPPED && (r.why == MESH_WHY_AUTOSTART_OFF || r.why == MESH_WHY_USER));
}
