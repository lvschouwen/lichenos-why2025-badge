#include "reg_row_text.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void expect(char const *label, char const *cached, char const *saved, char const *unit, char const *want) {
    char out[64];
    size_t n = reg_row_text(label, cached, saved, unit, out, sizeof(out));
    if (strcmp(out, want) != 0 || n != strlen(want)) {
        printf("FAIL: got \"%s\" want \"%s\"\n", out, want);
        failures++;
    }
}

int main(void) {
    expect("Screen now", "50 (3.2 s ago)", "50", "%", "Screen now: 50% (3.2 s)");
    expect(NULL, "dim (12.0 s ago)", NULL, NULL, "dim (12.0 s)");
    expect(NULL, "dim", NULL, NULL, "dim"); // no age: just the value
    // differs from saved: the saved value is named instead of the age
    expect("Screen now", "30 (1.0 s ago)", "50", "%", "Screen now: 30% (saved 50%)");
    // stale wins: the value may have moved since, so no "differs"
    expect(NULL, "30 (stale, 4 s ago)", "50", "%", "30% (stale 4 s)");
    expect(NULL, "30 (stale, 104 s ago)", NULL, NULL, "30 (stale 104 s)");
    // unread and unknown
    expect("Boot light", "? (not read yet)", "1", NULL, "Boot light: ? (not read yet)");
    expect(NULL, "? (no answer from the C6)", NULL, NULL, "? (no answer from the C6)");
    expect(NULL, "?", NULL, NULL, "?");
    expect("X", "", NULL, NULL, "X: ?");
    expect(NULL, NULL, NULL, NULL, "?");
    // a value with brackets of its own keeps them (the last " (" is the note's)
    expect(NULL, "a (b) (1.0 s ago)", NULL, NULL, "a (b) (1.0 s)");
    // a small output buffer truncates, never overruns
    char tiny[8];
    size_t n = reg_row_text("Label", "123456 (3.2 s ago)", NULL, NULL, tiny, sizeof(tiny));
    CHECK(n == strlen(tiny) && n < sizeof(tiny));
    CHECK(reg_row_text(NULL, "1", NULL, NULL, tiny, 0) == 0);
    return failures ? 1 : 0;
}
