// Host test: the applied LoRa radio's texts and the mismatch with MeshCore's settings (#335)
#include "mesh_applied.h"

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

static void expect(char const *got, char const *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL: got \"%s\" want \"%s\"\n", got, want);
        failures++;
    }
}

// MeshCore's 869.618 MHz as float MHz, times 1e6: what the C6 applies, below the whole kHz
static char const *const FRESH[MESH_APPLIED_KEYS] = {"869617981 (1.2 s ago)", "62500 (1.2 s ago)", "7 (1.2 s ago)",
                                                     "5 (1.2 s ago)", "22 (1.2 s ago)"};

static void test_fresh(void) {
    mesh_applied_t a;
    char           out[96];
    mesh_applied_parse(FRESH, &a);
    CHECK(a.read && !a.stale);
    CHECK(mesh_applied_radio(&a, out, sizeof(out)) == strlen(out));
    expect(out, "869.618 MHz, 62.5 kHz, SF7, CR5"); // rounded, not cut to 869.617
    mesh_applied_power(&a, out, sizeof(out));
    expect(out, "22 dBm");
    mesh_applied_line(&a, true, out, sizeof(out));
    expect(out, "869.618 MHz, 62.5 kHz, SF7, CR5, 22 dBm (1.2 s ago)");
    mesh_applied_line(&a, false, out, sizeof(out));
    expect(out, "869.618 MHz, 62.5 kHz, SF7, CR5, 22 dBm");
    CHECK(!mesh_applied_differs(&a, 869.618f, 62.5f, 7, 5, 22));
    CHECK(mesh_applied_differs(&a, 869.525f, 250.0f, 11, 5, 22)); // another preset
    CHECK(mesh_applied_differs(&a, 869.618f, 62.5f, 7, 5, 20));   // the power alone
    CHECK(mesh_applied_differs(&a, 869.618f, 62.5f, 8, 5, 22));
    CHECK(mesh_applied_differs(&a, 869.620f, 62.5f, 7, 5, 22)); // 2 kHz off
    mesh_applied_line(&a, true, out, 8); // cut, never overrun
    CHECK(strlen(out) == 7);
}

static void test_stale(void) {
    char const    *cached[MESH_APPLIED_KEYS] = {"869618000 (stale, 9 s ago)", "250000 (stale, 9 s ago)", "11", "5", "-9"};
    mesh_applied_t a;
    char           out[96];
    mesh_applied_parse(cached, &a);
    CHECK(a.read && a.stale);
    mesh_applied_radio(&a, out, sizeof(out));
    expect(out, "869.618 MHz, 250 kHz, SF11, CR5 (stale)");
    mesh_applied_power(&a, out, sizeof(out));
    expect(out, "-9 dBm (stale)");
    mesh_applied_line(&a, true, out, sizeof(out));
    expect(out, "869.618 MHz, 250 kHz, SF11, CR5, -9 dBm (stale, 9 s ago)");
    CHECK(!mesh_applied_differs(&a, 869.525f, 62.5f, 7, 5, 22)); // an old value proves nothing
}

static void test_unread(void) {
    char const    *cached[MESH_APPLIED_KEYS] = {"869618000 (1.0 s ago)", "? (not read yet)", "? (no answer from the C6)",
                                               "5 (1.0 s ago)", "22 (1.0 s ago)"};
    mesh_applied_t a;
    char           out[96];
    mesh_applied_parse(cached, &a);
    CHECK(!a.read);
    mesh_applied_radio(&a, out, sizeof(out));
    expect(out, "? (not read yet)"); // the first unread key's reason
    mesh_applied_power(&a, out, sizeof(out));
    expect(out, "? (not read yet)");
    mesh_applied_line(&a, true, out, sizeof(out));
    expect(out, "? (not read yet)");
    CHECK(!mesh_applied_differs(&a, 869.525f, 62.5f, 7, 5, 22));

    cached[1] = NULL; // a key the registry does not have
    cached[2] = "5 (1.0 s ago)";
    mesh_applied_parse(cached, &a);
    CHECK(!a.read);
    mesh_applied_radio(&a, out, sizeof(out));
    expect(out, "?");
    CHECK(mesh_applied_radio(&a, out, 0) == 0);
}

int main(void) {
    test_fresh();
    test_stale();
    test_unread();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_applied_test: all passed\n");
    return 0;
}
