#include "thermal_rules.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static th_input_t board(float c) {
    th_input_t in = {0};
    in.src[TH_SRC_BOARD] = (th_reading_t){true, c};
    return in;
}
static th_input_t both(float b, float d) {
    th_input_t in = board(b);
    in.src[TH_SRC_DIE] = (th_reading_t){true, d};
    return in;
}

static void warn_needs_two_polls_and_clears_with_hysteresis(void) {
    th_state_t st; th_reset(&st);
    th_input_t hot = board(51), mid = board(47), cool = board(44);
    CHECK(th_step(&st, &hot, 0) == 0);
    CHECK(th_step(&st, &hot, 5) == TH_EV_WARN);
    CHECK(st.level == TH_WARN && st.cause == TH_SRC_BOARD);
    CHECK(th_step(&st, &mid, 10) == 0); // between clear and warn: stays
    CHECK(th_step(&st, &mid, 15) == 0 && st.level == TH_WARN);
    CHECK(th_step(&st, &cool, 20) == 0);
    CHECK(th_step(&st, &cool, 25) == TH_EV_CLEARED && st.level == TH_OK);
}

static void single_glitch_does_nothing(void) {
    th_state_t st; th_reset(&st);
    th_input_t ok = board(30), spike = board(80);
    for (uint32_t t = 0; t < 40; t += 5) CHECK(th_step(&st, &ok, t) == 0);
    CHECK(th_step(&st, &spike, 40) == 0);
    CHECK(th_step(&st, &ok, 45) == 0);
    CHECK(st.level == TH_OK);
}

static void shutdown_at_limit_latches(void) {
    th_state_t st; th_reset(&st);
    th_input_t h = board(61);
    th_step(&st, &h, 0);
    CHECK(th_step(&st, &h, 5) == TH_EV_SHUTDOWN);
    CHECK(st.level == TH_SHUTDOWN && !st.cause_rise);
    th_input_t c = board(25);
    CHECK(th_step(&st, &c, 10) == 0 && st.level == TH_SHUTDOWN);
}

static void rise_of_ten_in_a_minute_shuts_down(void) {
    th_state_t st; th_reset(&st);
    th_input_t a = board(36), b = board(41), c = board(47);
    th_step(&st, &a, 0);
    th_step(&st, &a, 5);
    CHECK(th_step(&st, &b, 10) == 0); // +5 only
    th_step(&st, &c, 15);             // +11 from 36, at 47 C
    CHECK(th_step(&st, &c, 20) == TH_EV_SHUTDOWN);
    CHECK(st.cause_rise);
}

static void slow_rise_is_not_a_surge(void) {
    th_state_t st; th_reset(&st);
    for (uint32_t i = 0; i < 40; i++) { // +1 K per 10 s up to 56: warns, never shuts down
        th_input_t in = board(30.0f + (float)i * 0.65f);
        unsigned ev = th_step(&st, &in, i * 10);
        CHECK(!(ev & TH_EV_SHUTDOWN));
    }
    CHECK(st.level == TH_WARN);
}

static void rise_needs_the_floor(void) {
    th_state_t st; th_reset(&st);
    th_input_t a = board(10), b = board(25);
    th_step(&st, &a, 0);
    CHECK(th_step(&st, &b, 5) == 0);
    CHECK(th_step(&st, &b, 10) == 0 && st.level == TH_OK);
}

static void gap_restarts_the_window(void) {
    th_state_t st; th_reset(&st);
    th_input_t a = board(36), none = {0}, b = board(48);
    th_step(&st, &a, 0);
    th_step(&st, &none, 5);
    th_step(&st, &b, 10);
    CHECK(th_rise(&st, TH_SRC_BOARD, 10) == 0);
    CHECK(th_step(&st, &b, 15) == 0);
}

static void old_samples_leave_the_window(void) {
    th_state_t st; th_reset(&st);
    th_input_t a = board(36), b = board(47);
    th_step(&st, &a, 0);
    th_step(&st, &b, 70); // 70 s later: no rise
    CHECK(th_rise(&st, TH_SRC_BOARD, 70) == 0);
}

static void die_has_its_own_limits(void) {
    th_state_t st; th_reset(&st);
    th_input_t d = both(30, 65);
    th_step(&st, &d, 0);
    CHECK(th_step(&st, &d, 5) == 0 && st.level == TH_OK); // 65 C die is normal
    th_input_t h = both(30, 76);
    th_step(&st, &h, 10);
    CHECK(th_step(&st, &h, 15) == TH_EV_WARN && st.cause == TH_SRC_DIE);
}

static void one_dead_source_leaves_the_other_guarding(void) {
    th_state_t st; th_reset(&st);
    th_input_t in = {0};
    in.src[TH_SRC_DIE] = (th_reading_t){true, 91};
    th_step(&st, &in, 0);
    CHECK(th_step(&st, &in, 5) == TH_EV_SHUTDOWN);
}

static void implausible_is_ignored_and_counted(void) {
    th_state_t st; th_reset(&st);
    th_input_t bad = board(200), nan = board(0.0f / 0.0f);
    for (uint32_t t = 0; t < 100; t += 5) {
        CHECK((th_step(&st, &bad, t) & TH_EV_SHUTDOWN) == 0);
    }
    CHECK(st.level == TH_OK && st.implausible[TH_SRC_BOARD] == 20);
    th_step(&st, &nan, 100);
    CHECK(st.implausible[TH_SRC_BOARD] == 21);
}

static void blind_is_reported_once_and_never_shuts_down(void) {
    th_state_t st; th_reset(&st);
    th_input_t none = {0};
    unsigned total = 0;
    int blinds = 0;
    for (uint32_t t = 0; t <= 300; t += 5) {
        unsigned ev = th_step(&st, &none, t);
        total |= ev;
        blinds += (ev & TH_EV_BLIND) != 0;
    }
    CHECK(blinds == 1 && st.blind);
    CHECK(!(total & TH_EV_SHUTDOWN) && st.level == TH_OK);
    th_input_t ok = board(30);
    CHECK(th_step(&st, &ok, 305) == (TH_EV_SIGHT | TH_EV_BOARD_BACK) && !st.blind);
}

static void blind_warning_keeps_its_level(void) {
    th_state_t st; th_reset(&st);
    th_input_t h = board(52), none = {0};
    th_step(&st, &h, 0);
    th_step(&st, &h, 5);
    for (uint32_t t = 10; t < 200; t += 5) th_step(&st, &none, t);
    CHECK(st.level == TH_WARN); // cooling was never seen
}

static void heater_offset(void) {
    CHECK(th_board_from_bme(40, true) == 40 - TH_BME_HEATER_OFFSET_C);
    CHECK(th_board_from_bme(40, false) == 40);
}

static void board_loss_is_reported_while_die_answers(void) {
    th_state_t st; th_reset(&st);
    th_input_t die_only = {0};
    die_only.src[TH_SRC_DIE] = (th_reading_t){true, 50};
    int lost = 0;
    unsigned all = 0;
    for (uint32_t t = 0; t <= 200; t += 5) {
        unsigned ev = th_step(&st, &die_only, t);
        all |= ev;
        lost += (ev & TH_EV_BOARD_LOST) != 0;
    }
    CHECK(lost == 1 && st.board_lost);
    CHECK(!(all & (TH_EV_BLIND | TH_EV_SHUTDOWN)) && !st.blind);
    th_input_t ok = both(30, 50);
    CHECK(th_step(&st, &ok, 205) == TH_EV_BOARD_BACK && !st.board_lost);
}

static void die_rise_powers_off_by_design(void) {
    th_state_t st; th_reset(&st);
    th_input_t a = both(30, 62), b = both(30, 83);
    th_step(&st, &a, 0);
    th_step(&st, &b, 5);
    CHECK(th_step(&st, &b, 10) == TH_EV_SHUTDOWN && st.cause == TH_SRC_DIE && st.cause_rise);
}

int main(void) {
    board_loss_is_reported_while_die_answers();
    die_rise_powers_off_by_design();
    warn_needs_two_polls_and_clears_with_hysteresis();
    single_glitch_does_nothing();
    shutdown_at_limit_latches();
    rise_of_ten_in_a_minute_shuts_down();
    slow_rise_is_not_a_surge();
    rise_needs_the_floor();
    gap_restarts_the_window();
    old_samples_leave_the_window();
    die_has_its_own_limits();
    one_dead_source_leaves_the_other_guarding();
    implausible_is_ignored_and_counted();
    blind_is_reported_once_and_never_shuts_down();
    blind_warning_keeps_its_level();
    heater_offset();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("thermal_rules_test: ok\n");
    return 0;
}
