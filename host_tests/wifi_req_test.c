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

// The task's side, as handle_scan and handle_apply run it: plan, work, answer
static bool task_step(wifi_req_t *r, uint32_t seq, wifi_req_why_t gate, int *ran) {
    wifi_req_plan_t p = wifi_req_plan(r, seq, gate);
    if (p.run) {
        (*ran)++;
    }
    if (p.answer) {
        wifi_req_finish(r, seq, p.run, p.run ? WIFI_REQ_OK : p.why);
    }
    return p.answer;
}

static void test_plan(void) {
    wifi_req_t     r;
    bool           ok;
    wifi_req_why_t why;
    int            ran = 0;
    wifi_req_init(&r);
    // refused during a join: not run, answered with the reason
    uint32_t a = wifi_req_begin(&r);
    CHECK(task_step(&r, a, wifi_scan_gate(true, false, true, true, WIFI_JOIN_TRYING, false), &ran));
    CHECK(ran == 0 && wifi_req_result(&r, a, &ok, &why) && !ok && why == WIFI_REQ_JOINING);
    uint32_t b = wifi_req_begin(&r);
    CHECK(task_step(&r, b, wifi_link_gate(WIFI_JOIN_PENDING, false), &ran) && ran == 0);
    CHECK(wifi_req_result(&r, b, &ok, &why) && why == WIFI_REQ_JOINING);
    uint32_t c = wifi_req_begin(&r);
    CHECK(task_step(&r, c, wifi_link_gate(WIFI_JOIN_NONE, true), &ran) && ran == 0);
    CHECK(wifi_req_result(&r, c, &ok, &why) && why == WIFI_REQ_ROAMING);
    // runs when clear
    uint32_t d = wifi_req_begin(&r);
    CHECK(task_step(&r, d, wifi_link_gate(WIFI_JOIN_JOINED, false), &ran) && ran == 1);
    CHECK(wifi_req_result(&r, d, &ok, &why) && ok);
    // a timeout followed by a retry: the first message is still queued, the second owns the slot. The task skips the
    // first (no work, no answer) and runs the second once
    uint32_t e = wifi_req_begin(&r);
    uint32_t f = wifi_req_begin(&r);
    CHECK(!task_step(&r, e, WIFI_REQ_OK, &ran) && ran == 1);
    CHECK(task_step(&r, f, WIFI_REQ_OK, &ran) && ran == 2);
    CHECK(wifi_req_result(&r, f, &ok, &why) && ok);
    // a request is answered once: a repeated message of the same number is skipped
    CHECK(!task_step(&r, f, WIFI_REQ_OK, &ran) && ran == 2);
    CHECK(wifi_link_gate(WIFI_JOIN_FAILED_KEPT_OLD, false) == WIFI_REQ_OK);
}

// A choice changes only when the task ran it and the driver took it (the stored value is what later applies use)
static void test_keep(void) {
    wifi_req_t r;
    wifi_req_init(&r);
    int      stored = 'x';
    int      ran    = 0;
    uint32_t a      = wifi_req_begin(&r);
    wifi_req_plan_t p = wifi_req_plan(&r, a, wifi_link_gate(WIFI_JOIN_TRYING, false)); // refused by the gate
    CHECK(!p.run && wifi_req_keep(p.run, stored, 'n') == 'x');
    uint32_t b = wifi_req_begin(&r);
    (void)wifi_req_plan(&r, b, WIFI_REQ_OK);
    uint32_t c = wifi_req_begin(&r); // b's caller gave up
    p = wifi_req_plan(&r, b, WIFI_REQ_OK);
    CHECK(!p.run && !p.answer && wifi_req_keep(p.run, stored, 'n') == 'x');
    p = wifi_req_plan(&r, c, WIFI_REQ_OK);
    CHECK(p.run);
    stored = wifi_req_keep(false, stored, 'n'); // the driver said no
    CHECK(stored == 'x');
    stored = wifi_req_keep(true, stored, 'n'); // accepted
    CHECK(stored == 'n');
    (void)ran;
    CHECK(wifi_req_why_text(WIFI_REQ_BAD_ARG)[0] != '\0');
}

int main(void) {
    test_keep();
    test_plan();
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
