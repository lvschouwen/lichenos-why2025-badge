#include "app_secret_move.h"

#include <string.h>

typedef struct {
    char plain[APP_SECRET_VALUE_MAX + 1];
    char stored[APP_SECRET_VALUE_MAX + 1];
} values_t;

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

// One key: true when its plain copy is gone and the secret is in place
static bool move_one(app_secret_store_t const *s, char const *prefix, char const *key, values_t *v) {
    char name[APP_SECRET_STORED_MAX];
    if (!app_secret_stored_name(prefix, key, name, sizeof(name))) {
        return false; // too long to carry the mark: left where it is (and refused by the plain API)
    }
    if (!s->get(s->env, key, v->plain, sizeof(v->plain))) {
        return false;
    }
    // The plain copy is the one known good value while it exists: written over a stored one that differs (a read-back
    // that failed last time, or a password set after a move that failed), skipped when the same (a cut after the write)
    bool same = s->get(s->env, name, v->stored, sizeof(v->stored)) && strcmp(v->plain, v->stored) == 0;
    if (!same && (!s->set(s->env, name, v->plain) || !s->get(s->env, name, v->stored, sizeof(v->stored)) ||
                  strcmp(v->plain, v->stored) != 0)) {
        return false;
    }
    return s->erase(s->env, key);
}

app_secret_move_t app_secret_move(app_secret_store_t const *s, char const *prefix, bool other_migration,
                                  app_secret_move_count_t *count) {
    app_secret_move_count_t c = {0};
    if (count) {
        *count = c;
    }
    if (!prefix || !app_secret_prefix_ok(prefix) || s->moved_get(s->env)) {
        return APP_SECRET_MOVE_ALREADY;
    }
    if (other_migration) {
        return APP_SECRET_MOVE_DEFERRED;
    }
    char     keys[APP_SECRET_MOVE_BATCH][APP_SECRET_STORED_MAX];
    bool     more = false;
    size_t   n    = s->list(s->env, prefix, keys, APP_SECRET_MOVE_BATCH, &more);
    values_t v;
    for (size_t i = 0; i < n && i < APP_SECRET_MOVE_BATCH; i++) {
        keys[i][APP_SECRET_STORED_MAX - 1] = '\0';
        if (move_one(s, prefix, keys[i], &v)) {
            c.moved++;
        } else {
            c.failed++;
        }
        wipe(&v, sizeof(v));
    }
    if (count) {
        *count = c;
    }
    if (c.failed || more || !s->moved_set(s->env)) {
        return APP_SECRET_MOVE_PARTIAL;
    }
    return APP_SECRET_MOVE_DONE;
}
