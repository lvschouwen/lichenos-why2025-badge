// Host test: the advert table in PSRAM and its snapshot (#469)
#include "mesh_advtab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CAP 6

static mesh_advtab_t t;
static void         *mem;

static void fresh(size_t cap) {
    free(mem);
    mem = malloc(mesh_advtab_bytes(cap));
    mesh_advtab_init(&t, mem, cap);
}

static void key_of(uint8_t *key, uint8_t n) {
    memset(key, 0, 32);
    key[0] = n;
    key[7] = (uint8_t)(n ^ 0x5a);
}

static bool put_n(uint8_t n, uint8_t fill, size_t len) {
    uint8_t key[32], data[MESH_ADVTAB_DATA];
    key_of(key, n);
    memset(data, fill, sizeof(data));
    return mesh_advtab_put(&t, key, 32, data, len);
}

static uint8_t get_n(uint8_t n, uint8_t *out) {
    uint8_t key[32];
    key_of(key, n);
    return mesh_advtab_get(&t, key, 32, out);
}

static void put_reads_back_and_replaces(void) {
    uint8_t out[MESH_ADVTAB_DATA];
    fresh(CAP);
    CHECK(get_n(1, out) == 0);
    CHECK(put_n(1, 0xa1, 100));
    CHECK(get_n(1, out) == 100 && out[0] == 0xa1 && out[99] == 0xa1);
    uint32_t before = t.changes;
    CHECK(put_n(1, 0xb2, 255)); // the same node again: one entry, the newer packet
    CHECK(t.count == 1 && t.changes != before);
    CHECK(get_n(1, out) == 255 && out[254] == 0xb2);
    // Only the first 8 bytes name it
    uint8_t other[32];
    key_of(other, 1);
    other[20] = 0xff;
    CHECK(mesh_advtab_get(&t, other, 32, out) == 255);
}

static void refuses_bad_sizes_and_a_full_table(void) {
    uint8_t key[32], data[MESH_ADVTAB_DATA + 1] = {0};
    fresh(CAP);
    key_of(key, 1);
    CHECK(!mesh_advtab_put(&t, key, 7, data, 120));
    CHECK(!mesh_advtab_put(&t, key, 32, data, MESH_ADVTAB_MIN - 1));
    CHECK(!mesh_advtab_put(&t, key, 32, data, MESH_ADVTAB_DATA + 1));
    CHECK(t.count == 0 && t.refused == 0);
    for (uint8_t n = 1; n <= CAP; n++) {
        CHECK(put_n(n, n, 110));
    }
    CHECK(!put_n(CAP + 1, 9, 110) && t.refused == 1);
    CHECK(put_n(3, 0x33, 120)); // a known node still updates when full
    CHECK(t.count == CAP);
}

static void removed_slot_is_used_again(void) {
    uint8_t key[32], out[MESH_ADVTAB_DATA];
    fresh(CAP);
    for (uint8_t n = 1; n <= CAP; n++) {
        put_n(n, n, 110);
    }
    key_of(key, 2);
    CHECK(mesh_advtab_remove(&t, key, 32));
    CHECK(!mesh_advtab_remove(&t, key, 32));
    CHECK(get_n(2, out) == 0 && t.count == CAP - 1);
    CHECK(put_n(9, 0x99, 130));
    CHECK(get_n(9, out) == 130 && t.count == CAP);
    mesh_advtab_clear(&t);
    CHECK(t.count == 0 && get_n(9, out) == 0 && put_n(9, 1, 100));
}

static bool keep_odd(uint8_t const key[MESH_ADVTAB_KEY], void *ctx) {
    (*(int *)ctx)++;
    return key[0] & 1;
}

static void prune_drops_unknown_nodes(void) {
    uint8_t out[MESH_ADVTAB_DATA];
    int     asked = 0;
    fresh(CAP);
    for (uint8_t n = 1; n <= 5; n++) {
        put_n(n, n, 110);
    }
    CHECK(mesh_advtab_prune(&t, keep_odd, &asked) == 2 && asked == 5);
    CHECK(t.count == 3 && get_n(2, out) == 0 && get_n(4, out) == 0 && get_n(5, out) == 110);
}

static void snapshot_round_trip(void) {
    uint8_t buf[4096], out[MESH_ADVTAB_DATA];
    fresh(CAP);
    put_n(1, 0x11, 100);
    put_n(2, 0x22, 255);
    put_n(3, 0x33, 140);
    uint8_t key[32];
    key_of(key, 2);
    mesh_advtab_remove(&t, key, 32); // a hole is not written
    size_t n = mesh_advtab_pack_size(&t);
    CHECK(n == MESH_ADVTAB_HEAD + 2 * MESH_ADVTAB_REC + 100 + 140);
    CHECK(mesh_advtab_pack(&t, 7, buf, n - 1) == 0);
    CHECK(mesh_advtab_pack(&t, 7, buf, sizeof(buf)) == n);
    uint32_t gen = 0;
    CHECK(!mesh_advtab_valid(buf, n, &gen)); // not sealed yet
    mesh_advtab_seal(buf, n);
    CHECK(mesh_advtab_valid(buf, n, &gen) && gen == 7);

    fresh(CAP);
    put_n(8, 0x88, 100); // what the table held before a load goes
    CHECK(mesh_advtab_unpack(&t, buf, n) == 2);
    CHECK(get_n(1, out) == 100 && out[50] == 0x11);
    CHECK(get_n(3, out) == 140 && out[139] == 0x33);
    CHECK(get_n(8, out) == 0 && get_n(2, out) == 0);

    fresh(1); // a smaller table (no card): what fits is loaded
    CHECK(mesh_advtab_unpack(&t, buf, n) == 1);

    fresh(CAP); // an empty table is a valid snapshot too
    n = mesh_advtab_pack(&t, 8, buf, sizeof(buf));
    mesh_advtab_seal(buf, n);
    CHECK(n == MESH_ADVTAB_HEAD && mesh_advtab_valid(buf, n, &gen) && gen == 8);
}

static void damaged_snapshots_are_refused(void) {
    uint8_t buf[4096], copy[4096];
    uint32_t gen;
    fresh(CAP);
    put_n(1, 0x11, 100);
    put_n(2, 0x22, 150);
    size_t n = mesh_advtab_pack(&t, 3, buf, sizeof(buf));
    mesh_advtab_seal(buf, n);
    CHECK(mesh_advtab_valid(buf, n, &gen));
    CHECK(!mesh_advtab_valid(buf, n - 1, &gen)); // cut short
    CHECK(!mesh_advtab_valid(buf, n + 1, &gen)); // trailing bytes
    CHECK(!mesh_advtab_valid(buf, MESH_ADVTAB_HEAD - 1, &gen));
    CHECK(!mesh_advtab_valid(buf, 0, &gen));
    for (size_t i = 0; i < n; i++) { // any flipped byte but the generation's shows
        if (i >= 20 && i < 24) {
            continue;
        }
        if (i == 6 || i == 7) {
            continue; // reserved
        }
        memcpy(copy, buf, n);
        copy[i] ^= 0x40;
        CHECK(!mesh_advtab_valid(copy, n, &gen));
    }
    // A record length that lies, with a checksum that agrees: the record walk refuses it
    memcpy(copy, buf, n);
    copy[MESH_ADVTAB_HEAD + MESH_ADVTAB_KEY] = 250;
    mesh_advtab_seal(copy, n);
    CHECK(!mesh_advtab_valid(copy, n, &gen));
    put_n(5, 5, 100);
    CHECK(mesh_advtab_unpack(&t, copy, n) == 0 && t.count == 0); // a bad snapshot: an empty table
}

static void due_and_names(void) {
    mesh_advtab_clock_t c = {.changed = false, .on_card = true, .now_s = 100000, .last_try_s = 0};
    CHECK(!mesh_advtab_due(&c)); // nothing changed: never
    c.changed    = true;
    c.last_try_s = 1000;
    c.now_s      = 1000 + MESH_ADVTAB_CARD_S - 1;
    CHECK(!mesh_advtab_due(&c));
    c.now_s = 1000 + MESH_ADVTAB_CARD_S;
    CHECK(mesh_advtab_due(&c));
    c.on_card = false; // no card: the long interval
    CHECK(!mesh_advtab_due(&c));
    c.now_s = 1000 + MESH_ADVTAB_INTERNAL_S;
    CHECK(mesh_advtab_due(&c));
    // A card that fails every 15 minutes: /storage steps in, but only every 6 hours
    c.internal_written = true;
    c.last_internal_s  = c.now_s - 100;
    CHECK(!mesh_advtab_internal_ok(&c, false) && mesh_advtab_internal_ok(&c, true));
    CHECK(!mesh_advtab_due(&c)); // and without a card it is not even tried
    c.last_internal_s = c.now_s - MESH_ADVTAB_INTERNAL_S;
    CHECK(mesh_advtab_internal_ok(&c, false) && mesh_advtab_due(&c));
    // The takeover's first snapshot: at once after boot's minute, and a failed one waits a minute, not a loop (review H1)
    mesh_advtab_clock_t f = {.changed = true, .forced = true, .now_s = 59, .last_try_s = 0};
    CHECK(!mesh_advtab_due(&f));
    f.now_s = 60;
    CHECK(mesh_advtab_due(&f) && mesh_advtab_internal_ok(&f, false));
    f.last_try_s = 60; // it failed at 60
    f.now_s      = 61;
    CHECK(!mesh_advtab_due(&f));
    f.now_s = 120;
    CHECK(mesh_advtab_due(&f));
    f.changed = false;
    CHECK(!mesh_advtab_due(&f));
    uint8_t key[MESH_ADVTAB_KEY];
    CHECK(mesh_advtab_key_from_name("0123456789abCDEF", key) && key[0] == 0x01 && key[7] == 0xef);
    CHECK(!mesh_advtab_key_from_name("0123456789abCDE", key));
    CHECK(!mesh_advtab_key_from_name("0123456789abCDEFF", key));
    CHECK(!mesh_advtab_key_from_name("0123456789abCDEG", key));
    CHECK(!mesh_advtab_key_from_name("adverts.bin", key));
}

typedef struct {
    char const *names[64];
    size_t      count, pos, calls_next, reads;
    int         locked, lock_errors;
    size_t      budget;       // names a step may handle before spent says so; 0: no limit
    size_t      handled;      // names since the step began (next calls count)
    size_t      read_len;     // what read returns; 0: MESH_ADVTAB_MIN
    bool        read_error;   // read fails with MESH_ADVTAB_READ_ERROR for the second file
    bool        read_zero;    // read returns 0
    char const *race_name;    // read puts this node into the table first (an advert arrived mid-read); NULL: none
    bool        io_error;     // the end of the folder is an error
} fake_dir_t;

static void fake_lock(void *ctx) {
    ((fake_dir_t *)ctx)->locked++;
}

static void fake_unlock(void *ctx) {
    ((fake_dir_t *)ctx)->locked--;
}

static char const *fake_next(void *ctx) {
    fake_dir_t *d = ctx;
    d->calls_next++;
    d->lock_errors += d->locked != 0; // never walks the folder under the lock
    return d->pos < d->count ? d->names[d->pos++] : NULL;
}

static size_t fake_read(void *ctx, char const *name, uint8_t *data) {
    fake_dir_t *d = ctx;
    (void)name;
    d->reads++;
    d->lock_errors += d->locked != 0;
    uint8_t key[MESH_ADVTAB_KEY], newer[MESH_ADVTAB_DATA];
    if (d->race_name && strcmp(d->race_name, name) == 0 && mesh_advtab_key_from_name(name, key)) {
        memset(newer, 0xee, sizeof(newer));
        mesh_advtab_put(&t, key, sizeof(key), newer, 150); // the newer advert, in while the file is read
    }
    if (d->read_error && d->reads == 2) {
        return MESH_ADVTAB_READ_ERROR;
    }
    if (d->read_zero) {
        return 0;
    }
    size_t n = d->read_len ? d->read_len : MESH_ADVTAB_MIN;
    memset(data, 0x5a, n);
    return n;
}

static bool fake_spent(void *ctx) {
    fake_dir_t *d = ctx;
    return d->budget && ++d->handled >= d->budget;
}

static bool fake_end_clean(void *ctx) {
    return !((fake_dir_t *)ctx)->io_error;
}

static void fill_names(fake_dir_t *d, size_t first, size_t last) {
    static char store[64][17];
    d->count = 0;
    for (size_t n = first; n <= last; n++) {
        snprintf(store[n], sizeof(store[n]), "%016zx", n);
        d->names[d->count++] = store[n];
    }
}

static void budget_stops_a_step_and_the_walk_resumes(void) {
    fresh(32);
    fake_dir_t d = {.budget = 2};
    fill_names(&d, 1, 7);
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, fake_spent, fake_end_clean};
    mesh_advtab_walk_t     w   = {0};
    size_t                 steps = 0;
    while (!mesh_advtab_walk_step(&t, &w, &ops, &d, 16)) {
        steps++;
        CHECK(d.calls_next == steps * 2); // 2 names per step although 16 were allowed
        d.handled = 0;
        CHECK(steps < 10);
    }
    CHECK(w.done && !w.failed && steps == 3 && w.taken_over == 7 && t.count == 7);
    // A budget already gone still lets one name through
    fresh(32);
    fake_dir_t one = {.budget = 1};
    fill_names(&one, 1, 3);
    mesh_advtab_walk_t w1 = {0};
    CHECK(!mesh_advtab_walk_step(&t, &w1, &ops, &one, 16) && w1.names == 1 && w1.taken_over == 1);
}

static void a_newer_advert_during_the_read_is_kept(void) {
    uint8_t got[MESH_ADVTAB_DATA], key[MESH_ADVTAB_KEY];
    fresh(32);
    fake_dir_t d = {0};
    fill_names(&d, 1, 3);
    d.race_name = d.names[1];
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, NULL};
    mesh_advtab_walk_t     w   = {0};
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16) && w.done);
    CHECK(mesh_advtab_key_from_name(d.race_name, key));
    CHECK(mesh_advtab_get(&t, key, sizeof(key), got) == 150 && got[0] == 0xee); // the advert's, not the file's
    CHECK(w.taken_over == 2 && t.count == 3 && d.locked == 0 && d.lock_errors == 0);
}

static void unreadable_and_short_files_add_nothing(void) {
    fresh(32);
    fake_dir_t d = {.read_zero = true};
    fill_names(&d, 1, 3);
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, NULL};
    mesh_advtab_walk_t     w   = {0};
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16) && w.done);
    CHECK(d.reads == 3 && w.taken_over == 0 && t.count == 0);
    fresh(32);
    fake_dir_t s = {.read_len = MESH_ADVTAB_MIN - 1};
    fill_names(&s, 1, 3);
    memset(&w, 0, sizeof(w));
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &s, 16) && w.done);
    CHECK(s.reads == 3 && w.taken_over == 0 && t.count == 0 && t.refused == 0);
}

static void a_full_table_takes_no_more(void) {
    fresh(2);
    fake_dir_t d = {0};
    fill_names(&d, 1, 5);
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, NULL};
    mesh_advtab_walk_t     w   = {0};
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16) && w.done);
    CHECK(w.taken_over == 2 && t.count == 2 && t.refused == 3);
}

static void a_failed_file_read_keeps_the_old_files(void) {
    fresh(32);
    fake_dir_t d = {.read_error = true};
    fill_names(&d, 1, 3);
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, NULL};
    mesh_advtab_walk_t     w   = {0};
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16));
    CHECK(!w.done && w.failed && w.read_errors == 1 && w.taken_over == 2); // the others are still taken
}

static void an_io_error_is_not_the_end(void) {
    fresh(32);
    fake_dir_t d = {.io_error = true};
    fill_names(&d, 1, 2);
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, fake_end_clean};
    mesh_advtab_walk_t     w   = {0};
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16));
    CHECK(!w.done && w.failed && w.taken_over == 2);
    size_t before = d.calls_next;
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 16) && d.calls_next == before && !w.done); // not retried
}

static void takeover_walks_in_slices(void) {
    static char const *names[] = {".",  "..", "0000000000000001", "0000000000000002", "junk.txt", "0000000000000003",
                                  "0000000000000004", "0000000000000005", "0000000000000006", "0000000000000007"};
    fresh(32);
    fake_dir_t d = {.count = sizeof(names) / sizeof(names[0])};
    memcpy(d.names, names, sizeof(names));
    uint8_t key[MESH_ADVTAB_KEY], got[MESH_ADVTAB_DATA] = {0};
    CHECK(mesh_advtab_key_from_name("0000000000000003", key)); // the snapshot already has node 3
    CHECK(mesh_advtab_put(&t, key, sizeof(key), got, MESH_ADVTAB_MIN + 1));
    uint32_t           had = t.count;
    mesh_advtab_walk_ops_t ops = {fake_next, fake_read, fake_lock, fake_unlock, NULL, NULL};
    mesh_advtab_walk_t w   = {0};
    size_t             calls = 0;
    while (!mesh_advtab_walk_step(&t, &w, &ops, &d, 3)) {
        calls++;
        CHECK(!w.done);
        CHECK(d.calls_next == calls * 3); // never more than 3 names per call; the cursor kept its place
        CHECK(calls < 10);
    }
    CHECK(w.done && calls == 3);          // 10 names: 3 + 3 + 3, then the call that finds the end
    CHECK(w.names == 10);
    CHECK(w.taken_over == 6 && d.reads == 6); // nodes 1 2 4 5 6 7 once each; 3 was in the table, junk and dots skipped
    CHECK(t.count == had + 6 && d.locked == 0 && d.lock_errors == 0);
    size_t before = d.calls_next;
    CHECK(mesh_advtab_walk_step(&t, &w, &ops, &d, 3) && d.calls_next == before); // done stays done
    CHECK(mesh_advtab_get(&t, key, sizeof(key), got) == MESH_ADVTAB_MIN + 1); // the snapshot's copy was kept
}

int main(void) {
    budget_stops_a_step_and_the_walk_resumes();
    a_newer_advert_during_the_read_is_kept();
    unreadable_and_short_files_add_nothing();
    a_full_table_takes_no_more();
    a_failed_file_read_keeps_the_old_files();
    an_io_error_is_not_the_end();
    takeover_walks_in_slices();
    put_reads_back_and_replaces();
    refuses_bad_sizes_and_a_full_table();
    removed_slot_is_used_again();
    prune_drops_unknown_nodes();
    snapshot_round_trip();
    damaged_snapshots_are_refused();
    due_and_names();
    free(mem);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_advtab_test: ok\n");
    return 0;
}
