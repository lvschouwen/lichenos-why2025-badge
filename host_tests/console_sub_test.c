// Host test (#464): a command's sub-command table: what a row matches and hands on, and the gate, origin and hold
// decision run_command takes from it
#include "console_os_rules.h"
#include "console_sub.h"

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

static bool rest_is(console_sub_t const *sub, char const *args, char const *want) {
    char const *r = console_sub_match(sub, args);
    return want ? r && strcmp(r, want) == 0 : r == NULL;
}

static void test_match(void) {
    console_sub_t const exact = {"test panic", CON_MATCH_EXACT, 0, 0, 0};
    CHECK(rest_is(&exact, "test panic", ""));
    CHECK(rest_is(&exact, "test panic ", NULL));
    CHECK(rest_is(&exact, "test", NULL));
    console_sub_t const prefix = {"send ", CON_MATCH_PREFIX, 0, 0, 0};
    CHECK(rest_is(&prefix, "send #ch hi", "#ch hi"));
    CHECK(rest_is(&prefix, "send", NULL));
    CHECK(rest_is(&prefix, "sendx", NULL));
    console_sub_t const word = {"cat", CON_MATCH_WORD, 0, 0, 0};
    CHECK(rest_is(&word, "cat", ""));
    CHECK(rest_is(&word, "cat /sd/x", "/sd/x"));
    CHECK(rest_is(&word, "catalog", NULL));
    console_sub_t const raw = {"radio", CON_MATCH_WORD_RAW, 0, 0, 0};
    CHECK(rest_is(&raw, "radio eu", " eu"));
    CHECK(rest_is(&raw, "radio", ""));
    console_sub_t const rest = {NULL, CON_MATCH_REST, 0, 0, 0};
    CHECK(rest_is(&rest, "", ""));
    CHECK(rest_is(&rest, "anything at all", "anything at all"));
    console_sub_t const gone = {NULL, CON_MATCH_EXACT, 0, 0, 0}; // compiled out
    CHECK(rest_is(&gone, "", NULL));
    console_sub_t const end = CONSOLE_SUB_END;
    CHECK(rest_is(&end, "", NULL));
}

static void test_find(void) {
    static console_sub_t const table[] = {
        {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
        {"status", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
        {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
        CONSOLE_SUB_END,
    };
    CHECK(console_sub_find(table, "") == &table[0]);
    CHECK(console_sub_find(table, "status") == &table[1]);
    CHECK(console_sub_find(table, "status x") == &table[2]); // fail closed
    CHECK(console_sub_find(NULL, "x") == NULL);
    static console_sub_t const open_end[] = {{"x", CON_MATCH_EXACT, 0, 0, 0}, CONSOLE_SUB_END};
    CHECK(console_sub_find(open_end, "y") == NULL);
}

static void test_origin(void) {
    CHECK(console_origin_allows(CON_ORIGIN_ANY, CONSOLE_ORIGIN_WIFI));
    CHECK(console_origin_allows(CON_ORIGIN_ANY, CONSOLE_ORIGIN_UNKNOWN));
    CHECK(console_origin_allows(CON_ORIGIN_LOCAL, CONSOLE_ORIGIN_UART));
    CHECK(console_origin_allows(CON_ORIGIN_LOCAL, CONSOLE_ORIGIN_LOCAL));
    CHECK(!console_origin_allows(CON_ORIGIN_LOCAL, CONSOLE_ORIGIN_WIFI));
    CHECK(!console_origin_allows(CON_ORIGIN_LOCAL, CONSOLE_ORIGIN_UNKNOWN));
    CHECK(console_origin_allows(CON_ORIGIN_UART, CONSOLE_ORIGIN_UART));
    CHECK(!console_origin_allows(CON_ORIGIN_UART, CONSOLE_ORIGIN_LOCAL)); // injected keys can type there (rule 4)
    CHECK(!console_origin_allows(CON_ORIGIN_UART, CONSOLE_ORIGIN_WIFI));
    CHECK(!console_origin_allows(CON_ORIGIN_UART, CONSOLE_ORIGIN_UNKNOWN));
    CHECK(!console_origin_allows(7, CONSOLE_ORIGIN_UART)); // an unknown rule fails closed
}

static void test_rule(void) {
    console_sub_t const dev_uart = {"x", CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_UART, 0};
    console_ask_t       ask      = {.dev_on = false, .origin = CONSOLE_ORIGIN_WIFI, .held = false};
    CHECK(console_sub_rule(&dev_uart, &ask) == CON_RULE_DEV); // the gate comes first
    ask.dev_on = true;
    CHECK(console_sub_rule(&dev_uart, &ask) == CON_RULE_ORIGIN);
    ask.origin = CONSOLE_ORIGIN_UART;
    CHECK(console_sub_rule(&dev_uart, &ask) == CON_RULE_RUN);
    CHECK(console_sub_rule(NULL, &ask) == CON_RULE_RUN);

    console_sub_t const hold = {"other", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_USB_HOLD};
    console_ask_t       h    = {.dev_on = false, .origin = CONSOLE_ORIGIN_WIFI, .held = true};
    CHECK(console_sub_rule(&hold, &h) == CON_RULE_HOLD);
    h.origin = CONSOLE_ORIGIN_UART;
    CHECK(console_sub_rule(&hold, &h) == CON_RULE_RUN);
    h.origin = CONSOLE_ORIGIN_LOCAL;
    h.held   = false;
    CHECK(console_sub_rule(&hold, &h) == CON_RULE_RUN);

    console_sub_t const odd_gate = {"x", CON_MATCH_EXACT, 9, CON_ORIGIN_ANY, 0};
    console_ask_t       off      = {.dev_on = false, .origin = CONSOLE_ORIGIN_UART, .held = false};
    CHECK(console_sub_rule(&odd_gate, &off) == CON_RULE_DEV); // an unknown gate fails closed
}

static bool says(char const *got, char const *want) {
    if (strcmp(got, want) == 0) {
        return true;
    }
    printf("  got  \"%s\"\n  want \"%s\"\n", got, want);
    return false;
}

// The refusals run_command prints (console.c rule_refused), word for word
static void test_refusal(void) {
    char out[CONSOLE_SUB_REFUSAL_MAX];
    console_sub_t const *dev_on = console_sub_find(OS_DEV_RULES, "on");
    CHECK(dev_on != NULL);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_ORIGIN, "dev", dev_on, NULL),
               "dev on refused: only from the USB console (or About: press Enter 7 times)"));
#else
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_ORIGIN, "dev", dev_on, NULL),
               "dev on refused: only from the USB console")); // a release names no developer mode
#endif
    console_sub_t const *other = console_sub_find(OS_SLOT_RULES, "other");
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_HOLD, "slot", other, "the on-air allow list"),
               "slot other refused: the on-air allow list is on; switch it off at the USB console first (fw onair off)"));
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_HOLD, "slot", other, NULL),
               "slot other refused: ? is on; switch it off at the USB console first (fw onair off)")); // never a NULL
    console_sub_t const erase = {"format", CON_MATCH_WORD, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_ERASE};
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_ORIGIN, "sd", &erase, NULL),
               "sd format refused: it erases storage, so only the USB console can do it (the screen's Settings can too)"));
    console_sub_t const rest = {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0};
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_DEV, "psram", &rest, NULL),
               "psram: " CONSOLE_DEV_OFF_REPLY)); // a REST row names the command alone
    console_sub_t const prefix = {"crash ", CON_MATCH_PREFIX, CON_GATE_DEV, CON_ORIGIN_ANY, 0};
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_DEV, "app", &prefix, NULL),
               "app crash: " CONSOLE_DEV_OFF_REPLY)); // without the PREFIX word's own space
    console_sub_t const local = {"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_LOCAL, 0};
    CHECK(says(console_sub_refusal(out, sizeof(out), CON_RULE_ORIGIN, "c", &local, NULL),
               "c x refused: only on the badge (the USB console or the Console app)"));
    char small[12];
    console_sub_refusal(small, sizeof(small), CON_RULE_ORIGIN, "token", &local, NULL);
    CHECK(strlen(small) == sizeof(small) - 1); // cut, still a string
}

int main(void) {
    test_refusal();
    test_match();
    test_find();
    test_origin();
    test_rule();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("console_sub: all passed\n");
    return 0;
}
