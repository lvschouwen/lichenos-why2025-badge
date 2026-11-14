// Host test: the test and state-changing console forms need developer mode (#421), also under the "fw <app>" prefix
#include "dev_gate.h"

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

static char const *const IDS[] = {"meshcore", "bench"};

// The forms GATED[] gated, and those it left open, are pinned with every other rule in console_pin_test.c (#464)

// The gate decision, with the command function replaced by a counter: a refused line never runs it
static int ran;
static void counting(char const *args) {
    (void)args;
    ran++;
}

static void dispatch(char const *text, bool dev_on) {
    char                 buf[96];
    char const          *args;
    int                  app, i = 0;
    static console_entry_t const table[] = {{"psram", "psram", counting, false, 0, 0}, {"stress", "stress", counting, false, 0, 0},
                                            {"ui", "ui", counting, true, 0, 0}};
    snprintf(buf, sizeof(buf), "%s", text);
    char *name = dev_gate_split(buf, IDS, 2, &app, &args);
    dev_find_t found = dev_mode_find_app(table, 3, name, dev_on, app, &i);
    if (dev_gate_decide(found, dev_gate_needs_dev(name, args), dev_on) == DEV_GATE_RUN) {
        table[i].fn(args);
    }
}

static void test_decide(void) {
    CHECK(dev_gate_decide(DEV_FIND_RUN, true, false) == DEV_GATE_REFUSE);
    CHECK(dev_gate_decide(DEV_FIND_RUN, true, true) == DEV_GATE_RUN);
    CHECK(dev_gate_decide(DEV_FIND_RUN, false, false) == DEV_GATE_RUN);
    CHECK(dev_gate_decide(DEV_FIND_DEV_OFF, false, false) == DEV_GATE_REFUSE);
    CHECK(dev_gate_decide(DEV_FIND_NONE, true, true) == DEV_GATE_UNKNOWN);
    ran = 0;
    dispatch("psram speed", false);
    dispatch("psram speed 120 yes", false);
    dispatch("stress", false);
    dispatch("ui", false);
    CHECK(ran == 0);
    dispatch("psram", false);
    dispatch("stress stop", false);
    CHECK(ran == 2);
    dispatch("psram speed", true);
    dispatch("stress", true);
    CHECK(ran == 4);
}

static void test_split(void) {
    char        a[] = "meshcore  psram  test 1";
    char const *args;
    int         app;
    char       *name = dev_gate_split(a, IDS, 2, &app, &args);
    CHECK(strcmp(name, "psram") == 0 && strcmp(args, "test 1") == 0 && app == 1);
    char b[] = "psram";
    name     = dev_gate_split(b, IDS, 2, &app, &args);
    CHECK(strcmp(name, "psram") == 0 && args[0] == '\0' && app == 0);
    char c[] = "bench";
    name     = dev_gate_split(c, IDS, 2, &app, &args);
    CHECK(name[0] == '\0' && args[0] == '\0' && app == 2);
}

int main(void) {
    test_split();
    test_decide();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_gate_test: ok\n");
    return 0;
}
