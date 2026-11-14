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
    "psram test 64", "psram test", "psram dma", "psram dl 3", "psram dlboot 3 yes", "psram speed 120", "psram carrier",
    "psram  test", "noscreen", "noscreen now", "stress", "stress 5", "stress stop", "stress status ", "stress bogus",
    "idle test 30", "idle bogus", "notify test warn", "notify test", "notify test info @x hi", "vib test",
    "vib test alert", "setup reset", "setup reset mesh", "setup done mesh", "setup", "sensors start", "sensors stop",
    "sensors boot on", "sensors boot off", "sensors boot x", "sensors bogus",
};

static char const *const OPEN[] = {
    "thermal", "psram", "stress status", "idle", "notify", "notify clear", "notify read", "notify dnd on",
    "notify testing", "vib", "vib on", "vib off", "vib strength low", "vib tests", "sensors", "sensors raw",
    "sensors boot", "help", "wifi on", "mem", "crash", "boot", "reboot",
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
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_gate_test: ok\n");
    return 0;
}
