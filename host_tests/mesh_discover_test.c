// Host test: Discover nearby's request, answer table, rate limit and texts (#167)
#include "mesh_discover.h"

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

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: got '%s' want '%s'\n", __FILE__, __LINE__, (got), (want));                             \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void answer(uint8_t out[6 + DISC_PREFIX], uint8_t type, int8_t their_snr_x4, uint32_t tag, uint8_t key0) {
    out[0] = DISC_RESP_TYPE | type;
    out[1] = (uint8_t)their_snr_x4;
    memcpy(&out[2], &tag, 4);
    for (int i = 0; i < DISC_PREFIX; i++) {
        out[6 + i] = (uint8_t)(key0 + i);
    }
}

static void test_request(void) {
    uint8_t r[DISC_REQ_LEN];
    disc_request(0x04030201u, r);
    CHECK(r[0] == 0x81 && r[1] == 0xFF && r[2] == 1 && r[3] == 2 && r[4] == 3 && r[5] == 4);
}

static void test_rate_limit(void) {
    static disc_t d;
    char const   *why = NULL;
    disc_init(&d);
    CHECK(disc_wait_s(&d, 5) == 0);
    CHECK(disc_start(&d, 7, 100, &why));
    CHECK(d.state == DISC_PENDING);
    CHECK(!disc_start(&d, 8, 200, &why)); // still running, however late
    disc_sent(&d, 100);
    CHECK(disc_wait_s(&d, 130) == 30);
    disc_poll(&d, 120);
    CHECK(d.state == DISC_DONE);
    CHECK(!disc_start(&d, 8, 130, &why));
    CHECK_STR(why, "asked a moment ago");
    CHECK(disc_wait_s(&d, 160) == 0);
    CHECK(disc_start(&d, 8, 160, &why));
    CHECK(d.count == 0);
}

static void test_answers(void) {
    static disc_t d;
    char const   *why;
    uint8_t       p[6 + DISC_PREFIX];
    disc_init(&d);
    disc_start(&d, 42, 10, &why);
    answer(p, 2, 24, 42, 0x10);
    CHECK(!disc_on_payload(&d, p, sizeof(p), 65, NULL, 11)); // not on air yet: not ours to take
    disc_sent(&d, 10);
    CHECK(disc_on_payload(&d, p, sizeof(p), 65, "Hill", 12));
    answer(p, 2, 8, 99, 0x20);
    CHECK(!disc_on_payload(&d, p, sizeof(p), 10, NULL, 12)); // another request's tag
    answer(p, 2, 8, 42, 0x20);
    CHECK(disc_on_payload(&d, p, sizeof(p), -35, NULL, 14));
    CHECK(!disc_on_payload(&d, p, 10, 0, NULL, 14)); // key cut short
    p[0] = 0x81;
    CHECK(!disc_on_payload(&d, p, sizeof(p), 0, NULL, 14)); // a request, not an answer
    CHECK(d.count == 2);
    // the same node again: one row, newest values
    answer(p, 2, 20, 42, 0x10);
    CHECK(disc_on_payload(&d, p, sizeof(p), 50, NULL, 16));
    CHECK(d.count == 2);
    disc_answer_t o[4];
    CHECK(disc_copy(&d, o, 4) == 2);
    CHECK(o[0].prefix[0] == 0x10 && o[0].snr_x10 == 50 && o[0].at_s == 16); // newest first
    CHECK(o[1].prefix[0] == 0x20);
    CHECK_STR(o[0].name, "Hill");
    // none after the window
    CHECK(disc_poll(&d, 24) == DISC_LISTENING);
    CHECK(disc_poll(&d, 25) == DISC_DONE);
    answer(p, 2, 20, 42, 0x30);
    CHECK(!disc_on_payload(&d, p, sizeof(p), 50, NULL, 26));
}

static void test_overflow(void) {
    static disc_t d;
    char const   *why;
    uint8_t       p[6 + DISC_PREFIX];
    disc_init(&d);
    disc_start(&d, 1, 0, &why);
    disc_sent(&d, 0);
    for (int i = 0; i < DISC_MAX + 3; i++) {
        answer(p, 2, 0, 1, (uint8_t)(i * 9));
        CHECK(disc_on_payload(&d, p, sizeof(p), 0, NULL, 1));
    }
    CHECK(d.count == DISC_MAX && d.extra == 3);
    char t[80];
    disc_poll(&d, 99);
    disc_status(&d, 99, t, sizeof(t));
    CHECK_STR(t, "Heard 27 (showing 24)");
}

static void test_texts(void) {
    static disc_t d;
    char          t[96];
    char const   *why;
    disc_init(&d);
    disc_status(&d, 0, t, sizeof(t));
    CHECK(strstr(t, "one short request") != NULL);
    disc_start(&d, 5, 100, &why);
    disc_status(&d, 100, t, sizeof(t));
    CHECK_STR(t, "Sending...");
    disc_sent(&d, 100);
    disc_status(&d, 103, t, sizeof(t));
    CHECK_STR(t, "Listening, 12 s left...");
    uint8_t p[6 + DISC_PREFIX];
    answer(p, 2, 16, 5, 0xab);
    disc_on_payload(&d, p, sizeof(p), 65, NULL, 104);
    disc_status(&d, 105, t, sizeof(t));
    CHECK_STR(t, "Listening, 10 s left. Heard 1 so far.");
    disc_answer_t a;
    disc_copy(&d, &a, 1);
    disc_row_title(&a, t, sizeof(t));
    CHECK_STR(t, "Repeater abacadae");
    disc_row_detail(&a, 114, t, sizeof(t));
    CHECK_STR(t, "Repeater, SNR 6.5 dB, hears us at 4.0 dB, 10 s ago");
    disc_poll(&d, 120);
    disc_status(&d, 120, t, sizeof(t));
    CHECK_STR(t, "Heard 1");
    // empty and failed ends
    disc_start(&d, 6, 200, &why);
    disc_sent(&d, 200);
    disc_poll(&d, 215);
    disc_status(&d, 215, t, sizeof(t));
    CHECK_STR(t, "No answers. Nobody nearby, or out of range.");
    disc_start(&d, 7, 300, &why);
    disc_failed(&d, "no free packet");
    disc_status(&d, 300, t, sizeof(t));
    CHECK_STR(t, "Not sent: no free packet. Try again.");
}

int main(void) {
    test_request();
    test_rate_limit();
    test_answers();
    test_overflow();
    test_texts();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_discover_test ok\n");
    return 0;
}
