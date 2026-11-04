// Host test: the remote key lock and "fw ui open" matching (#190)
#include "remote_ctl.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ESC 1
#define KEY 30

static void test_timeout(void) {
    rlock_t l = {0};
    CHECK(!rlock_key(&l, KEY, false, true, 0)); // off: keys pass
    CHECK(!rlock_key(&l, KEY, false, false, 10));
    rlock_on(&l, 60, 1000);
    CHECK(rlock_tick(&l, 1000));
    CHECK(rlock_left_s(&l, 1000) == 60);
    CHECK(rlock_key(&l, KEY, false, true, 2000)); // dropped
    CHECK(rlock_key(&l, KEY, false, false, 2100)); // and its release
    // A console command at 50 s: the 60 s count again from there
    rlock_touch(&l, 50000);
    CHECK(rlock_tick(&l, 60999));
    CHECK(rlock_tick(&l, 109999));
    CHECK(rlock_left_s(&l, 109001) == 1);
    CHECK(!rlock_tick(&l, 110000));
    CHECK(rlock_take_end(&l) == RLOCK_END_TIMEOUT);
    CHECK(rlock_take_end(&l) == RLOCK_END_NONE);
    CHECK(!rlock_key(&l, KEY, false, true, 110100)); // keys work again
    CHECK(rlock_left_s(&l, 110100) == 0);
}

static void test_lazy_expiry_and_wrap(void) {
    // No tick ran (LVGL busy): the key itself finds the time up and passes
    rlock_t l = {0};
    rlock_on(&l, 1, 0xFFFFFC00u);
    CHECK(rlock_key(&l, KEY, false, true, 0xFFFFFF00u));
    CHECK(rlock_key(&l, KEY, false, false, 0xFFFFFF10u));
    CHECK(!rlock_key(&l, KEY, false, true, 0x00000300u)); // 1.28 s later, across the wrap
    CHECK(rlock_take_end(&l) == RLOCK_END_TIMEOUT);
    CHECK(!rlock_key(&l, KEY, false, false, 0x00000400u));
}

static void test_held_before_lock(void) {
    rlock_t l = {0};
    CHECK(!rlock_key(&l, KEY, false, true, 0)); // down before the lock
    rlock_on(&l, 60, 100);
    CHECK(!rlock_key(&l, KEY, false, false, 200)); // its release reaches LVGL
    CHECK(rlock_key(&l, 40, false, true, 300));
    rlock_off(&l);
    CHECK(rlock_take_end(&l) == RLOCK_END_CONSOLE);
    CHECK(rlock_key(&l, 40, false, false, 400)); // pressed under the lock: the release stays dropped
    CHECK(!rlock_key(&l, 40, false, true, 500));
}

static void test_esc_hold(void) {
    rlock_t l = {0};
    rlock_on(&l, 600, 0);
    // A short Esc is just a dropped key
    CHECK(rlock_key(&l, ESC, true, true, 1000));
    CHECK(rlock_tick(&l, 2000));
    CHECK(rlock_key(&l, ESC, true, false, 2500));
    CHECK(rlock_tick(&l, 9000));
    // Held 3 s: the tick ends the lock; the release that follows is still dropped (no Esc reaches the screen)
    CHECK(rlock_key(&l, ESC, true, true, 10000));
    CHECK(rlock_tick(&l, 12999));
    CHECK(!rlock_tick(&l, 13000));
    CHECK(rlock_take_end(&l) == RLOCK_END_ESC);
    CHECK(rlock_key(&l, ESC, true, false, 14000));
    CHECK(!rlock_key(&l, ESC, true, true, 15000));
    CHECK(!rlock_key(&l, ESC, true, false, 15100));
    // No tick while it was held: the release after 3 s ends it all the same
    rlock_on(&l, 600, 20000);
    CHECK(rlock_key(&l, ESC, true, true, 20000));
    CHECK(rlock_key(&l, ESC, true, false, 23500));
    CHECK(!rlock_tick(&l, 23600));
    CHECK(rlock_take_end(&l) == RLOCK_END_ESC);
    // Other keys held do not count; an Esc down when the lock starts is no escape
    CHECK(!rlock_key(&l, ESC, true, true, 30000));
    rlock_on(&l, 600, 30100);
    CHECK(rlock_tick(&l, 40000));
    CHECK(rlock_key(&l, KEY, false, true, 40000));
    CHECK(rlock_tick(&l, 50000));
}

static void test_parse(void) {
    uint32_t s = 0;
    CHECK(rlock_parse("", &s) == RLOCK_ARGS_SHOW);
    CHECK(rlock_parse(" on", &s) == RLOCK_ARGS_ON && s == RLOCK_DEFAULT_S);
    CHECK(rlock_parse(" on 120", &s) == RLOCK_ARGS_ON && s == 120);
    CHECK(rlock_parse("on 600 ", &s) == RLOCK_ARGS_ON && s == 600);
    CHECK(rlock_parse("on 601", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("on 0", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("on 99999", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("on 1x", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("on -5", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse(" off", &s) == RLOCK_ARGS_OFF);
    CHECK(rlock_parse("off 5", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("onn", &s) == RLOCK_ARGS_ERR);
    CHECK(rlock_parse("lock", &s) == RLOCK_ARGS_ERR);
}

static void test_find_app(void) {
    char const *ids[]   = {"meshcore", "settings", "keytest", "weather", "about"};
    char const *names[] = {"MeshCore", "Settings", "Keyboard test", "Weather", "Settings"};
    CHECK(rctl_find_app(" weather", ids, names, 5) == 3);
    CHECK(rctl_find_app("WEATHER ", ids, names, 5) == 3);
    CHECK(rctl_find_app("Keyboard test", ids, names, 5) == 2);
    CHECK(rctl_find_app("keyboard TEST", ids, names, 5) == 2);
    CHECK(rctl_find_app("meshcore", ids, names, 5) == 0);
    CHECK(rctl_find_app("settings", ids, names, 5) == 1); // an id wins over a later app's name
    CHECK(rctl_find_app("weath", ids, names, 5) == -1);
    CHECK(rctl_find_app("", ids, names, 5) == -1);
    CHECK(rctl_find_app("   ", ids, names, 5) == -1);
    CHECK(rctl_find_app("Keyboard", ids, names, 5) == -1);
}

int main(void) {
    test_timeout();
    test_lazy_expiry_and_wrap();
    test_held_before_lock();
    test_esc_hold();
    test_parse();
    test_find_app();
    if (failures) {
        return 1;
    }
    printf("remote_ctl_test: ok\n");
    return 0;
}
