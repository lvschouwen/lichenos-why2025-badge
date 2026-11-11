// Host test: how lichen_clock applies an accepted time (#326): SNTP under 1 s slewed, a failed slew or a larger offset
// stepped, other sources never slewed; the fix counters, the pending slew and the saver only after a real set.
// The clock_host_* hooks replace lichen_clock.c's weak host defaults; the host's own clock is never touched.
#include "lichen_clock.h"

#include <stdio.h>
#include <sys/time.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int     set_calls, slew_calls, saves;
static int     set_result, slew_result;
static int64_t slew_offset_us, slew_prev_us, pending_now_us;

int clock_host_set(struct timeval const *tv);
int clock_host_slew(int64_t offset_us, int64_t *pending_us);
int64_t clock_host_pending_us(void);

int clock_host_set(struct timeval const *tv) {
    (void)tv;
    set_calls++;
    return set_result;
}

int clock_host_slew(int64_t offset_us, int64_t *pending_us) {
    slew_calls++;
    slew_offset_us = offset_us;
    *pending_us    = slew_result == 0 ? slew_prev_us : 0;
    return slew_result;
}

int64_t clock_host_pending_us(void) {
    return pending_now_us;
}

static void count_save(void) {
    saves++;
}

// The host clock plus off_us, as an answer would carry it
static struct timeval ahead(int64_t off_us) {
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t us = (int64_t)now.tv_sec * 1000000 + now.tv_usec + off_us;
    return (struct timeval){.tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000)};
}

static void reset_calls(void) {
    set_calls = slew_calls = saves = 0;
}

// A manual set within a second: stepped, never slewed, and not counted as an SNTP correction
static void test_manual_never_slews(void) {
    reset_calls();
    struct timeval tv = ahead(200000);
    CHECK(clock_set_manual(&tv, false) == CLOCK_OK);
    CHECK(set_calls == 1 && slew_calls == 0 && saves == 1);
    clock_sync_info_t i;
    clock_fix_info_t  f;
    clock_sync_snapshot(&i, &f);
    CHECK(f.fix == CLOCK_FIX_NONE && f.slewed == 0 && f.stepped == 0);
}

static void test_sntp_small_slews(void) {
    reset_calls();
    slew_prev_us      = 12000; // an earlier slew still had 12 ms to go
    struct timeval tv = ahead(48000);
    clock_answer_t a;
    CHECK(clock_offer_sntp(&tv, &a) == CLOCK_OK);
    CHECK(slew_calls == 1 && set_calls == 0 && saves == 1);
    CHECK(slew_offset_us > 40000 && slew_offset_us <= 48000);
    CHECK(a.how == CLOCK_FIX_SLEW && a.pending_ms == 12 && a.step_ms >= 40 && a.step_ms <= 48);
    clock_sync_info_t i;
    clock_fix_info_t  f;
    clock_sync_snapshot(&i, &f);
    CHECK(f.fix == CLOCK_FIX_SLEW && f.slewed == 1 && f.stepped == 0 && f.pending_ms == 12);
    CHECK(i.synced && i.source == CLOCK_SRC_SNTP && i.step_ms == a.step_ms);
}

static void test_sntp_slew_fails_steps(void) {
    reset_calls();
    slew_result       = -1;
    pending_now_us    = 7000;
    struct timeval tv = ahead(500000);
    clock_answer_t a;
    CHECK(clock_offer_sntp(&tv, &a) == CLOCK_OK);
    CHECK(slew_calls == 1 && set_calls == 1 && saves == 1);
    CHECK(a.how == CLOCK_FIX_STEP && a.pending_ms == 7);
    slew_result = 0;
}

static void test_sntp_large_steps(void) {
    reset_calls();
    pending_now_us    = 0;
    struct timeval tv = ahead(5 * 1000000LL);
    clock_answer_t a;
    CHECK(clock_offer_sntp(&tv, &a) == CLOCK_OK);
    CHECK(slew_calls == 0 && set_calls == 1 && saves == 1);
    CHECK(a.how == CLOCK_FIX_STEP && a.step_ms >= 4990 && a.step_ms <= 5000);
    clock_sync_info_t i;
    clock_fix_info_t  f;
    clock_sync_snapshot(&i, &f);
    CHECK(f.fix == CLOCK_FIX_STEP && f.slewed == 1 && f.stepped == 2);
}

// Refused (a jump too big after a sync) and accepted-but-not-taken: neither labelled, counted nor saved
static void test_not_set_not_labelled(void) {
    reset_calls();
    struct timeval tv = ahead(3600 * 1000000LL);
    clock_answer_t a  = {.how = CLOCK_FIX_STEP};
    CHECK(clock_offer_sntp(&tv, &a) == CLOCK_BIG_STEP);
    CHECK(a.how == CLOCK_FIX_NONE && slew_calls == 0 && set_calls == 0 && saves == 0);

    set_result = -1;
    tv         = ahead(3 * 1000000LL);
    CHECK(clock_offer_sntp(&tv, &a) == CLOCK_OK);
    CHECK(a.how == CLOCK_FIX_NONE && set_calls == 1 && saves == 0);
    set_result = 0;

    clock_sync_info_t i;
    clock_fix_info_t  f;
    clock_sync_snapshot(&i, &f);
    CHECK(f.fix == CLOCK_FIX_STEP && f.slewed == 1 && f.stepped == 2);
}

int main(void) {
    clock_sync_info_t i = clock_sync_info();
    if (i.source != CLOCK_SRC_RTC) {
        printf("clock_apply_test: the host clock is outside the build window, nothing to test\n");
        return 1;
    }
    clock_set_saver(count_save);
    test_manual_never_slews();
    test_sntp_small_slews();
    test_sntp_slew_fails_steps();
    test_sntp_large_steps();
    test_not_set_not_labelled();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("clock_apply_test: all passed\n");
    return 0;
}
