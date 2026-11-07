#include "ui_lock_hits.h"

#include <string.h>

static void fill(ui_lock_hit_site_t *s, char const *task, char const *what) {
    strncpy(s->task, task ? task : "?", sizeof(s->task) - 1);
    s->task[sizeof(s->task) - 1] = '\0';
    s->what                      = what;
    atomic_store(&s->ready, true);
}

bool ui_lock_hits_record(ui_lock_hits_t *h, uintptr_t site, char const *task, char const *what) {
    atomic_fetch_add(&h->total, 1);
    for (size_t i = 0; i < UI_LOCK_HIT_SITES; i++) {
        ui_lock_hit_site_t *s    = &h->sites[i];
        uintptr_t           seen = atomic_load(&s->site);
        if (seen == 0) {
            uintptr_t expect = 0;
            if (atomic_compare_exchange_strong(&s->site, &expect, site)) {
                fill(s, task, what);
                atomic_fetch_add(&s->count, 1);
                return true;
            }
            seen = expect; // another task claimed it first: maybe for this very site
        }
        if (seen == site) {
            atomic_fetch_add(&s->count, 1);
            return false;
        }
    }
    return false;
}

void ui_lock_hits_reset(ui_lock_hits_t *h) {
    for (size_t i = 0; i < UI_LOCK_HIT_SITES; i++) {
        atomic_store(&h->sites[i].ready, false);
        atomic_store(&h->sites[i].count, 0);
        atomic_store(&h->sites[i].site, 0);
    }
    atomic_store(&h->total, 0);
}
