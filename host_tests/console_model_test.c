// Host test: the Console app's history, secret-line filter and capped output buffer (#150)
#include "console_model.h"

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

static void test_secret(void) {
    CHECK(console_line_is_secret("wifi password hunter2"));
    CHECK(console_line_is_secret("mesh remote X LOGIN abc"));
    CHECK(console_line_is_secret("MyPassWord"));
    CHECK(!console_line_is_secret("wifi stats"));
    CHECK(!console_line_is_secret("passwor"));
    CHECK(!console_line_is_secret(""));
}

static void test_public(void) {
    CHECK(console_line_is_public("help"));
    CHECK(console_line_is_public("fw wifi stats"));
    CHECK(console_line_is_public("wifi"));
    CHECK(console_line_is_public("mesh link"));
    CHECK(console_line_is_public("mem blocks internal"));
    CHECK(!console_line_is_public("wifi MyNet hunter2")); // an ssid + password
    CHECK(!console_line_is_public("wifi --clear"));
    CHECK(!console_line_is_public("wifi on hunter2pass")); // cmd_wifi reads this as ssid "on" + a password
    CHECK(!console_line_is_public("wifi scan secret"));
    CHECK(!console_line_is_public("wifi tx 52abc"));
    CHECK(!console_line_is_public("wifi ps none x"));
    CHECK(console_line_is_public("wifi tx 52"));
    CHECK(console_line_is_public("wifi ps min"));
    CHECK(console_line_is_public("wifi proto ax"));
    CHECK(!console_line_is_public("channel add secret AAAA"));
    CHECK(!console_line_is_public("mesh remote abc cli reboot"));
    CHECK(!console_line_is_public("mesh send chan hello"));
    CHECK(!console_line_is_public("nvs-enc burn"));
    CHECK(!console_line_is_public("token"));
    CHECK(!console_line_is_public("factory-reset all"));
    CHECK(!console_line_is_public("unknowncmd"));
    CHECK(!console_line_is_public("wifi scan password")); // keyword deny on top
    CHECK(!console_line_is_public("sd format YES")); // a confirm is typed each time
    CHECK(!console_line_is_public("thermal test shutdown YES"));
    CHECK(console_line_is_public("thermal test warn"));
    CHECK(!console_line_is_public("wifi"" ""on YES"));
    char w[24];
    console_first_word("  fw wifi a b", w, sizeof(w));
    CHECK(strcmp(w, "wifi") == 0);
}

static void test_hist(void) {
    console_hist_t h;
    console_hist_init(&h);
    CHECK(console_hist_older(&h) == NULL); // empty: nothing to walk
    CHECK(console_hist_newer(&h) == NULL);
    CHECK(console_hist_push(&h, "wifi"));
    CHECK(!console_hist_push(&h, "wifi")); // same as the newest
    CHECK(!console_hist_push(&h, ""));
    CHECK(!console_hist_push(&h, "wifi password abc")); // secret: not kept
    CHECK(!console_hist_push(&h, "sd format YES"));
    CHECK(!console_hist_push(&h, "reboot")); // not on the allow-list
    CHECK(!console_hist_push(&h, "mesh remote x login y"));
    CHECK(console_hist_push(&h, "mesh"));
    CHECK(h.count == 2);
    CHECK(strcmp(console_hist_older(&h), "mesh") == 0);
    CHECK(console_hist_browsing(&h));
    CHECK(strcmp(console_hist_older(&h), "wifi") == 0);
    CHECK(strcmp(console_hist_older(&h), "wifi") == 0); // stays at the oldest
    CHECK(strcmp(console_hist_newer(&h), "mesh") == 0);
    CHECK(strcmp(console_hist_newer(&h), "") == 0); // past the newest: back to an empty line
    CHECK(!console_hist_browsing(&h));
    CHECK(console_hist_newer(&h) == NULL);
    console_hist_older(&h);
    CHECK(console_hist_push(&h, "help")); // a run ends browsing
    CHECK(!console_hist_browsing(&h));
}

static void test_hist_ring(void) {
    console_hist_t h;
    console_hist_init(&h);
    char line[16];
    for (int i = 0; i < CONSOLE_HIST_N + 4; i++) {
        snprintf(line, sizeof(line), "mem %d", i);
        CHECK(console_hist_push(&h, line));
    }
    CHECK(h.count == CONSOLE_HIST_N);
    CHECK(strcmp(console_hist_older(&h), "mem 19") == 0);
    char const *last = NULL;
    for (int i = 0; i < 40; i++) {
        last = console_hist_older(&h);
    }
    CHECK(strcmp(last, "mem 4") == 0); // the 4 oldest fell out
    char too_long[CONSOLE_LINE_MAX + 20];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!console_hist_push(&h, too_long));
}

static void test_out(void) {
    char   buf[32];
    size_t len = 0;
    buf[0]     = '\0';
    len        = console_out_append(buf, sizeof(buf), len, "one\ntwo\n", 8);
    CHECK(len == 8 && strcmp(buf, "one\ntwo\n") == 0);
    len = console_out_append(buf, sizeof(buf), len, "three\nfour\n", 11);
    CHECK(len == 19 && strcmp(buf, "one\ntwo\nthree\nfour\n") == 0);
    // 19 + 15 + 1 > 32: the oldest whole lines go, the rest stays intact
    len = console_out_append(buf, sizeof(buf), len, "fivefivefive\n12\n", 16);
    CHECK(len < sizeof(buf));
    CHECK(strlen(buf) == len);
    CHECK(strcmp(buf + len - 16, "fivefivefive\n12\n") == 0);
    CHECK(buf[0] == 'f' || buf[0] == 't' || buf[0] == 'o'); // starts at a line start (checked next)
    CHECK(strstr(buf, "hree") == NULL || strstr(buf, "three") != NULL); // no half line

    // one chunk bigger than the buffer: its newest lines from a line start
    char big[200];
    memset(big, 0, sizeof(big));
    for (int i = 0; i < 20; i++) {
        char l[16];
        snprintf(l, sizeof(l), "line%02d\n", i);
        strcat(big, l);
    }
    len = console_out_append(buf, sizeof(buf), 0, big, strlen(big));
    CHECK(len <= sizeof(buf) - 1);
    CHECK(strncmp(buf, "line", 4) == 0);
    CHECK(strcmp(buf + len - 7, "line19\n") == 0);

    // control characters
    len = console_out_append(buf, sizeof(buf), 0, "a\rb\tc\x01\x7f\n", 8);
    CHECK(strcmp(buf, "a b c ?\n") == 0 || strcmp(buf, "a b c  ?\n") == 0);

    // one huge line without a newline: UTF-8 is not cut in the middle
    char wide[100];
    for (size_t i = 0; i + 1 < sizeof(wide); i += 2) {
        wide[i]     = (char)0xC3;
        wide[i + 1] = (char)0xA9;
    }
    len = console_out_append(buf, sizeof(buf), 0, wide, sizeof(wide) - 1 - (sizeof(wide) - 1) % 2);
    CHECK(len > 0 && (unsigned char)buf[0] == 0xC3);
    CHECK(console_out_append(buf, 1, 0, "x", 1) == 0); // no room at all
}

int main(void) {
    test_secret();
    test_public();
    test_hist();
    test_hist_ring();
    test_out();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("console_model_test: ok\n");
    return 0;
}
