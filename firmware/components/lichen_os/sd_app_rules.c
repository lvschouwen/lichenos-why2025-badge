// The SD apps' loader rules (#523), see sd_app_rules.h. Pure.
#include "sd_app_rules.h"

#include "lichen_api_version.h"

#include <stdio.h>
#include <string.h>

sd_app_scan_t sd_app_scan_verdict(lichen_manifest_err_t err, lichen_manifest_t const *m, bool id_taken, bool entry_ok) {
    if (err != LICHEN_MANIFEST_OK || !m) {
        return SD_APP_SCAN_BAD_MANIFEST;
    }
    if (!lichen_api_compatible(m->api_major, m->api_minor)) {
        return SD_APP_SCAN_API;
    }
    if (m->stack_kib > SD_APP_STACK_KIB_MAX) {
        return SD_APP_SCAN_STACK;
    }
    if (id_taken) {
        return SD_APP_SCAN_SHADOW;
    }
    return entry_ok ? SD_APP_SCAN_OK : SD_APP_SCAN_NO_ENTRY;
}

bool sd_app_push_is_manifest(char const *apps_dir, char const *path) {
    size_t n = strlen(apps_dir);
    if (!path || strncmp(path, apps_dir, n) != 0 || path[n] != '/') {
        return false;
    }
    char const *id  = path + n + 1;
    char const *end = strchr(id, '/');
    size_t      len = end ? (size_t)(end - id) : 0;
    return end && len > 0 && len < LICHEN_MANIFEST_ID_MAX && id[0] != '.' && strcmp(end + 1, "manifest") == 0;
}

bool sd_app_push_marks(sd_push_rec_t rec) {
    return rec != SD_PUSH_DEAD;
}

uint32_t sd_app_task_budget(uint16_t internal_ram_kib) {
    return (internal_ram_kib ? internal_ram_kib : SD_APP_RAM_KIB_DEFAULT) * 1024u;
}

void sd_app_scan_reason(sd_app_scan_t v, lichen_manifest_err_t err, lichen_manifest_t const *m, char const *dir_id,
                        char *out, size_t cap) {
    if (!out || !cap) {
        return;
    }
    char const *id = (v != SD_APP_SCAN_BAD_MANIFEST && m) ? m->id : (dir_id ? dir_id : "?");
    switch (v) {
    case SD_APP_SCAN_BAD_MANIFEST:
        snprintf(out, cap, "%s: bad manifest (%s)", id, lichen_manifest_err_text(err));
        break;
    case SD_APP_SCAN_API:
        snprintf(out, cap, "%s needs API %u.%u, this is " LICHEN_API_VERSION_TEXT, id, (unsigned)m->api_major,
                 (unsigned)m->api_minor);
        break;
    case SD_APP_SCAN_SHADOW:
        snprintf(out, cap, "%s is already an app here", id);
        break;
    case SD_APP_SCAN_STACK:
        snprintf(out, cap, "%s: stack %u KiB, at most %u", id, (unsigned)m->stack_kib, (unsigned)SD_APP_STACK_KIB_MAX);
        break;
    case SD_APP_SCAN_NO_ENTRY:
        snprintf(out, cap, "%s: %s is missing", id, m ? m->entry : "?");
        break;
    case SD_APP_SCAN_OK:
    default:
        snprintf(out, cap, "%s: refused", id);
        break;
    }
}

sd_open_t sd_app_open(sd_app_t *a, char const *hold_why) {
    if (a->removing) {
        return SD_OPEN_REMOVED;
    }
    switch (a->state) {
    case SD_APP_FAULTED:
        return a->late && !a->late_stale ? SD_OPEN_BUSY : SD_OPEN_FAULTED; // a resumable fault is a slow close, not a dead app
    case SD_APP_UNLOADING:
        return SD_OPEN_BUSY;
    case SD_APP_LOADING:
        a->closing = false; // opened again before the load ended: it stays
        a->screens++;
        return SD_OPEN_WAIT;
    case SD_APP_LOADED: // an app that is loaded reopens under the hold on purpose: the hold guards loads, not screens
        a->closing = false;
        a->screens++;
        return SD_OPEN_NOW;
    case SD_APP_TILE:
    default:
        break;
    }
    if (hold_why) {
        return SD_OPEN_HELD;
    }
    a->state   = SD_APP_LOADING;
    a->closing = false;
    a->screens++;
    return SD_OPEN_LOAD;
}

char const *sd_open_text(sd_open_t v) {
    switch (v) {
    case SD_OPEN_HELD:
        return "Not while the on-air hold is on";
    case SD_OPEN_BUSY:
        return "Still closing: try again";
    case SD_OPEN_FAULTED:
        return "It did not stop: closed until a restart";
    case SD_OPEN_REMOVED:
        return "The card is gone";
    default:
        return "Loading";
    }
}

sd_next_t sd_app_load_done(sd_app_t *a, bool ok) {
    if (a->state != SD_APP_LOADING) {
        return SD_NEXT_NONE;
    }
    if (!ok) {
        a->state   = SD_APP_TILE;
        a->closing = false;
        return SD_NEXT_NONE;
    }
    a->state = SD_APP_LOADED;
    return (a->closing || a->removing) ? SD_NEXT_UNLOAD : SD_NEXT_OPEN;
}

bool sd_app_load_skip(sd_app_t *a) {
    if (a->state != SD_APP_LOADING || !a->closing || a->screens > 0 || a->removing) {
        return false;
    }
    a->state   = SD_APP_TILE;
    a->closing = false;
    return true;
}

bool sd_app_load_abort(sd_app_t *a) {
    if (a->state != SD_APP_LOADING) {
        return false;
    }
    a->state   = SD_APP_UNLOADING;
    a->closing = false;
    return true;
}

bool sd_app_screen_deleted(sd_app_t *a) {
    if (a->screens > 0) {
        a->screens--;
    }
    if (a->screens > 0 || (a->state != SD_APP_LOADED && a->state != SD_APP_LOADING)) {
        return false; // a FAULTED app's last screen: the loader's late unload is claimed apart (sd_app_late_claim)
    }
    a->closing = true;
    return a->state == SD_APP_LOADED;
}

bool sd_app_unload_begin(sd_app_t *a) {
    if (a->state != SD_APP_LOADED || !(a->removing || (a->closing && a->screens == 0))) {
        return false;
    }
    a->state   = SD_APP_UNLOADING;
    a->closing = false;
    return true;
}

bool sd_app_may_free(sd_app_t const *a) {
    return a->screens == 0;
}

bool sd_app_may_unregister(sd_app_t const *a, size_t tasks_left) {
    return (a->state == SD_APP_TILE || a->state == SD_APP_UNLOADING) && tasks_left == 0;
}

void sd_app_unload_done(sd_app_t *a, bool clean) {
    if (a->state == SD_APP_UNLOADING) {
        a->state = clean ? SD_APP_TILE : SD_APP_FAULTED;
        a->late  = false;
    }
}

void sd_app_unload_fault(sd_app_t *a, sd_unload_step_t at) {
    if (a->state == SD_APP_UNLOADING) {
        a->state = SD_APP_FAULTED;
        a->late  = at == SD_UNLOAD_STOP || at == SD_UNLOAD_TASKS;
        a->late_queued = false;
        a->late_stale  = false;
    }
}

void sd_app_late_expire(sd_app_t *a) {
    a->late_stale = a->state == SD_APP_FAULTED && a->late;
}

bool sd_app_late_claim(sd_app_t *a) {
    if (a->state != SD_APP_FAULTED || !a->late || a->screens != 0 || a->late_queued) {
        return false;
    }
    a->late_queued = true;
    return true;
}

void sd_app_late_unclaim(sd_app_t *a) {
    a->late_queued = false;
}

bool sd_app_late_begin(sd_app_t *a, size_t tasks_left) {
    a->late_stale  = false;
    a->late_queued = false; // the message that asked is consumed, whatever the answer
    if (a->state != SD_APP_FAULTED || !a->late || tasks_left != 0 || a->screens != 0) {
        return false;
    }
    a->state = SD_APP_UNLOADING;
    a->late  = false;
    return true;
}

bool sd_app_remove(sd_app_t *a) {
    a->removing = true;
    return a->state == SD_APP_LOADED || a->state == SD_APP_LOADING;
}

bool sd_app_hooks_live(sd_app_t const *a) {
    return a->state == SD_APP_LOADED;
}

sd_unload_step_t sd_unload_next(sd_unload_step_t cur, bool ok) {
    if (!ok) {
        return SD_UNLOAD_FAULT;
    }
    return cur < SD_UNLOAD_DONE ? (sd_unload_step_t)(cur + 1) : cur; // the enum is the order
}

unsigned sd_app_exit(sd_app_t *a) {
    switch (a->state) {
    case SD_APP_LOADING: // main() exited: the loader's wait ends, and a load that did finish unloads at once
        a->closing = true;
        return SD_EXIT_WAKE | (a->screens > 0 ? SD_EXIT_HOME : 0u);
    case SD_APP_LOADED:
        if (a->screens > 0) {
            return SD_EXIT_HOME;
        }
        a->closing = true;
        return SD_EXIT_UNLOAD;
    default:
        return SD_EXIT_NONE; // unloading, faulted or not loaded: the unload that runs (or none) is all there is
    }
}

bool sd_app_free_enters(bool unregistered) {
    return !unregistered;
}

int sd_progress_pct(size_t done, size_t total) {
    if (total == 0) {
        return 0;
    }
    return done >= total ? 100 : (int)((uint64_t)done * 100 / total);
}

bool sd_progress_due(sd_progress_t const *p, size_t done, size_t total, uint32_t now_ms, int *pct) {
    int v = sd_progress_pct(done, total);
    if (v > 99) {
        v = 99;
    }
    if (p->any && (v <= p->last_pct || now_ms - p->last_ms < SD_PROGRESS_MIN_MS)) {
        return false;
    }
    *pct = v;
    return true;
}

void sd_progress_sent(sd_progress_t *p, int pct, uint32_t now_ms) {
    p->any      = true;
    p->last_pct = pct;
    p->last_ms  = now_ms;
}
