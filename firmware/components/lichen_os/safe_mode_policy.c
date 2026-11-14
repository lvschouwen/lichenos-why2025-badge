// Crash attribution and safe mode (#232): the pure rules (safe_mode_policy.h) that run at boot and on the console.
// What the panic handler runs is in safe_mode_rec.c.
#include "safe_mode_policy.h"

#include <string.h>

#define FNV_BASIS 2166136261u
#define FNV_PRIME 16777619u

uint32_t safe_mode_hash(char const *id) {
    uint32_t h = FNV_BASIS;
    for (size_t i = 0; id && i < SAFE_MODE_ID_CAP && id[i]; i++) {
        h = (h ^ (uint8_t)id[i]) * FNV_PRIME;
    }
    return h <= SAFE_MODE_NOBODY ? h + 2u : h; // never CLEAN or NOBODY
}

void safe_mode_note_rearm(safe_mode_rec_t *r, bool restarting) {
    safe_mode_ensure(r);
    r->flags = restarting ? r->flags | SAFE_MODE_F_REARM : r->flags & ~SAFE_MODE_F_REARM;
    safe_mode_seal(r);
}

static bool any_crash(safe_mode_rec_t const *r) {
    for (size_t i = 0; i < SAFE_MODE_WINDOW; i++) {
        if (r->window[i] != SAFE_MODE_CLEAN) {
            return true;
        }
    }
    return false;
}

bool safe_mode_degraded(safe_mode_rec_t const *r) {
    return safe_mode_valid(r) && any_crash(r);
}

static void push(safe_mode_rec_t *r, uint32_t entry) {
    for (size_t i = SAFE_MODE_WINDOW - 1; i > 0; i--) {
        r->window[i] = r->window[i - 1];
    }
    r->window[0] = entry;
}

static unsigned count_of(safe_mode_rec_t const *r, uint32_t entry) {
    unsigned n = 0;
    for (size_t i = 0; i < SAFE_MODE_WINDOW; i++) {
        n += r->window[i] == entry;
    }
    return n;
}

// The panic handler's mark is a crash even when the reset reason did not say so: it ran, so the boot died. The re-arm
// restart is a software restart the OS made because of the crash before it: that pair is one crash, so its tail takes
// no place in the window (it is neither a crash nor a clean boot that would dilute the count).
safe_mode_boot_t safe_mode_boot(safe_mode_rec_t *r, bool crashed) {
    safe_mode_boot_t out = {0};
    safe_mode_ensure(r);
    bool panic = (r->flags & SAFE_MODE_F_PANIC) != 0;
    bool rearm = (r->flags & SAFE_MODE_F_REARM) != 0;
    out.crash  = crashed || panic;
    if (out.crash && panic && safe_mode_id_ok(r->crashed)) {
        memcpy(out.app, r->crashed, sizeof(out.app));
    }
    r->flags = 0;
    memset(r->crashed, 0, sizeof(r->crashed));
    out.counted = out.crash || !rearm;
    if (out.counted) {
        uint32_t entry = !out.crash ? SAFE_MODE_CLEAN : out.app[0] ? safe_mode_hash(out.app) : SAFE_MODE_NOBODY;
        push(r, entry);
        if (out.app[0]) {
            out.app_crashes   = count_of(r, entry);
            out.limit_reached = out.app_crashes >= SAFE_MODE_LIMIT;
        }
    }
    out.degraded = any_crash(r);
    safe_mode_seal(r);
    return out;
}

bool safe_mode_switch_off(safe_mode_boot_t const *b, char const *id, bool may_switch_off) {
    return b->limit_reached && may_switch_off && safe_mode_id_ok(id) && strcmp(b->app, id) == 0;
}

bool safe_mode_may_switch_off(bool has_background, bool exempt) {
    return has_background && !exempt;
}

void safe_mode_forget(safe_mode_rec_t *r, char const *id) {
    safe_mode_ensure(r);
    uint32_t h = safe_mode_hash(id);
    for (size_t i = 0; i < SAFE_MODE_WINDOW; i++) {
        if (r->window[i] == h) {
            r->window[i] = SAFE_MODE_CLEAN;
        }
    }
    safe_mode_seal(r);
}

// ---- the list ------------------------------------------------------------------------------------------------------

// The next entry of list from *pos: copied into id (cut entries are malformed and come back empty); false at the end
static bool next_entry(char const *list, size_t cap, size_t *pos, char id[SAFE_MODE_LIST_MAX]) {
    size_t p = *pos;
    if (p >= cap || list[p] == '\0') {
        return false;
    }
    size_t n = 0;
    for (; p < cap && list[p] && list[p] != ','; p++) {
        if (n < SAFE_MODE_LIST_MAX - 1) {
            id[n++] = list[p];
        }
    }
    id[n] = '\0';
    *pos  = p < cap && list[p] == ',' ? p + 1 : p;
    return true;
}

bool safe_mode_list_has(char const *list, char const *id) {
    char   entry[SAFE_MODE_LIST_MAX];
    size_t pos = 0;
    if (!list || !safe_mode_id_ok(id)) {
        return false;
    }
    while (next_entry(list, SAFE_MODE_LIST_MAX, &pos, entry)) {
        if (strcmp(entry, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool append(char *out, size_t cap, size_t *len, char const *id) {
    size_t n    = strlen(id);
    size_t need = (*len ? 1 : 0) + n;
    if (*len + need >= cap) {
        return false;
    }
    if (*len) {
        out[(*len)++] = ',';
    }
    memcpy(out + *len, id, n);
    *len += n;
    out[*len] = '\0';
    return true;
}

// list rewritten well-formed into itself without drop (NULL: none) and with add (NULL: none); *had: drop was in it.
// False when add did not fit (list untouched then).
static bool rebuild(char *list, size_t cap, char const *drop, char const *add, bool *had) {
    char   out[SAFE_MODE_LIST_MAX] = "";
    char   entry[SAFE_MODE_LIST_MAX];
    size_t len = 0, pos = 0;
    *had       = false;
    while (next_entry(list, cap, &pos, entry)) {
        bool is_drop = drop && strcmp(entry, drop) == 0;
        *had |= is_drop;
        if (!is_drop && safe_mode_id_ok(entry) && !safe_mode_list_has(out, entry) && !(add && strcmp(entry, add) == 0)) {
            append(out, cap, &len, entry); // what was in fits again: the rewrite is never longer
        }
    }
    if (add && !append(out, cap, &len, add)) {
        return false;
    }
    memcpy(list, out, len + 1);
    return true;
}

bool safe_mode_list_add(char *list, size_t cap, char const *id) {
    bool had;
    if (!list || cap == 0 || cap > SAFE_MODE_LIST_MAX || !safe_mode_id_ok(id)) {
        return false;
    }
    return rebuild(list, cap, NULL, id, &had);
}

bool safe_mode_list_remove(char *list, size_t cap, char const *id) {
    bool had = false;
    if (!list || cap == 0 || cap > SAFE_MODE_LIST_MAX || !safe_mode_id_ok(id)) {
        return false;
    }
    rebuild(list, cap, id, NULL, &had);
    return had;
}
