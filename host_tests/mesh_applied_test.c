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

// MeshCore's float MHz and kHz never land on whole Hz: within 500 Hz and 50 Hz is the same radio
static void test_tolerance(void) {
    char const    *cached[MESH_APPLIED_KEYS] = {"869618400 (1.0 s ago)", "62540 (1.0 s ago)", "7", "5", "22"};
    mesh_applied_t a;
    mesh_applied_parse(cached, &a);
    CHECK(a.read && a.freq_hz == 869618400u && a.bw_hz == 62540u);
    CHECK(!mesh_applied_differs(&a, 869.618f, 62.5f, 7, 5, 22));
    cached[0] = "869618600 (1.0 s ago)"; // 600 Hz off
    mesh_applied_parse(cached, &a);
    CHECK(mesh_applied_differs(&a, 869.618f, 62.5f, 7, 5, 22));
    cached[0] = "869618000";
    cached[1] = "62560"; // 60 Hz off
    mesh_applied_parse(cached, &a);
    CHECK(mesh_applied_differs(&a, 869.618f, 62.5f, 7, 5, 22));
    cached[0] = "4000000000 (1.0 s ago)"; // a U32 past INT32_MAX still parses
    cached[1] = "62500";
    mesh_applied_parse(cached, &a);
    CHECK(a.read && a.freq_hz == 4000000000u);
}

// Anything that is not "<number>" or "<number> (<note>)" counts as unread, never as a value
static void test_malformed(void) {
    char const *bad[] = {"abc", "12 (", "", "12x", "12 (1.0 s ago", "(1.0 s ago)"};
    char        out[96];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char const    *cached[MESH_APPLIED_KEYS] = {"869618000", "62500", "7", "5", bad[i]};
        mesh_applied_t a;
        mesh_applied_parse(cached, &a);
        if (a.read) {
            printf("FAIL: \"%s\" read as a value\n", bad[i]);
            failures++;
        }
        mesh_applied_power(&a, out, sizeof(out));
        CHECK(out[0] == '?');
    }
}

int main(void) {
    test_tolerance();
    test_malformed();
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
