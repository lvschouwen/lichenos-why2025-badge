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

static char keys[20][APP_SECRET_KEY_BUF];
static int  nkeys;
static pw_contact_t state_for[256]; // by the first prefix byte
static int  erase_fail_at = -1, erases;

static size_t list(void *env, char (*out)[APP_SECRET_KEY_BUF], size_t cap) {
    (void)env;
    size_t n = (size_t)nkeys < cap ? (size_t)nkeys : cap;
    for (size_t i = 0; i < n; i++) {
        memcpy(out[i], keys[i], APP_SECRET_KEY_BUF);
    }
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
    CHECK(pw_sweep(&io, &left) == 1 && left == 2 && nkeys == 2);
    CHECK(!strcmp(keys[0], "pw0a0000000000") && !strcmp(keys[1], "pw0c0000000000"));
    CHECK(pw_sweep(&io, NULL) == 0); // nothing more to drop
}

static void odd_keys_and_failures_are_left_alone(void) {
    reset();
    add("pw0A0000000000"); // capital hex: not made by rr_pw_key
    add("pwshort");
    add("pw0d0000000000");
    state_for[0x0a] = state_for[0x0d] = PW_CONTACT_GONE;
    erase_fail_at = 0; // the first erase fails (only the parsable key reaches it)
    size_t left = 0;
    CHECK(pw_sweep(&io, &left) == 0 && left == 3 && nkeys == 3 && erases == 1);
    CHECK(pw_sweep(&io, &left) == 1 && left == 2 && nkeys == 2); // the retry works, the odd ones stay
}

static void a_batch_is_bounded(void) {
    reset();
    for (int i = 0; i < 20; i++) {
        char k[APP_SECRET_KEY_BUF];
        snprintf(k, sizeof(k), "pw%02x0000000000", i);
        add(k);
        state_for[i] = PW_CONTACT_GONE;
    }
    CHECK(pw_sweep(&io, NULL) == PW_SWEEP_BATCH && nkeys == 4);
    CHECK(pw_sweep(&io, NULL) == 4 && nkeys == 0);
}

int main(void) {
    only_gone_contacts_lose_their_secret();
    odd_keys_and_failures_are_left_alone();
    a_batch_is_bounded();
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
