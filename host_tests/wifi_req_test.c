// The c6wifi request slot and the scan gate (#502)
#include "wifi_req.h"

#include <stdio.h>

static int failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);           \
            failed++;                                                    \
        }                                                                \
    } while (0)

static void test_slot(void) {
    wifi_req_t     r;
    bool           ok;
    wifi_req_why_t why;
    wifi_req_init(&r);
    uint32_t a = wifi_req_begin(&r);
    CHECK(a != 0);
    CHECK(!wifi_req_result(&r, a, &ok, &why)); // not answered yet
    CHECK(wifi_req_wanted(&r, a));
    CHECK(wifi_req_finish(&r, a, true, WIFI_REQ_OK));
    CHECK(wifi_req_result(&r, a, &ok, &why) && ok && why == WIFI_REQ_OK);
    CHECK(!wifi_req_wanted(&r, a));
}

// A caller that timed out starts another request: the first one's late answer is dropped and the second never sees it
static void test_stale_answer(void) {
    wifi_req_t     r;
    bool           ok;
    wifi_req_why_t why;
    wifi_req_init(&r);
    uint32_t a = wifi_req_begin(&r);
    uint32_t b = wifi_req_begin(&r);
    CHECK(a != b);
    CHECK(!wifi_req_wanted(&r, a)); // the task skips the old scan
    CHECK(!wifi_req_finish(&r, a, true, WIFI_REQ_OK));
    CHECK(!wifi_req_result(&r, b, &ok, &why));
    CHECK(!wifi_req_result(&r, a, &ok, &why));
    CHECK(wifi_req_finish(&r, b, false, WIFI_REQ_JOINING));
    CHECK(wifi_req_result(&r, b, &ok, &why) && !ok && why == WIFI_REQ_JOINING);
}

static void test_wrap(void) {
    wifi_req_t r;
    wifi_req_init(&r);
    r.seq = 0xFFFFFFFFu;
    CHECK(wifi_req_begin(&r) == 1);
    CHECK(!wifi_req_finish(&r, 0, true, WIFI_REQ_OK));
}

static void test_scan_gate(void) {
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_NONE, false) == WIFI_REQ_OK);
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_JOINED, false) == WIFI_REQ_OK);
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_FAILED_KEPT_OLD, false) == WIFI_REQ_OK);
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_TRYING, false) == WIFI_REQ_JOINING);
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_PENDING, false) == WIFI_REQ_JOINING);
    CHECK(wifi_scan_gate(true, false, true, true, WIFI_JOIN_NONE, true) == WIFI_REQ_ROAMING);
    CHECK(wifi_scan_gate(true, true, true, true, WIFI_JOIN_NONE, false) == WIFI_REQ_UPDATING);
    CHECK(wifi_scan_gate(false, false, true, true, WIFI_JOIN_NONE, false) == WIFI_REQ_NO_LINK);
    CHECK(wifi_scan_gate(true, false, false, true, WIFI_JOIN_NONE, false) == WIFI_REQ_OFF);
    CHECK(wifi_scan_gate(true, false, true, false, WIFI_JOIN_NONE, false) == WIFI_REQ_DRIVER);
    // the link and the update are the hardest reasons: they win over a join in flight
    CHECK(wifi_scan_gate(false, false, true, true, WIFI_JOIN_TRYING, true) == WIFI_REQ_NO_LINK);
    CHECK(wifi_req_why_text(WIFI_REQ_JOINING)[0] != '\0');
}

int main(void) {
    test_slot();
    test_stale_answer();
    test_wrap();
    test_scan_gate();
    if (failed) {
        return 1;
    }
    printf("wifi_req_test: ok\n");
    return 0;
}
