// Host test: the `fw c6log` duration argument (#458)
#include "c6_log_arg.h"

#include <stdio.h>

static int failures;

static void expect(char const *args, bool ok, uint32_t want) {
    uint32_t ms = 7;
    bool     r  = c6_log_parse_ms(args, &ms);
    if (r != ok || (ok && ms != want) || (!ok && ms != 7)) {
        printf("FAIL \"%s\": got %d / %u\n", args, r, (unsigned)ms);
        failures++;
    }
}

int main(void) {
    expect("", true, 10000);
    expect("1", true, 1);
    expect("250", true, 250);
    expect("60000", true, 60000);
    expect("  500", true, 500);
    expect("0", false, 0);
    expect("60001", false, 0);
    expect("10s", false, 0); // was taken as 10 ms
    expect("10ms", false, 0);
    expect("10 s", false, 0);
    expect("10 ", false, 0);
    expect("+5", false, 0);
    expect("-5", false, 0);
    expect("abc", false, 0);
    expect("   ", false, 0);
    expect("99999999999999999999", false, 0);
    expect("4294967306", false, 0); // 2^32 + 10 must not wrap to 10
    if (failures) {
        return 1;
    }
    printf("c6_log_arg_test: ok\n");
    return 0;
}
