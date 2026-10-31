// Host tests for noticing a C6 reset at runtime and pacing the re-attach (#14)
#include "c6_watch.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CONTAINS(s, sub) (strstr((s), (sub)) != NULL)

static c6_watch_t fresh(void) {
    c6_watch_t w;
    c6_watch_init(&w, false, false);
    return w;
}

static void starts_up_and_quiet(void) {
    c6_watch_t w = fresh();
    CHECK(w.state == C6W_UP);
    CHECK(w.restarts == 0 && w.reason[0] == '\0');
    CHECK(c6_watch_tick(&w, 0) == 0);
    CHECK(c6_watch_tick(&w, 1000000) == 0);
    char line[200];
    c6_watch_line(&w, 0, line, sizeof(line));
    CHECK(strcmp(line, "c6: up | restarts 0") == 0);
}

static void hard_signal_reattaches_at_once(void) {
    c6w_source_t const hard[] = {C6W_SRC_TRANSPORT, C6W_SRC_KEYBOARD};
    for (size_t i = 0; i < 2; i++) {
        c6_watch_t w = fresh();
        CHECK(c6_watch_hard(&w, hard[i], 5000) == (C6W_ACT_DOWN_HOOKS | C6W_ACT_REATTACH));
        CHECK(w.state == C6W_ATTACHING && w.attempt == 1 && w.source == hard[i]);
    }
}

static void repeats_while_down_are_ignored(void) {
    c6_watch_t w = fresh();
    c6_watch_hard(&w, C6W_SRC_KEYBOARD, 1000);
    CHECK(c6_watch_hard(&w, C6W_SRC_TRANSPORT, 1100) == 0);
    CHECK(c6_watch_hard(&w, C6W_SRC_KEYBOARD, 1200) == 0);
    CHECK(c6_watch_soft_fail(&w, 1300) == 0);
    CHECK(c6_watch_reattach(&w, 1400) == 0); // an attempt is running
    CHECK(w.source == C6W_SRC_KEYBOARD && w.attempt == 1);
    // and while waiting for the next attempt
    c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 2000);
    CHECK(w.state == C6W_DOWN);
    CHECK(c6_watch_hard(&w, C6W_SRC_TRANSPORT, 2500) == 0);
    CHECK(w.state == C6W_DOWN && w.source == C6W_SRC_KEYBOARD);
}

static void soft_signals_probe_first(void) {
    c6_watch_t w = fresh();
    CHECK(c6_watch_soft_fail(&w, 100) == 0);
    CHECK(c6_watch_soft_fail(&w, 200) == 0);
    CHECK(c6_watch_soft_fail(&w, 300) == C6W_ACT_PROBE);
    CHECK(w.state == C6W_UP);
    CHECK(c6_watch_soft_fail(&w, 400) == 0); // one probe at a time
    // an answer: the C6 is fine, count again from zero
    CHECK(c6_watch_probe(&w, true, 500) == 0);
    CHECK(w.state == C6W_UP);
    CHECK(c6_watch_soft_fail(&w, 600) == 0);
    c6_watch_soft_ok(&w); // a send went through: the run is broken
    CHECK(c6_watch_soft_fail(&w, 700) == 0);
    CHECK(c6_watch_soft_fail(&w, 800) == 0);
    CHECK(c6_watch_soft_fail(&w, 900) == C6W_ACT_PROBE);
    // no answer: an outage
    CHECK(c6_watch_probe(&w, false, 1000) == (C6W_ACT_DOWN_HOOKS | C6W_ACT_REATTACH));
    CHECK(w.state == C6W_ATTACHING && w.source == C6W_SRC_LORA);
    // a probe result nobody asked for changes nothing
    c6_watch_t v = fresh();
    CHECK(c6_watch_probe(&v, false, 10) == 0 && v.state == C6W_UP);
}

static void back_off_then_give_up_then_slow_retry(void) {
    c6_watch_t w = fresh();
    c6_watch_hard(&w, C6W_SRC_TRANSPORT, 10000);
    // attempt 1 fails: the second comes 2 s later
    CHECK(c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 11000) == 0);
    CHECK(w.state == C6W_DOWN);
    CHECK(c6_watch_tick(&w, 12999) == 0);
    CHECK(c6_watch_tick(&w, 13000) == C6W_ACT_REATTACH);
    CHECK(w.state == C6W_ATTACHING && w.attempt == 2);
    CHECK(c6_watch_tick(&w, 13100) == 0); // nothing while it runs
    // attempt 2 fails: the third comes 10 s later
    CHECK(c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 14000) == 0);
    CHECK(c6_watch_tick(&w, 23999) == 0);
    CHECK(c6_watch_tick(&w, 24000) == C6W_ACT_REATTACH && w.attempt == 3);
    char line[200];
    c6_watch_line(&w, 24000, line, sizeof(line));
    CHECK(CONTAINS(line, "c6: recovering 3/3"));
    // attempt 3 fails: give up, and offer the one restart
    CHECK(c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 25000) == (C6W_ACT_GIVE_UP | C6W_ACT_FALLBACK_RESTART));
    CHECK(w.state == C6W_GAVE_UP && w.fallback_used);
    c6_watch_line(&w, 25000, line, sizeof(line));
    CHECK(CONTAINS(line, "c6: down (no answer)"));
    // the slow retry, every 5 minutes, without giving up (or offering a restart) again
    CHECK(c6_watch_tick(&w, 25000 + C6W_RETRY_SLOW_MS - 1) == 0);
    CHECK(c6_watch_tick(&w, 25000 + C6W_RETRY_SLOW_MS) == C6W_ACT_REATTACH);
    CHECK(w.state == C6W_ATTACHING);
    CHECK(c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 26000 + C6W_RETRY_SLOW_MS) == 0);
    CHECK(w.state == C6W_GAVE_UP);
    CHECK(c6_watch_tick(&w, 26000 + 2 * C6W_RETRY_SLOW_MS) == C6W_ACT_REATTACH);
    // a slow retry that works brings everything back
    CHECK(c6_watch_attached(&w, true, 4000, 27000 + 2 * C6W_RETRY_SLOW_MS) == (C6W_ACT_UP_HOOKS | C6W_ACT_NOTE));
    CHECK(w.state == C6W_UP && w.restarts == 1);
}

static void time_wraps(void) {
    c6_watch_t w = fresh();
    uint32_t   t = 0xFFFFF000u;
    c6_watch_hard(&w, C6W_SRC_TRANSPORT, t);
    c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, t + 1000);
    CHECK(c6_watch_tick(&w, t + 2999) == 0);
    CHECK(c6_watch_tick(&w, t + 3000) == C6W_ACT_REATTACH); // past the wrap
    CHECK(c6_watch_attached(&w, true, 1000, t + 5000) == (C6W_ACT_UP_HOOKS | C6W_ACT_NOTE));
    CHECK(w.last_recovery_ms == 5000);
}

static void fallback_at_most_once_never_on_trial(void) {
    // twice in one boot: only the first outage offers the restart
    c6_watch_t w = fresh();
    for (int round = 0; round < 2; round++) {
        uint32_t t = 100000u * (uint32_t)(round + 1);
        c6_watch_hard(&w, C6W_SRC_TRANSPORT, t);
        unsigned last = 0;
        for (int i = 0; i < C6W_ATTEMPTS; i++) {
            t += 20000;
            c6_watch_tick(&w, t);
            last = c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, t + 1);
        }
        CHECK(w.state == C6W_GAVE_UP);
        CHECK(last == (round == 0 ? (C6W_ACT_GIVE_UP | C6W_ACT_FALLBACK_RESTART) : C6W_ACT_GIVE_UP));
        c6_watch_reattach(&w, t + 2);
        c6_watch_attached(&w, true, 1000, t + 3);
        CHECK(w.state == C6W_UP);
    }
    // already restarted recently (NVS), or a trial image: never
    for (int k = 0; k < 2; k++) {
        c6_watch_t v;
        c6_watch_init(&v, k == 0, k == 1);
        c6_watch_hard(&v, C6W_SRC_KEYBOARD, 0);
        unsigned last = 0;
        for (uint32_t i = 0, t = 0; i < C6W_ATTEMPTS; i++) {
            t += 20000;
            c6_watch_tick(&v, t);
            last = c6_watch_attached(&v, false, C6W_UPTIME_UNKNOWN, t + 1);
        }
        CHECK(v.state == C6W_GAVE_UP && last == C6W_ACT_GIVE_UP);
    }
}

static void reattach_command(void) {
    c6_watch_t w = fresh();
    CHECK(c6_watch_reattach(&w, 1000) == (C6W_ACT_DOWN_HOOKS | C6W_ACT_REATTACH));
    CHECK(w.state == C6W_ATTACHING && w.source == C6W_SRC_REATTACH);
    c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 2000);
    // waiting for attempt 2: the command does not wait for the back-off
    CHECK(c6_watch_reattach(&w, 2100) == C6W_ACT_REATTACH && w.attempt == 2);
    c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 2200);
    c6_watch_tick(&w, 20000);
    c6_watch_attached(&w, false, C6W_UPTIME_UNKNOWN, 20100);
    CHECK(w.state == C6W_GAVE_UP);
    // given up: the command tries at once
    CHECK(c6_watch_reattach(&w, 30000) == C6W_ACT_REATTACH && w.state == C6W_ATTACHING);
}

static void reason_from_uptime(void) {
    char line[200];
    // back after 6.8 s with a C6 that has run 5 s: it rebooted, and fw c6 reset caused it
    c6_watch_t w = fresh();
    c6_watch_manual_reset(&w, 10000);
    c6_watch_hard(&w, C6W_SRC_TRANSPORT, 10400);
    CHECK(c6_watch_attached(&w, true, 5000, 17200) == (C6W_ACT_UP_HOOKS | C6W_ACT_NOTE));
    CHECK(w.restarts == 1 && w.last_recovery_ms == 6800);
    CHECK(strcmp(w.reason, "transport failure 0.4 s after fw c6 reset, back in 6.8 s, C6 rebooted (uptime 5 s)") == 0);
    c6_watch_line(&w, 17200, line, sizeof(line));
    CHECK(strcmp(line, "c6: up | restarts 1 | last: transport failure 0.4 s after fw c6 reset, back in 6.8 s, C6 "
                       "rebooted (uptime 5 s)") == 0);
    // a C6 that has run far longer than the outage only lost the transport
    c6_watch_hard(&w, C6W_SRC_KEYBOARD, 1000000); // fw c6 reset is long ago: not blamed
    c6_watch_attached(&w, true, 3600000, 1001000);
    CHECK(w.restarts == 2);
    CHECK(strcmp(w.reason, "keyboard chip reset, back in 1.0 s, transport only (C6 uptime 3600 s)") == 0);
    // no uptime
    c6_watch_reattach(&w, 2000000);
    c6_watch_attached(&w, true, C6W_UPTIME_UNKNOWN, 2000500);
    CHECK(strcmp(w.reason, "fw c6 reattach, back in 0.5 s, C6 uptime unknown") == 0);
    // a probe without an answer
    c6_watch_soft_fail(&w, 3000000);
    c6_watch_soft_fail(&w, 3000001);
    c6_watch_soft_fail(&w, 3000002);
    c6_watch_probe(&w, false, 3000003);
    c6_watch_attached(&w, true, 2000, 3002003);
    CHECK(strcmp(w.reason, "LoRa sends failing, back in 2.0 s, C6 rebooted (uptime 2 s)") == 0);
}

static void line_fits_and_while_down(void) {
    c6_watch_t w = fresh();
    c6_watch_manual_reset(&w, 0);
    c6_watch_hard(&w, C6W_SRC_KEYBOARD, 1200);
    char line[200];
    c6_watch_line(&w, 4200, line, sizeof(line));
    CHECK(strcmp(line, "c6: recovering 1/3 | restarts 0 | now: keyboard chip reset 1.2 s after fw c6 reset, "
                       "down 3.0 s") == 0);
    // never past the buffer, always terminated
    char small[16];
    memset(small, 'x', sizeof(small));
    size_t n = c6_watch_line(&w, 4200, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small));
    CHECK(c6_watch_line(&w, 0, NULL, 0) == 0);
}

int main(void) {
    starts_up_and_quiet();
    hard_signal_reattaches_at_once();
    repeats_while_down_are_ignored();
    soft_signals_probe_first();
    back_off_then_give_up_then_slow_retry();
    time_wraps();
    fallback_at_most_once_never_on_trial();
    reattach_command();
    reason_from_uptime();
    line_fits_and_while_down();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("c6_watch_test: all passed\n");
    return 0;
}
