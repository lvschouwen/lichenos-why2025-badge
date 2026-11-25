// Host test: the notice policy while a video owner holds the screen, and the OS hold-Esc timing (#563, part of #527)
#include "video_notice.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int reclaimed;
static vn_kind_t last_kind;
static bool fake_reclaim(vn_kind_t kind) {
    reclaimed++;
    last_kind = kind;
    return true;
}

static void policy_table(void) {
    // No owner: everything just shows
    for (int k = VN_TOAST; k <= VN_ESCAPE; k++) {
        CHECK(video_notice_policy((vn_kind_t)k, false) == VN_SHOW);
    }
    // An active owner: the toast waits, every critical notice takes the screen back
    CHECK(video_notice_policy(VN_TOAST, true) == VN_HOLD);
    CHECK(video_notice_policy(VN_DIALOG, true) == VN_RECLAIM);
    CHECK(video_notice_policy(VN_THERMAL, true) == VN_RECLAIM);
    CHECK(video_notice_policy(VN_CARD, true) == VN_RECLAIM);
    CHECK(video_notice_policy(VN_ESCAPE, true) == VN_RECLAIM);
}

static void raise_calls_reclaim(void) {
    video_notice_set_reclaim(fake_reclaim);
    video_notice_set_active(false);
    CHECK(!video_notice_raise(VN_THERMAL) && reclaimed == 0); // no owner: nothing to take back
    video_notice_set_active(true);
    CHECK(!video_notice_raise(VN_TOAST) && reclaimed == 0);    // held, never reclaims
    CHECK(video_notice_raise(VN_DIALOG) && reclaimed == 1 && last_kind == VN_DIALOG);
    CHECK(video_notice_raise(VN_CARD) && reclaimed == 2 && last_kind == VN_CARD);
    video_notice_set_reclaim(NULL);
    CHECK(!video_notice_raise(VN_ESCAPE)); // no hook registered yet: no crash, nothing done
    video_notice_set_active(false);
}

static void escape_timing(void) {
    video_escape_t e = {0};
    // Short press: down 600 ms, up: never fires
    CHECK(!video_escape_sample(&e, true, false, 0, 1000));
    CHECK(!video_escape_sample(&e, true, true, 1, 1100));
    CHECK(!video_escape_sample(&e, true, true, 1, 1700));
    CHECK(!video_escape_sample(&e, true, false, 1, 1800));
    CHECK(!video_escape_sample(&e, true, false, 1, 9000));
    // Hold: fires once, at 2 s
    CHECK(!video_escape_sample(&e, true, true, 2, 10000));
    CHECK(!video_escape_sample(&e, true, true, 2, 11900));
    CHECK(video_escape_sample(&e, true, true, 2, 12000));
    CHECK(!video_escape_sample(&e, true, true, 2, 12100));
    CHECK(!video_escape_sample(&e, true, false, 2, 12200));
    // A release and press between two samples restarts the hold
    CHECK(!video_escape_sample(&e, true, true, 3, 20000));
    CHECK(!video_escape_sample(&e, true, true, 4, 21500)); // a new press: the clock starts again at 21500
    CHECK(!video_escape_sample(&e, true, true, 4, 23400));
    CHECK(video_escape_sample(&e, true, true, 4, 23500));
}

static void escape_ignores_old_and_idle(void) {
    video_escape_t e = {0};
    // Esc already down when the owner starts is no escape, however long it stays down
    CHECK(!video_escape_sample(&e, false, true, 5, 100));
    CHECK(!video_escape_sample(&e, true, true, 5, 200));
    CHECK(!video_escape_sample(&e, true, true, 5, 9000));
    // No owner: a hold never fires
    video_escape_t idle = {0};
    CHECK(!video_escape_sample(&idle, false, true, 1, 0));
    CHECK(!video_escape_sample(&idle, false, true, 1, 5000));
    // The wrap of the tick counter does not matter
    video_escape_t w = {0};
    CHECK(!video_escape_sample(&w, true, false, 0, 0xFFFFFC00u));
    CHECK(!video_escape_sample(&w, true, true, 1, 0xFFFFFC10u));
    CHECK(video_escape_sample(&w, true, true, 1, 0xFFFFFC10u + 2000u));
}

int main(void) {
    policy_table();
    raise_calls_reclaim();
    escape_timing();
    escape_ignores_old_and_idle();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("video_notice_test: ok\n");
    return 0;
}
