#include "app_secret_move.h"

#include <string.h>

typedef struct {
    char plain[APP_SECRET_VALUE_MAX + 1];
    char stored[APP_SECRET_VALUE_MAX + 1];
} values_t;

typedef enum { KEY_MOVED, KEY_DROPPED, KEY_FAILED } key_end_t;

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

// A plain key that can never be a secret goes: unreachable through the plain calls, it would only wait in flash
static key_end_t drop(app_secret_store_t const *s, char const *key) {
    return s->erase(s->env, key) ? KEY_DROPPED : KEY_FAILED;
}

static key_end_t move_one(app_secret_store_t const *s, char const *prefix, char const *key, values_t *v) {
    char name[APP_SECRET_STORED_MAX];
    if (!app_secret_stored_name(prefix, key, name, sizeof(name))) {
        return drop(s, key); // too long to carry the mark
    }
    app_secret_get_t got = s->get(s->env, key, v->plain, sizeof(v->plain));
    if (got != APP_SECRET_GET_OK) {
        return got == APP_SECRET_GET_TOO_LONG ? drop(s, key) : KEY_FAILED;
    }
    // The plain copy is the one known good value while it exists: written over a stored one that differs, skipped
    // when the same (a cut after the write)
    bool same = s->get(s->env, name, v->stored, sizeof(v->stored)) == APP_SECRET_GET_OK &&
                strcmp(v->plain, v->stored) == 0;
    if (!same && (!s->set(s->env, name, v->plain) ||
                  s->get(s->env, name, v->stored, sizeof(v->stored)) != APP_SECRET_GET_OK ||
                  strcmp(v->plain, v->stored) != 0)) {
        return KEY_FAILED;
    }
    return s->erase(s->env, key) ? KEY_MOVED : KEY_FAILED;
}

// The batch's keys, each moved, dropped or failed
static void move_batch(app_secret_store_t const *s, char const *prefix, char (*keys)[APP_SECRET_STORED_MAX], size_t n,
                       app_secret_move_count_t *c) {
    values_t v;
    for (size_t i = 0; i < n && i < APP_SECRET_MOVE_BATCH; i++) {
        keys[i][APP_SECRET_STORED_MAX - 1] = '\0';
        key_end_t end                      = move_one(s, prefix, keys[i], &v);
        c->moved += end == KEY_MOVED;
        c->dropped += end == KEY_DROPPED;
        c->failed += end == KEY_FAILED;
        wipe(&v, sizeof(v));
    }
}

app_secret_move_t app_secret_move(app_secret_store_t const *s, char const *prefix, bool other_migration,
                                  app_secret_move_count_t *count) {
    app_secret_move_count_t c = {0};
    if (count) {
        *count = c;
    }
    if (!prefix || !app_secret_prefix_ok(prefix)) {
        return APP_SECRET_MOVE_ALREADY;
    }
    if (other_migration) {
        return APP_SECRET_MOVE_DEFERRED;
    }
    char   keys[APP_SECRET_MOVE_BATCH][APP_SECRET_STORED_MAX];
    size_t n      = 0;
    bool   more   = false;
    bool   listed = s->list(s->env, prefix, keys, APP_SECRET_MOVE_BATCH, &n, &more);
    if (listed && n == 0 && !more && s->moved_get(s->env)) {
        return APP_SECRET_MOVE_ALREADY;
    }
    if (listed) {
        move_batch(s, prefix, keys, n, &c);
    } else {
        c.failed++; // keys may be left that were not seen: no record, the next boot lists again
    }
    if (count) {
        *count = c;
    }
    if (c.failed || more || !(s->moved_get(s->env) || s->moved_set(s->env))) {
        return APP_SECRET_MOVE_PARTIAL;
    }
    return APP_SECRET_MOVE_DONE;
}
