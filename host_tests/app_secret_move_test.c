// Host test: the one-time move of an app's plain secrets into its secrets class (#288). A fake store with injected
// failures and a power cut after every single write and erase: at every point each value is still readable (plain,
// stored, or both with the same value), a later boot finishes the move, and the record ends it. Values are made up.
#include "app_secret_move.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ENTRIES 40

typedef struct {
    char key[APP_SECRET_STORED_MAX];
    char value[APP_SECRET_VALUE_MAX + 8];
    bool used;
} entry_t;

typedef struct {
    entry_t e[ENTRIES];
    bool    moved;
    int     writes_left;   // the power goes after this many writes and erases (-1: never)
    bool    dead;          // after the cut nothing happens any more
    char    fail_set[16];  // this key's write fails (the badge goes on)
    char    fail_erase[16]; // this key's erase fails
    bool    corrupt_read;  // a read-back of a stored name gives a different value
    size_t  list_cap;      // list hands out at most this many (0: as asked)
    unsigned ops;          // writes and erases done
} fake_t;

static entry_t *find(fake_t *f, char const *key) {
    for (int i = 0; i < ENTRIES; i++) {
        if (f->e[i].used && !strcmp(f->e[i].key, key)) {
            return &f->e[i];
        }
    }
    return NULL;
}

static void put(fake_t *f, char const *key, char const *value) {
    entry_t *e = find(f, key);
    for (int i = 0; !e && i < ENTRIES; i++) {
        if (!f->e[i].used) {
            e = &f->e[i];
        }
    }
    e->used = true;
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
}

// One write or erase: false when the power is gone (before it, so nothing of it lands: NVS writes are atomic)
static bool power(fake_t *f) {
    if (f->dead || f->writes_left == 0) {
        f->dead = true;
        return false;
    }
    if (f->writes_left > 0) {
        f->writes_left--;
    }
    f->ops++;
    return true;
}

static size_t f_list(void *env, char const *prefix, char (*keys)[APP_SECRET_STORED_MAX], size_t cap, bool *more) {
    fake_t *f = env;
    size_t  n = 0;
    *more     = false;
    if (f->dead) {
        return 0;
    }
    size_t lim = f->list_cap && f->list_cap < cap ? f->list_cap : cap;
    for (int i = 0; i < ENTRIES; i++) {
        if (f->e[i].used && !strncmp(f->e[i].key, prefix, strlen(prefix))) {
            if (n == lim) {
                *more = true;
                break;
            }
            snprintf(keys[n++], APP_SECRET_STORED_MAX, "%s", f->e[i].key);
        }
    }
    return n;
}

static bool f_get(void *env, char const *key, char *out, size_t cap) {
    fake_t  *f = env;
    entry_t *e = f->dead ? NULL : find(f, key);
    if (!e || strlen(e->value) >= cap) {
        return false;
    }
    strcpy(out, e->value);
    if (f->corrupt_read && key[0] == APP_SECRET_MARK) {
        out[0] ^= 1;
    }
    return true;
}

static bool f_set(void *env, char const *key, char const *value) {
    fake_t *f = env;
    if (!power(f) || !strcmp(f->fail_set, key)) {
        return false;
    }
    put(f, key, value);
    return true;
}

static bool f_erase(void *env, char const *key) {
    fake_t *f = env;
    if (!power(f) || !strcmp(f->fail_erase, key)) {
        return false;
    }
    entry_t *e = find(f, key);
    if (e) {
        e->used = false;
    }
    return true;
}

static bool f_moved_get(void *env) {
    return ((fake_t *)env)->moved;
}

static bool f_moved_set(void *env) {
    fake_t *f = env;
    if (!power(f)) {
        return false;
    }
    f->moved = true;
    return true;
}

static app_secret_store_t store_of(fake_t *f) {
    return (app_secret_store_t){f, f_list, f_get, f_set, f_erase, f_moved_get, f_moved_set};
}

static char const *const keys[]   = {"pw0123456789ab", "pwaabbccddeeff", "pw00000000000a"};
static char const *const values[] = {"made-up-one", "made-up-two", ""}; // an empty password is a value too
#define N 3

static void seed(fake_t *f) {
    memset(f, 0, sizeof(*f));
    f->writes_left = -1;
    for (int i = 0; i < N; i++) {
        put(f, keys[i], values[i]);
    }
    put(f, "autostart", "1"); // not under the prefix: never touched
}

static char const *at(fake_t *f, char const *key) {
    entry_t *e = find(f, key);
    return e ? e->value : NULL;
}

static char const *stored(fake_t *f, int i) {
    char name[APP_SECRET_STORED_MAX];
    app_secret_stored_name("pw", keys[i], name, sizeof(name));
    return at(f, name);
}

// Never both lost, never two different values
static bool every_value_readable(fake_t *f) {
    bool ok = true;
    for (int i = 0; i < N; i++) {
        char const *p = at(f, keys[i]), *s = stored(f, i);
        ok = ok && (p || s) && (!p || !strcmp(p, values[i])) && (!s || !strcmp(s, values[i]));
    }
    return ok && at(f, "autostart");
}

static bool all_moved(fake_t *f) {
    bool ok = f->moved && at(f, "autostart");
    for (int i = 0; i < N; i++) {
        ok = ok && !at(f, keys[i]) && stored(f, i) && !strcmp(stored(f, i), values[i]);
    }
    return ok;
}

static void fresh_move(void) {
    fake_t                  f;
    app_secret_store_t      s = store_of(&f);
    app_secret_move_count_t c;
    seed(&f);
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_DONE && c.moved == N && c.failed == 0);
    CHECK(all_moved(&f));
    unsigned ops = f.ops;
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_ALREADY && c.moved == 0 && f.ops == ops);
    // nothing to move at all: the record still ends it
    memset(&f, 0, sizeof(f));
    f.writes_left = -1;
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && f.moved);
    // no declaration: nothing
    seed(&f);
    CHECK(app_secret_move(&s, NULL, false, NULL) == APP_SECRET_MOVE_ALREADY && !f.moved && at(&f, keys[0]));
}

static void deferred_by_another_migration(void) {
    fake_t             f;
    app_secret_store_t s = store_of(&f);
    seed(&f);
    CHECK(app_secret_move(&s, "pw", true, NULL) == APP_SECRET_MOVE_DEFERRED && f.ops == 0 && !f.moved);
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
}

// The power goes after cut writes and erases, for every cut until one that the move survives whole
static void power_cut_at_every_step(void) {
    fake_t             f;
    app_secret_store_t s = store_of(&f);
    seed(&f);
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE);
    int const steps = (int)f.ops; // N writes, N erases, the record
    CHECK(steps == 2 * N + 1);
    for (int cut = 0; cut <= steps; cut++) {
        seed(&f);
        f.writes_left       = cut;
        app_secret_move_t r = app_secret_move(&s, "pw", false, NULL);
        CHECK(cut == steps ? r == APP_SECRET_MOVE_DONE : r == APP_SECRET_MOVE_PARTIAL);
        CHECK(every_value_readable(&f));
        CHECK(f.moved == (cut == steps));
        // the next boot finishes it, and the one after does nothing
        f.dead        = false;
        f.writes_left = -1;
        if (cut < steps) {
            CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE);
        }
        CHECK(all_moved(&f));
        CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_ALREADY);
    }
    // a cut during the second boot's run too
    for (int cut = 0; cut <= steps; cut++) {
        seed(&f);
        f.writes_left = 2; // first boot: one key written, then the power goes
        app_secret_move(&s, "pw", false, NULL);
        f.dead        = false;
        f.writes_left = cut;
        app_secret_move(&s, "pw", false, NULL);
        CHECK(every_value_readable(&f));
        f.dead        = false;
        f.writes_left = -1;
        app_secret_move(&s, "pw", false, NULL);
        CHECK(all_moved(&f));
    }
}

static void injected_failures(void) {
    fake_t                  f;
    app_secret_store_t      s = store_of(&f);
    app_secret_move_count_t c;
    // a write fails: that key stays plain, the others move, no record
    seed(&f);
    strcpy(f.fail_set, "~pwaabbccddeeff");
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_PARTIAL && c.moved == 2 && c.failed == 1);
    CHECK(every_value_readable(&f) && at(&f, keys[1]) && !stored(&f, 1) && !f.moved);
    f.fail_set[0] = '\0';
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_DONE && c.moved == 1 && all_moved(&f));
    // an erase fails: both copies stay (the same value), the next boot erases the plain one
    seed(&f);
    strcpy(f.fail_erase, keys[0]);
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_PARTIAL && c.failed == 1);
    CHECK(every_value_readable(&f) && at(&f, keys[0]) && stored(&f, 0));
    f.fail_erase[0] = '\0';
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
    // the read-back differs: the plain value is not erased
    seed(&f);
    f.corrupt_read = true;
    CHECK(app_secret_move(&s, "pw", false, &c) == APP_SECRET_MOVE_PARTIAL && c.failed == N);
    for (int i = 0; i < N; i++) {
        CHECK(at(&f, keys[i]) && !strcmp(at(&f, keys[i]), values[i]));
    }
    // the record cannot be written: everything moved, and the next boot finds nothing left and writes it
    seed(&f);
    f.writes_left = 2 * N;
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_PARTIAL && !f.moved);
    f.dead        = false;
    f.writes_left = -1;
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
}

static void plain_copy_counts_while_it_exists(void) {
    // a stored value that differs (a bad read-back last boot): written over from the plain copy, then the plain goes
    fake_t             f;
    app_secret_store_t s = store_of(&f);
    seed(&f);
    put(&f, "~pw0123456789ab", "made-up-garbled");
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
    // the corrupt read-back of injected_failures, then a clean boot: nothing lost
    seed(&f);
    f.corrupt_read = true;
    app_secret_move(&s, "pw", false, NULL);
    f.corrupt_read = false;
    // the plain copies are all still there
    CHECK(at(&f, keys[0]) && at(&f, keys[1]) && at(&f, keys[2]));
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
    // a cut right after the write: the second boot sees the same value stored and only erases
    seed(&f);
    f.writes_left = 1;
    app_secret_move(&s, "pw", false, NULL);
    f.dead        = false;
    f.writes_left = -1;
    f.ops         = 0;
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
    CHECK(f.ops == 1 + 2 * (N - 1) + 1); // key 0: the erase only; the others: write and erase; the record
}

static void more_than_a_batch(void) {
    fake_t             f;
    app_secret_store_t s = store_of(&f);
    seed(&f);
    f.list_cap = 2; // the store hands out two at a time
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_PARTIAL && !f.moved);
    CHECK(every_value_readable(&f));
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_DONE && all_moved(&f));
}

static void unmovable_key(void) {
    // a value longer than a secret may be: left plain (refused by the plain API, erased by Clear data), no record
    fake_t             f;
    app_secret_store_t s = store_of(&f);
    seed(&f);
    char longv[APP_SECRET_VALUE_MAX + 2];
    memset(longv, 'a', sizeof(longv) - 1);
    longv[sizeof(longv) - 1] = '\0';
    put(&f, "pwlong", longv);
    CHECK(app_secret_move(&s, "pw", false, NULL) == APP_SECRET_MOVE_PARTIAL && at(&f, "pwlong") && !f.moved);
}

int main(void) {
    fresh_move();
    deferred_by_another_migration();
    power_cut_at_every_step();
    injected_failures();
    plain_copy_counts_while_it_exists();
    more_than_a_batch();
    unmovable_key();
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
