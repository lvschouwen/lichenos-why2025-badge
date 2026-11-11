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

static void test_lines(void) {
    char out[96];
    char const *lora[RR_LORA_KEYS] = {"869618000 (1.2 s ago)", "62500 (1.2 s ago)", "8 (1.2 s ago)", "5 (1.2 s ago)", "22 (1.2 s ago)"};
    reg_lora_line("LoRa", lora, out, sizeof(out));
    CHECK(strcmp(out, "LoRa: 869.618 MHz 62.5 kHz SF8 CR5 22 dBm (1.2 s)") == 0);
    lora[1] = "125000 (stale, 9 s ago)";
    reg_lora_line(NULL, lora, out, sizeof(out));
    CHECK(strcmp(out, "869.618 MHz 125 kHz SF8 CR5 22 dBm (stale)") == 0);
    lora[2] = "? (no answer from the C6)";
    reg_lora_line("LoRa", lora, out, sizeof(out));
    CHECK(strcmp(out, "LoRa: ? (no answer from the C6)") == 0);
    lora[2] = NULL; // an unknown key
    reg_lora_line("LoRa", lora, out, sizeof(out));
    CHECK(strcmp(out, "LoRa: ?") == 0);

    char const *wifi[RR_WIFI_KEYS] = {"NL (2.0 s ago)", "80 (2.0 s ago)", "20 (2.0 s ago)", "7 (2.0 s ago)", "min (2.0 s ago)"};
    reg_wifi_line("Radio", wifi, out, sizeof(out));
    CHECK(strcmp(out, "Radio: NL, 20 dBm, 20 MHz, bgn, ps min (2.0 s)") == 0);
    wifi[1] = "74 (2.0 s ago)";
    wifi[3] = "71 (2.0 s ago)"; // b g n + ax
    reg_wifi_line(NULL, wifi, out, sizeof(out));
    CHECK(strcmp(out, "NL, 18.50 dBm, 20 MHz, bgnax, ps min (2.0 s)") == 0);
    reg_wifi_line("W", wifi, out, 6); // truncates, never overruns
    CHECK(strlen(out) < 6);

    reg_row_value("120 (3.0 s ago)", out, sizeof(out));
    CHECK(strcmp(out, "120") == 0);
    reg_row_value("? (not read yet)", out, sizeof(out));
    CHECK(out[0] == '\0');
    reg_row_value(NULL, out, sizeof(out));
    CHECK(out[0] == '\0');
    reg_mhz_text("PSRAM", "80000000 (3.0 s ago)", "80", out, sizeof(out));
    CHECK(strcmp(out, "PSRAM: 80 MHz (3.0 s)") == 0);
    reg_mhz_text("PSRAM", "80000000 (3.0 s ago)", "120", out, sizeof(out));
    CHECK(strcmp(out, "PSRAM: 80 MHz (wants 120 MHz)") == 0);
    reg_mhz_text(NULL, "80000000 (stale, 4 s ago)", "120", out, sizeof(out));
    CHECK(strcmp(out, "80 MHz (stale 4 s)") == 0);
    reg_mhz_text(NULL, "? (not read yet)", "120", out, sizeof(out));
    CHECK(strcmp(out, "? (not read yet)") == 0);
}

int main(void) {
    test_lines();
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
