// Host test: the sweep of remembered passwords whose contact is gone (#413), over a fake store and contact table.
#include "mesh_pw_sweep.h"

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

static char keys[40][APP_SECRET_KEY_BUF];
static int  nkeys;
static pw_contact_t state_for[256]; // by the first prefix byte
static int  erase_fail_at = -1, erases;

static size_t list(void *env, char (*out)[APP_SECRET_KEY_BUF], size_t cap, size_t skip, bool *more) {
    (void)env;
    size_t have = (size_t)nkeys > skip ? (size_t)nkeys - skip : 0;
    size_t n    = have < cap ? have : cap;
    for (size_t i = 0; i < n; i++) {
        memcpy(out[i], keys[skip + i], APP_SECRET_KEY_BUF);
    }
    *more = have > n;
    return n;
}
static pw_contact_t contact(void *env, uint8_t const p[MF_PREFIX]) {
    (void)env;
    return state_for[p[0]];
}
static bool erase(void *env, char const *key) {
    (void)env;
    if (erases++ == erase_fail_at) {
        return false;
    }
    for (int i = 0; i < nkeys; i++) {
        if (!strcmp(keys[i], key)) {
            memmove(keys[i], keys[i + 1], (size_t)(nkeys - i - 1) * APP_SECRET_KEY_BUF);
            nkeys--;
            return true;
        }
    }
    return true;
}
static pw_sweep_io_t const io = {NULL, list, contact, erase};

static void reset(void) {
    nkeys = 0;
    erases = 0;
    erase_fail_at = -1;
    for (int i = 0; i < 256; i++) {
        state_for[i] = PW_CONTACT_THERE;
    }
}
static void add(char const *k) {
    snprintf(keys[nkeys++], APP_SECRET_KEY_BUF, "%s", k);
}

static void only_gone_contacts_lose_their_secret(void) {
    reset();
    add("pw0a0000000000");
    add("pw0b0000000000");
    add("pw0c0000000000");
    state_for[0x0b] = PW_CONTACT_GONE;
    state_for[0x0c] = PW_CONTACT_UNKNOWN; // a busy table never counts as gone
    size_t left = 99;
    CHECK(pw_sweep(&io, true, &left, NULL) == 1 && left == 2 && nkeys == 2);
    CHECK(!strcmp(keys[0], "pw0a0000000000") && !strcmp(keys[1], "pw0c0000000000"));
    CHECK(pw_sweep(&io, true, NULL, NULL) == 0); // nothing more to drop
}

static void odd_keys_and_failures_are_left_alone(void) {
    reset();
    add("pw0A0000000000"); // capital hex: not made by rr_pw_key
    add("pwshort");
    add("pw0d0000000000");
    state_for[0x0a] = state_for[0x0d] = PW_CONTACT_GONE;
    erase_fail_at = 0; // the first erase fails (only the parsable key reaches it)
    size_t left = 0;
    CHECK(pw_sweep(&io, true, &left, NULL) == 0 && left == 3 && nkeys == 3 && erases == 1);
    CHECK(pw_sweep(&io, true, &left, NULL) == 1 && left == 2 && nkeys == 2); // the retry works, the odd ones stay
}

static void a_long_namespace_is_walked_in_batches(void) {
    reset();
    for (int i = 0; i < 20; i++) {
        char k[APP_SECRET_KEY_BUF];
        snprintf(k, sizeof(k), "pw%02x0000000000", i);
        add(k);
        state_for[i] = PW_CONTACT_GONE;
    }
    CHECK(pw_sweep(&io, true, NULL, NULL) == 20 && nkeys == 0); // more than one batch
}

// #413 review: 16 live passwords fill the first batch, the orphan behind them is still reached
static void an_orphan_behind_a_full_batch_is_dropped(void) {
    reset();
    for (int i = 0; i < PW_SWEEP_BATCH + 1; i++) {
        char k[APP_SECRET_KEY_BUF];
        snprintf(k, sizeof(k), "pw%02x0000000000", i);
        add(k);
    }
    state_for[PW_SWEEP_BATCH] = PW_CONTACT_GONE;
    size_t left = 0;
    CHECK(pw_sweep(&io, true, &left, NULL) == 1 && left == PW_SWEEP_BATCH && nkeys == PW_SWEEP_BATCH);
}

// A table that may be incomplete, or a busy one: nothing is erased, however the contacts read
static void untrusted_or_unknown_erases_nothing(void) {
    reset();
    for (int i = 0; i < 20; i++) {
        char k[APP_SECRET_KEY_BUF];
        snprintf(k, sizeof(k), "pw%02x0000000000", i);
        add(k);
        state_for[i] = PW_CONTACT_GONE;
    }
    CHECK(pw_sweep(&io, false, NULL, NULL) == 0 && nkeys == 20 && erases == 0);
    for (int i = 0; i < 256; i++) {
        state_for[i] = PW_CONTACT_UNKNOWN;
    }
    size_t left = 0;
    bool stopped = false;
    CHECK(pw_sweep(&io, true, &left, &stopped) == 0 && stopped && nkeys == 20 && erases == 0 && left == PW_SWEEP_BATCH);
    stopped = true;
    reset();
    add("pw0a0000000000");
    CHECK(pw_sweep(&io, true, NULL, &stopped) == 0 && !stopped); // a sweep that ran through is not "early"
}

// The first busy answer ends the sweep: later keys are not asked, whatever they would have said
static int asked;
static pw_contact_t counting_contact(void *env, uint8_t const p[MF_PREFIX]) {
    (void)env;
    asked++;
    return p[0] == 3 ? PW_CONTACT_UNKNOWN : PW_CONTACT_GONE;
}
static void a_busy_answer_stops_the_sweep(void) {
    reset();
    for (int i = 0; i < 20; i++) {
        char k[APP_SECRET_KEY_BUF];
        snprintf(k, sizeof(k), "pw%02x0000000000", i);
        add(k);
    }
    pw_sweep_io_t const busy = {NULL, list, counting_contact, erase};
    asked                    = 0;
    size_t left              = 0;
    bool stopped             = false;
    CHECK(pw_sweep(&busy, true, &left, &stopped) == 3 && stopped && asked == 4 && nkeys == 17 && left == 13); // 0..2 erased, 3 busy: stop, the rest of the batch kept
}

// A listing that claims more keys than the batch holds is not believed: nothing is erased or read past the buffer
static size_t overrun_list(void *env, char (*out)[APP_SECRET_KEY_BUF], size_t cap, size_t skip, bool *more) {
    (void)env;
    (void)skip;
    for (size_t i = 0; i < cap; i++) {
        snprintf(out[i], APP_SECRET_KEY_BUF, "pw%02x0000000000", (unsigned)i);
    }
    *more = true;
    return cap + 1;
}
static void an_overrunning_listing_is_refused(void) {
    reset();
    for (int i = 0; i < 256; i++) {
        state_for[i] = PW_CONTACT_GONE;
    }
    pw_sweep_io_t const bad = {NULL, overrun_list, contact, erase};
    CHECK(pw_sweep(&bad, true, NULL, NULL) == 0 && erases == 0);
}

static void a_complete_load_is_known_by_the_file(void) {
    CHECK(pw_load_complete(152 * 5, 5, 1000));
    CHECK(!pw_load_complete(0, 0, 1000));            // no contacts at all: nothing to judge by
    CHECK(!pw_load_complete(152 * 5 + 1, 5, 1000));  // a short last record
    CHECK(!pw_load_complete(152 * 5, 4, 1000));      // the table lost one (duplicate, bad read)
    CHECK(!pw_load_complete(152 * 5, 6, 1000));
    CHECK(!pw_load_complete(152 * 1000, 1000, 1000)); // full: the read stopped, maybe short of the file
    CHECK(!pw_load_complete(-1, 0, 1000));            // no file
}

int main(void) {
    only_gone_contacts_lose_their_secret();
    odd_keys_and_failures_are_left_alone();
    a_long_namespace_is_walked_in_batches();
    an_orphan_behind_a_full_batch_is_dropped();
    untrusted_or_unknown_erases_nothing();
    a_busy_answer_stops_the_sweep();
    an_overrunning_listing_is_refused();
    a_complete_load_is_known_by_the_file();
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
