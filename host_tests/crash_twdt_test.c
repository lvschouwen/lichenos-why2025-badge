// Host test (#479): "crash test twdt [core]" argument parsing and the one-test-at-a-time claim.
#include "crash_twdt.h"

#include <stdio.h>

static int failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool core_of(char const *rest, int expect) {
    int core = -1;
    return crash_twdt_parse(rest, &core) && core == expect;
}

int main(void) {
    int core = 9;
    CHECK(core_of("", 0)); // default core 0
    CHECK(core_of(" 0", 0));
    CHECK(core_of(" 1", 1));
    CHECK(!crash_twdt_parse(" 2", &core));
    CHECK(!crash_twdt_parse(" ", &core));
    CHECK(!crash_twdt_parse(" -1", &core));
    CHECK(!crash_twdt_parse(" 10", &core));
    CHECK(!crash_twdt_parse(" 1 ", &core));
    CHECK(!crash_twdt_parse("1", &core));
    CHECK(!crash_twdt_parse(" x", &core));
    CHECK(core == 9); // a refusal leaves the output alone

    CHECK(crash_twdt_claim());
    CHECK(!crash_twdt_claim()); // refused while running
    crash_twdt_release();
    CHECK(crash_twdt_claim()); // free again after the task ended
    crash_twdt_release();

    if (failures) {
        return 1;
    }
    printf("crash_twdt_test ok\n");
    return 0;
}
