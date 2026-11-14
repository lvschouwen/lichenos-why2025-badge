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

// What console.c decides for a whole line: split (prefix too), then ask the table
static bool line_gated(char const *text) {
    char        buf[96];
    char const *args;
    int         app;
    snprintf(buf, sizeof(buf), "%s", text);
    char *name = dev_gate_split(buf, IDS, 2, &app, &args);
    return dev_gate_needs_dev(name, args);
}

static char const *const GATED[] = {
    "thermal test warn", "thermal test clear", "thermal test shutdown YES", "thermal seen reset", "thermal bogus",
    "psram test 64", "psram test", "psram speed", "psram dma", "psram dl 3", "psram dlboot 3 yes", "psram speed 120", "psram carrier",
    "psram  test", "noscreen", "noscreen now", "stress", "stress 5", "stress status ", "stress bogus",
    "idle test 30", "idle bogus", "notify test warn", "notify test", "notify test info @x hi", "vib test",
    "vib test alert", "setup reset", "setup reset mesh", "setup done mesh", "setup", "sensors start", "sensors stop",
    "sensors boot on", "sensors boot off", "sensors boot x", "sensors bogus",
    "mesh chat open #badgetest", "mesh chat close", "mesh chat", "mesh chat open", "mesh  chat   open x",
    "mesh ident simulate", "mesh ident", "mesh  ident   simulate",
};

static char const *const OPEN[] = {
    "thermal", "psram", "stress status", "stress stop", "idle", "notify", "notify clear", "notify read", "notify dnd on",
    "notify testing", "vib", "vib on", "vib off", "vib strength low", "vib tests", "sensors", "sensors raw",
    "sensors boot", "mesh", "mesh chats", "mesh send #x hi", "mesh chatty", "mesh identity", "mesh airlog", "help", "wifi on", "mem", "crash", "boot", "reboot",
};

static void test_forms(void) {
    for (unsigned i = 0; i < sizeof(GATED) / sizeof(GATED[0]); i++) {
        if (!line_gated(GATED[i])) {
            printf("not gated: %s\n", GATED[i]);
            failures++;
        }
    }
    for (unsigned i = 0; i < sizeof(OPEN) / sizeof(OPEN[0]); i++) {
        if (line_gated(OPEN[i])) {
            printf("gated but should be open: %s\n", OPEN[i]);
            failures++;
        }
    }
}

// "fw <app> <command>" (#423): the same decision under either spelling
static void test_prefixed(void) {
    for (unsigned i = 0; i < sizeof(GATED) / sizeof(GATED[0]); i++) {
        char line[96];
        snprintf(line, sizeof(line), "meshcore %s", GATED[i]);
        if (!line_gated(line)) {
            printf("not gated with prefix: %s\n", line);
            failures++;
        }
    }
    for (unsigned i = 0; i < sizeof(OPEN) / sizeof(OPEN[0]); i++) {
        char line[96];
        snprintf(line, sizeof(line), "bench %s", OPEN[i]);
        if (line_gated(line)) {
            printf("gated with prefix but open: %s\n", line);
            failures++;
        }
    }
}

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
    static console_entry_t const table[] = {{"psram", "psram", counting, false, 0}, {"stress", "stress", counting, false, 0},
                                            {"ui", "ui", counting, true, 0}};
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
    test_forms();
    test_prefixed();
    test_split();
    test_decide();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_gate_test: ok\n");
    return 0;
}
