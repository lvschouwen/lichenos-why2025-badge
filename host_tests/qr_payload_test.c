// Host test: the text a QR code carries (#153)
#include "qr_payload.h"

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

static void test_wifi(void) {
    char out[128];
    CHECK(qr_wifi_payload("Home", "secret", out, sizeof(out)) && strcmp(out, "WIFI:T:WPA;S:Home;P:secret;;") == 0);
    CHECK(qr_wifi_payload("Cafe", "", out, sizeof(out)) && strcmp(out, "WIFI:T:nopass;S:Cafe;;") == 0);
    CHECK(qr_wifi_payload("Cafe", NULL, out, sizeof(out)) && strcmp(out, "WIFI:T:nopass;S:Cafe;;") == 0);
    // The characters the format reserves are escaped with a backslash
    CHECK(qr_wifi_payload("a;b:c", "p\\w\"d,", out, sizeof(out)) &&
          strcmp(out, "WIFI:T:WPA;S:a\\;b\\:c;P:p\\\\w\\\"d\\,;;") == 0);
}

static void test_limits(void) {
    char out[32];
    CHECK(!qr_wifi_payload("", "x", out, sizeof(out)) && out[0] == '\0');
    CHECK(!qr_wifi_payload(NULL, "x", out, sizeof(out)));
    CHECK(!qr_wifi_payload("Home", "secret", out, 0));
    // Exactly full and one over: "WIFI:T:WPA;S:Home;P:secret;;" is 28 characters
    char tight[29], small[28];
    CHECK(qr_wifi_payload("Home", "secret", tight, sizeof(tight)));
    CHECK(!qr_wifi_payload("Home", "secret", small, sizeof(small)) && small[0] == '\0');
    // An escape that does not fit whole is not cut in half
    char esc[18];
    CHECK(!qr_wifi_payload("a;;;;;;;;;;", "", esc, sizeof(esc)));
}

static void test_ok(void) {
    char big[QR_PAYLOAD_MAX + 2];
    memset(big, 'a', sizeof(big));
    big[QR_PAYLOAD_MAX]     = '\0';
    CHECK(qr_payload_ok(big));
    big[QR_PAYLOAD_MAX] = 'a';
    big[QR_PAYLOAD_MAX + 1] = '\0';
    CHECK(!qr_payload_ok(big));
    CHECK(!qr_payload_ok("") && !qr_payload_ok(NULL) && qr_payload_ok("x"));
}

int main(void) {
    test_wifi();
    test_limits();
    test_ok();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("qr_payload_test: ok\n");
    return 0;
}
