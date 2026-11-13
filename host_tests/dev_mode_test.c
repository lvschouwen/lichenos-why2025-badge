// Host test: developer mode (#373). Debug commands run and are listed only while it is on; `fw dev on` is refused
// from Wi-Fi and the Console app; a factory reset erases the saved state.
#include "dev_mode.h"
#include "factory_plan.h"

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

static void noop(char const *args) {
    (void)args;
}

static console_entry_t const table[] = {
    {"help", "help", noop, false},
    {"ui", "ui [dump|perf]", noop, true},
    {"reboot", "reboot", noop, false},
    {"keys", "keys send", noop, true},
};
#define N ((int)(sizeof(table) / sizeof(table[0])))

static void gate_off_and_on(void) {
    int i = -1;
    CHECK(dev_mode_find(table, N, "ui", false, &i) == DEV_FIND_DEV_OFF && i == 1);
    CHECK(dev_mode_find(table, N, "ui", true, &i) == DEV_FIND_RUN && i == 1);
    CHECK(dev_mode_find(table, N, "reboot", false, &i) == DEV_FIND_RUN && i == 2); // a plain command always runs
    CHECK(dev_mode_find(table, N, "reboot", true, &i) == DEV_FIND_RUN);
    CHECK(dev_mode_find(table, N, "nope", true, &i) == DEV_FIND_NONE);
    CHECK(dev_mode_find(table, N, "u", true, &i) == DEV_FIND_NONE);  // whole names only
    CHECK(dev_mode_find(table, 1, "ui", true, &i) == DEV_FIND_NONE); // only the published count is searched
}

typedef struct {
    char seen[256];
} help_ctx_t;

static void collect(char const *usage, void *ctx) {
    help_ctx_t *h = ctx;
    strncat(h->seen, usage, sizeof(h->seen) - strlen(h->seen) - 2);
    strcat(h->seen, ";");
}

static void help_hides_debug_commands(void) {
    help_ctx_t off = {{0}};
    CHECK(dev_mode_help(table, N, false, collect, &off) == 2);
    CHECK(strcmp(off.seen, "help;reboot;") == 0);
    help_ctx_t on = {{0}};
    CHECK(dev_mode_help(table, N, true, collect, &on) == 4);
    CHECK(strcmp(on.seen, "help;ui [dump|perf];reboot;keys send;") == 0);
}

static void on_only_from_usb(void) {
    CHECK(dev_mode_request("on", CONSOLE_ORIGIN_UART) == DEV_REQ_ON);
    CHECK(dev_mode_request("on", CONSOLE_ORIGIN_WIFI) == DEV_REQ_REFUSED);  // the OTA token is not enough
    CHECK(dev_mode_request("on", CONSOLE_ORIGIN_LOCAL) == DEV_REQ_REFUSED); // the Console app: About's taps instead
    CHECK(dev_mode_request("off", CONSOLE_ORIGIN_UART) == DEV_REQ_OFF);
    CHECK(dev_mode_request("off", CONSOLE_ORIGIN_WIFI) == DEV_REQ_OFF); // off works from anywhere
    CHECK(dev_mode_request("off", CONSOLE_ORIGIN_LOCAL) == DEV_REQ_OFF);
    CHECK(dev_mode_request("", CONSOLE_ORIGIN_WIFI) == DEV_REQ_STATUS);
    CHECK(dev_mode_request("status", CONSOLE_ORIGIN_WIFI) == DEV_REQ_STATUS);
    CHECK(dev_mode_request("onn", CONSOLE_ORIGIN_UART) == DEV_REQ_USAGE);
    CHECK(dev_mode_request("on ", CONSOLE_ORIGIN_UART) == DEV_REQ_USAGE);
}

// console.c records an exec run's origin and asks it back per task: swapping WIFI and LOCAL, or letting a task that is
// not the console's own count as the UART, would let `fw dev on` in over Wi-Fi
static void origin_plumbing(void) {
    CHECK(dev_mode_exec_origin(true) == CONSOLE_ORIGIN_WIFI);   // console_exec_with_secrets: POST /console
    CHECK(dev_mode_exec_origin(false) == CONSOLE_ORIGIN_LOCAL); // console_exec: the Console app
    CHECK(dev_mode_origin(true, CONSOLE_ORIGIN_WIFI, false) == CONSOLE_ORIGIN_WIFI);
    CHECK(dev_mode_origin(true, CONSOLE_ORIGIN_LOCAL, false) == CONSOLE_ORIGIN_LOCAL);
    CHECK(dev_mode_origin(false, CONSOLE_ORIGIN_WIFI, true) == CONSOLE_ORIGIN_UART);     // the console task's own line
    CHECK(dev_mode_origin(false, CONSOLE_ORIGIN_WIFI, false) == CONSOLE_ORIGIN_UNKNOWN); // some other task: closed
    CHECK(dev_mode_request("on", dev_mode_origin(true, dev_mode_exec_origin(true), false)) == DEV_REQ_REFUSED);
    CHECK(dev_mode_request("on", dev_mode_origin(false, CONSOLE_ORIGIN_LOCAL, false)) == DEV_REQ_REFUSED);
    CHECK(dev_mode_request("on", dev_mode_origin(false, CONSOLE_ORIGIN_LOCAL, true)) == DEV_REQ_ON);
}

static void factory_reset_clears_it(void) {
    CHECK(factory_nvs_erases("badge_fw", DEV_MODE_KEY, FACTORY_SETTINGS));
    CHECK(factory_nvs_erases("badge_fw", DEV_MODE_KEY, FACTORY_ALL));
    CHECK(strlen(DEV_MODE_KEY) <= 15); // NVS keys hold at most 15 characters
}

int main(void) {
    gate_off_and_on();
    help_hides_debug_commands();
    on_only_from_usb();
    origin_plumbing();
    factory_reset_clears_it();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_mode_test ok\n");
    return 0;
}
