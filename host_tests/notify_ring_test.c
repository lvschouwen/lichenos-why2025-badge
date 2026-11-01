// Host test: the notification history behind lichen_notify.h (#62): order, coalescing, the eviction order, marks,
// the file codec (a torn or damaged file loads empty)
#include "notify_ring.h"

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

static uint32_t add(nr_ring_t *r, char const *src, char const *tgt, uint8_t level, uint32_t mono_s, bool coalesce,
                    bool restricted, nr_outcome_t *out) {
    nr_new_t const n = {.source = src, .target = tgt, .title = "title", .body = "body", .level = level,
                        .mono_s = mono_s, .coalesce = coalesce, .restricted = restricted};
    uint32_t       id;
    nr_outcome_t   o = nr_add(r, &n, &id);
    if (out) {
        *out = o;
    }
    return id;
}

static void test_order_and_coalesce(nr_ring_t *r) {
    nr_init(r, 1);
    nr_outcome_t o;
    uint32_t     a = add(r, "app", "x", NR_INFO, 10, true, false, &o);
    CHECK(o == NR_ADDED && a != 0);
    uint32_t b = add(r, "app", "y", NR_INFO, 11, true, false, &o);
    uint32_t ids[NR_SLOTS];
    CHECK(nr_ids(r, ids, NR_SLOTS) == 2 && ids[0] == b && ids[1] == a); // newest first

    // Same (source, target), unread, within 10 min: merged, count up, back on top, highest level kept
    uint32_t c = add(r, "app", "x", NR_WARN, 10 + NR_COALESCE_S, true, false, &o);
    CHECK(o == NR_COALESCED && c == a);
    CHECK(nr_get(r, a)->count == 2 && nr_get(r, a)->level == NR_WARN);
    CHECK(nr_ids(r, ids, NR_SLOTS) == 2 && ids[0] == a);
    // Without the flag, or too late, or once read: a new entry
    CHECK(add(r, "app", "x", NR_INFO, 20 + NR_COALESCE_S, false, false, &o) != a && o == NR_ADDED);
    nr_init(r, 1);
    a = add(r, "app", "x", NR_INFO, 0, true, false, NULL);
    CHECK(add(r, "app", "x", NR_INFO, NR_COALESCE_S + 1, true, false, &o) != a && o == NR_ADDED);
    nr_init(r, 1);
    a = add(r, "app", "x", NR_INFO, 0, true, false, NULL);
    CHECK(nr_mark_read(r, "app", "x") == 1);
    CHECK(add(r, "app", "x", NR_INFO, 1, true, false, &o) != a && o == NR_ADDED);
    // NULL target and "" are the same target
    nr_init(r, 1);
    a = add(r, "app", NULL, NR_INFO, 0, true, false, NULL);
    CHECK(add(r, "app", "", NR_INFO, 1, true, false, NULL) == a);
    // nr_ids honours max
    add(r, "other", NULL, NR_INFO, 2, false, false, NULL);
    CHECK(nr_ids(r, ids, 1) == 1 && ids[0] != a);
}

static void test_eviction(nr_ring_t *r) {
    // An unread error survives 60 info notices; infos push each other out
    nr_init(r, 1);
    uint32_t err = add(r, "lichen", "crash", NR_ERROR, 0, false, false, NULL);
    for (int i = 0; i < 60; i++) {
        CHECK(add(r, "app", NULL, NR_INFO, (uint32_t)i, false, false, NULL) != 0);
    }
    CHECK(nr_get(r, err) != NULL && nr_count(r) == NR_SLOTS && r->dropped == 0);

    // Read entries go first, whatever their level
    nr_init(r, 1);
    uint32_t first = add(r, "app", NULL, NR_INFO, 0, false, false, NULL);
    uint32_t rerr  = add(r, "app", NULL, NR_ERROR, 0, false, false, NULL);
    for (int i = 2; i < NR_SLOTS; i++) {
        add(r, "app", NULL, NR_WARN, 0, false, false, NULL);
    }
    nr_mark_read_id(r, rerr);
    add(r, "app", NULL, NR_INFO, 0, false, false, NULL);
    CHECK(nr_get(r, rerr) == NULL && nr_get(r, first) != NULL);
    // Then the oldest info before any warning
    add(r, "app", NULL, NR_WARN, 0, false, false, NULL);
    CHECK(nr_get(r, first) == NULL);

    // A ring of unread errors: an info is dropped and counted, an error pushes out the oldest error
    nr_init(r, 1);
    uint32_t oldest = add(r, "lichen", NULL, NR_ERROR, 0, false, false, NULL);
    for (int i = 1; i < NR_SLOTS; i++) {
        add(r, "lichen", NULL, NR_ERROR, 0, false, false, NULL);
    }
    nr_outcome_t o;
    CHECK(add(r, "app", NULL, NR_INFO, 0, false, false, &o) == 0 && o == NR_DROPPED && r->dropped == 1);
    CHECK(add(r, "app", NULL, NR_ERROR, 0, false, false, &o) != 0 && o == NR_ADDED && nr_get(r, oldest) == NULL);

    // Over its rate limit, a source may only push out its own unread entries (or read ones)
    nr_init(r, 1);
    for (int i = 0; i < NR_SLOTS; i++) {
        add(r, "quiet", NULL, NR_INFO, 0, false, false, NULL);
    }
    CHECK(add(r, "noisy", NULL, NR_INFO, 0, false, true, &o) == 0 && o == NR_DROPPED);
    CHECK(add(r, "noisy", NULL, NR_INFO, 0, false, false, &o) != 0 && o == NR_ADDED);
    CHECK(add(r, "noisy", NULL, NR_INFO, 0, false, true, &o) != 0 && o == NR_ADDED); // its own entry made room
    CHECK(nr_unread(r, "noisy") == 1 && nr_unread(r, "quiet") == NR_SLOTS - 1);
}

static void test_queries(nr_ring_t *r) {
    nr_init(r, 1);
    CHECK(nr_unread_top(r) == -1 && nr_unread(r, NULL) == 0);
    uint32_t i = add(r, "a", "t1", NR_INFO, 0, false, false, NULL);
    uint32_t w = add(r, "a", "t2", NR_WARN, 0, false, false, NULL);
    uint32_t e = add(r, "b", NULL, NR_ERROR, 0, false, false, NULL);
    CHECK(nr_unread(r, NULL) == 3 && nr_unread(r, "a") == 2 && nr_unread(r, "b") == 1 && nr_unread(r, "c") == 0);
    CHECK(nr_unread_top(r) == NR_ERROR);
    CHECK(nr_mark_read_upto(r, NR_INFO) == 1 && nr_get(r, i)->read && !nr_get(r, w)->read);
    CHECK(nr_mark_read(r, "a", "t2") == 1 && nr_unread_top(r) == NR_ERROR);
    CHECK(nr_mark_read_id(r, e) == 1 && nr_unread_top(r) == -1);
    CHECK(nr_mark_read(r, NULL, NULL) == 0); // nothing left: no change
    uint32_t gen = r->generation;
    CHECK(nr_remove(r, w) && !nr_remove(r, w) && r->generation == gen + 1 && nr_count(r) == 2);
    nr_clear(r);
    CHECK(nr_count(r) == 0);
    // Ids are never reused within a store
    CHECK(add(r, "a", NULL, NR_INFO, 0, false, false, NULL) > e);
}

static void test_file(nr_ring_t *r) {
    static uint8_t buf[NR_FILE_MAX];
    static nr_ring_t back;
    nr_init(r, 7);
    uint32_t a = add(r, "meshcore", "ch:badgetest", NR_WARN, 5, false, false, NULL);
    uint32_t b = add(r, "lichen", "crash", NR_ERROR, 6, false, false, NULL);
    add(r, "meshcore", "ch:badgetest", NR_WARN, 7, true, false, NULL);
    nr_mark_read_id(r, b);
    size_t len = nr_encode(r, buf, sizeof(buf));
    CHECK(len == NR_HEADER_SIZE + 2 * NR_RECORD_SIZE + 4);
    CHECK(nr_encode(r, buf, len - 1) == 0); // too small: nothing
    len = nr_encode(r, buf, sizeof(buf));
    CHECK(nr_decode(&back, buf, len));
    CHECK(back.boot == 7 && nr_count(&back) == 2);
    nr_entry_t const *ea = nr_get(&back, a), *eb = nr_get(&back, b);
    CHECK(ea && eb && ea->count == 2 && ea->mono_s == 7 && !ea->read && eb->read && eb->level == NR_ERROR);
    CHECK(ea && strcmp(ea->target, "ch:badgetest") == 0 && strcmp(ea->source, "meshcore") == 0);
    uint32_t ids[NR_SLOTS];
    CHECK(nr_ids(&back, ids, NR_SLOTS) == 2 && ids[0] == a); // order survives
    // New ids continue after the file's
    CHECK(add(&back, "x", NULL, NR_INFO, 0, false, false, NULL) > b);

    // Torn (any shorter length), one flipped byte, a wrong version or magic: empty, false
    for (size_t cut = 0; cut < len; cut += 37) {
        CHECK(!nr_decode(&back, buf, cut) && nr_count(&back) == 0);
    }
    buf[NR_HEADER_SIZE + 40] ^= 0x01;
    CHECK(!nr_decode(&back, buf, len) && nr_count(&back) == 0 && back.boot == 0);
    buf[NR_HEADER_SIZE + 40] ^= 0x01;
    CHECK(nr_decode(&back, buf, len));
    buf[4] = NR_FILE_VERSION + 1;
    CHECK(!nr_decode(&back, buf, len));
    CHECK(!nr_decode(&back, NULL, 0));

    // An empty history round-trips too
    nr_init(r, 3);
    len = nr_encode(r, buf, sizeof(buf));
    CHECK(len == NR_HEADER_SIZE + 4 && nr_decode(&back, buf, len) && back.boot == 3 && nr_count(&back) == 0);
    // A full one fits NR_FILE_MAX exactly
    for (int i = 0; i < NR_SLOTS; i++) {
        add(r, "app", NULL, NR_INFO, 0, false, false, NULL);
    }
    CHECK(nr_encode(r, buf, sizeof(buf)) == NR_FILE_MAX && nr_decode(&back, buf, NR_FILE_MAX) && nr_count(&back) == NR_SLOTS);
    CHECK(nr_crc32((uint8_t const *)"123456789", 9) == 0xCBF43926u); // the standard check value
}

static void test_strings_cut(nr_ring_t *r) {
    nr_init(r, 1);
    char long_text[300];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    nr_new_t const n = {.source = long_text, .target = long_text, .title = long_text, .body = long_text};
    uint32_t       id;
    CHECK(nr_add(r, &n, &id) == NR_ADDED);
    nr_entry_t const *e = nr_get(r, id);
    CHECK(strlen(e->source) == NR_SOURCE_CAP - 1 && strlen(e->body) == NR_BODY_CAP - 1);
    CHECK(strlen(e->title) == NR_TITLE_CAP - 1 && strlen(e->target) == NR_TARGET_CAP - 1);
}

int main(void) {
    nr_ring_t *r = malloc(sizeof(*r));
    test_order_and_coalesce(r);
    test_eviction(r);
    test_queries(r);
    test_file(r);
    test_strings_cut(r);
    free(r);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notify_ring_test: ok\n");
    return 0;
}
