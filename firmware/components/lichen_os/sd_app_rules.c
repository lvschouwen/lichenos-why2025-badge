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
    if (id_taken) {
        return SD_APP_SCAN_SHADOW;
    }
    return entry_ok ? SD_APP_SCAN_OK : SD_APP_SCAN_NO_ENTRY;
}

void sd_app_scan_reason(sd_app_scan_t v, lichen_manifest_err_t err, lichen_manifest_t const *m, char const *dir_id,
                        char *out, size_t cap) {
    if (!out || !cap) {
        return;
    }
    char const *id = (v != SD_APP_SCAN_BAD_MANIFEST && m) ? m->id : (dir_id ? dir_id : "?");
    switch (v) {
    case SD_APP_SCAN_OK:
        snprintf(out, cap, "%s is fine", id);
        break;
    case SD_APP_SCAN_BAD_MANIFEST:
        snprintf(out, cap, "%s: bad manifest (%s)", id, lichen_manifest_err_text(err));
        break;
    case SD_APP_SCAN_API:
        snprintf(out, cap, "%s needs app API %u.%u, this is " LICHEN_API_VERSION_TEXT, id, (unsigned)m->api_major,
                 (unsigned)m->api_minor);
        break;
    case SD_APP_SCAN_SHADOW:
        snprintf(out, cap, "%s is the name of an app that is already here", id);
        break;
    case SD_APP_SCAN_NO_ENTRY:
        snprintf(out, cap, "%s: the program file %s is missing", id, m ? m->entry : "?");
        break;
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
        return SD_OPEN_FAULTED;
    case SD_APP_UNLOADING:
        return SD_OPEN_BUSY;
    case SD_APP_LOADING:
        a->closing = false; // opened again before the load ended: it stays
        return SD_OPEN_WAIT;
    case SD_APP_LOADED:
        a->closing = false;
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
    return SD_OPEN_LOAD;
}

char const *sd_open_text(sd_open_t v) {
    switch (v) {
    case SD_OPEN_HELD:
        return "Not while the on-air hold is on";
    case SD_OPEN_BUSY:
        return "Still closing: try again in a moment";
    case SD_OPEN_FAULTED:
        return "This app did not stop in time: it stays closed until the card is out";
    case SD_OPEN_REMOVED:
        return "The card is gone";
    case SD_OPEN_LOAD:
    case SD_OPEN_WAIT:
    case SD_OPEN_NOW:
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

bool sd_app_closed(sd_app_t *a) {
    if (a->state != SD_APP_LOADED && a->state != SD_APP_LOADING) {
        return false;
    }
    a->closing = true;
    return a->state == SD_APP_LOADED;
}

bool sd_app_unload_begin(sd_app_t *a) {
    if (a->state != SD_APP_LOADED || !(a->closing || a->removing)) {
        return false;
    }
    a->state   = SD_APP_UNLOADING;
    a->closing = false;
    return true;
}

void sd_app_unload_done(sd_app_t *a, bool clean) {
    if (a->state == SD_APP_UNLOADING) {
        a->state = clean ? SD_APP_TILE : SD_APP_FAULTED;
    }
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
    switch (cur) {
    case SD_UNLOAD_STOP:
        return SD_UNLOAD_TASKS;
    case SD_UNLOAD_TASKS:
        return SD_UNLOAD_UI;
    case SD_UNLOAD_UI:
        return SD_UNLOAD_FREE;
    case SD_UNLOAD_FREE:
        return SD_UNLOAD_DONE;
    case SD_UNLOAD_DONE:
    case SD_UNLOAD_FAULT:
    default:
        return cur;
    }
}

char const *sd_unload_step_name(sd_unload_step_t s) {
    static char const *const names[] = {"stop hook", "tasks", "timers and UI calls", "free", "done", "fault"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
