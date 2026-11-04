// Host test: one console_exec run's output (#38) and its secret lines (#123)
#include "console_capture.h"

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

static void add(console_capture_t *c, char const *text, bool secret) {
    console_capture_add(c, text, strlen(text), secret);
}

static bool holds(console_capture_t const *c, char const *text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= c->len; i++) {
        if (memcmp(c->buf + i, text, n) == 0) {
            return true;
        }
    }
    return false;
}

// The Console app (keep_secret false): the key never lands in its buffer, one note stands in for all secret lines
static void screen_hides_secrets(void) {
    char              buf[512];
    console_capture_t c = {.buf = buf, .cap = sizeof(buf)};
    add(&c, "channel 1: test", false);
    add(&c, "key (hex) 0123456789abcdef", true);
    add(&c, "key (base64) ASNFZ4mrze8=", true);
    add(&c, "done", false);
    CHECK(!holds(&c, "0123456789abcdef") && !holds(&c, "ASNFZ4mrze8="));
    char const want[] = "channel 1: test\n" CONSOLE_SECRET_NOTE "\ndone\n";
    CHECK(c.len == sizeof(want) - 1 && memcmp(buf, want, c.len) == 0);
}

// The Wi-Fi reply (keep_secret true): the one who asked gets every line, in order
static void wifi_keeps_secrets(void) {
    char              buf[512];
    console_capture_t c = {.buf = buf, .cap = sizeof(buf), .keep_secret = true};
    add(&c, "a", false);
    add(&c, "token 00ff", true);
    add(&c, "b", false);
    char const want[] = "a\ntoken 00ff\nb\n";
    CHECK(c.len == sizeof(want) - 1 && memcmp(buf, want, c.len) == 0);
    CHECK(!holds(&c, "hidden"));
}

// A full buffer ends with the cut note; nothing is written past cap
static void cut(void) {
    char              buf[sizeof(CONSOLE_CUT_NOTE) + 8];
    memset(buf, 'x', sizeof(buf));
    console_capture_t c = {.buf = buf, .cap = sizeof(buf) - 2};
    add(&c, "1234", false);
    add(&c, "5678", false);
    add(&c, "more", false);
    CHECK(c.cut && c.len <= c.cap && buf[sizeof(buf) - 1] == 'x' && buf[sizeof(buf) - 2] == 'x');
    CHECK(c.len >= sizeof(CONSOLE_CUT_NOTE) - 1 &&
          memcmp(buf + c.len - (sizeof(CONSOLE_CUT_NOTE) - 1), CONSOLE_CUT_NOTE, sizeof(CONSOLE_CUT_NOTE) - 1) == 0);
    console_capture_t tiny = {.buf = buf, .cap = 4};
    add(&tiny, "a", false);
    CHECK(tiny.len == 0);
}

// A secret line goes only to the one who asked, never to the log tap (#123)
static void route(void) {
    console_route_t uart_line = console_route(true, false); // typed on the UART
    CHECK(uart_line.uart && !uart_line.tap && !uart_line.capture);
    console_route_t exec_line = console_route(true, true); // the Wi-Fi console or the Console app asked
    CHECK(!exec_line.uart && !exec_line.tap && exec_line.capture);
    console_route_t pub = console_route(false, true);
    CHECK(pub.uart && pub.tap && pub.capture);
    pub = console_route(false, false);
    CHECK(pub.uart && pub.tap && !pub.capture);
}

int main(void) {
    route();
    screen_hides_secrets();
    wifi_keeps_secrets();
    cut();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("console_capture_test: all checks passed\n");
    return 0;
}
