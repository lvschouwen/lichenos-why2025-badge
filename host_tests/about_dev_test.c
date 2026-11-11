// Host test: About > For developers' value lines (#334)
#include "about_dev.h"

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

static int is(char const *key, char const *raw, char const *want) {
    char out[96];
    about_dev_text(key, raw, out, sizeof(out));
    if (strcmp(out, want) != 0) {
        printf("got \"%s\" want \"%s\"\n", out, want);
        return 0;
    }
    return 1;
}

int main(void) {
    CHECK(is("disp.refresh", "446 (3.2 s ago)", "44.6 Hz (3.2 s ago)"));
    CHECK(is("disp.refresh", "600", "60.0 Hz"));
    CHECK(is("psram.clk", "200000000 (1 s ago)", "200 MHz (1 s ago)"));
    CHECK(is("psram.clk", "80000000 (stale, 4 s ago)", "80 MHz (stale, 4 s ago)"));
    CHECK(is("wifi.country", "NL (2 s ago)", "NL (2 s ago)"));
    CHECK(is("bt.state", "idle (stale, 12 s ago)", "idle (stale, 12 s ago)"));
    CHECK(is("lora.power", "14 (0.5 s ago)", "14 (0.5 s ago)"));
    CHECK(is("settings.last_err", " (3 s ago)", "none (3 s ago)"));
    CHECK(is("settings.last_err", "", "none")); // no failure yet: an empty value, not "not available"
    CHECK(is("settings.write_fail", "0", "0"));
    CHECK(is("c6.reg_caps", "32790 (4 s ago)", "0x8016 (4 s ago)"));
    CHECK(is("c6.reg_caps", "32790", "0x8016"));
    // not readable: the registry's reason is shown whole, never the unit conversion
    CHECK(is("psram.clk", "? (not read yet)", "? (not read yet)"));
    CHECK(is("c6.reg_caps", "? (stale, 31 s ago)", "? (stale, 31 s ago)"));
    // a value that is not a number stays as written
    CHECK(is("disp.refresh", "abc (1 s ago)", "abc (1 s ago)"));
    // an unknown key: 0 bytes
    CHECK(is("nope", "", "not available"));
    CHECK(is("nope", NULL, "not available"));
    // a small buffer is cut, not overrun
    char tiny[8];
    CHECK(about_dev_text("psram.clk", "200000000 (1 s ago)", tiny, sizeof(tiny)) >= sizeof(tiny) && tiny[7] == 0);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("about_dev_test: ok\n");
    return 0;
}
