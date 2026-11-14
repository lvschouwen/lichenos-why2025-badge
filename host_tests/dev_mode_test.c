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
    {"help", "help", noop, false, 0},
    {"ui", "ui [dump|perf]", noop, true, 0},
    {"reboot", "reboot", noop, false, 0},
    {"keys", "keys send", noop, true, 0},
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
// The USB hold (#289): while a guard holds, a console factory reset only from the USB console; without one, from any
static void usb_hold(void) {
    CHECK(!console_hold_refuses(true, CONSOLE_ORIGIN_UART));
    CHECK(console_hold_refuses(true, CONSOLE_ORIGIN_WIFI));
    CHECK(console_hold_refuses(true, CONSOLE_ORIGIN_LOCAL)); // injected keys can type in the Console app
    CHECK(console_hold_refuses(true, CONSOLE_ORIGIN_UNKNOWN));
    CHECK(!console_hold_refuses(false, CONSOLE_ORIGIN_WIFI)); // nothing holds: as before
    CHECK(!console_hold_refuses(false, CONSOLE_ORIGIN_LOCAL));
    CHECK(!console_hold_refuses(false, CONSOLE_ORIGIN_UART));
}

// #420: while the hold is on, "fw slot other" only from the USB console (another image may not keep the list); the
// listing and the usage answer from anywhere
static void slot_other_held(void) {
    CHECK(console_slot_request("other", true, CONSOLE_ORIGIN_UART) == SLOT_REQ_OTHER);
    CHECK(console_slot_request("other", true, CONSOLE_ORIGIN_WIFI) == SLOT_REQ_REFUSED);
    CHECK(console_slot_request("other", true, CONSOLE_ORIGIN_LOCAL) == SLOT_REQ_REFUSED);
    CHECK(console_slot_request("other", true, CONSOLE_ORIGIN_UNKNOWN) == SLOT_REQ_REFUSED);
    CHECK(console_slot_request("other", false, CONSOLE_ORIGIN_WIFI) == SLOT_REQ_OTHER); // nothing holds: as before
    CHECK(console_slot_request("other", false, CONSOLE_ORIGIN_LOCAL) == SLOT_REQ_OTHER);
    CHECK(console_slot_request("", true, CONSOLE_ORIGIN_WIFI) == SLOT_REQ_SHOW);
    CHECK(console_slot_request("other ", true, CONSOLE_ORIGIN_UART) == SLOT_REQ_USAGE);
    CHECK(console_slot_request("others", false, CONSOLE_ORIGIN_UART) == SLOT_REQ_USAGE);
}

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

// Per-app prefix (#423): "mesh" and "advert" belong to app 1 (meshcore), "qr" to app 2, the rest to the OS
static console_entry_t const app_table[] = {
    {"help", "help", noop, false, 0},
    {"mesh", "mesh send ...", noop, false, 1},
    {"advert", "advert", noop, false, 1},
    {"mdbg", "mdbg", noop, true, 1}, // an app's debug command
    {"qr", "qr <text>", noop, false, 2},
};
#define AN ((int)(sizeof(app_table) / sizeof(app_table[0])))

static void app_ids_and_intern(void) {
    char const *ids[CONSOLE_MAX_APPS] = {0};
    int         n = 0;
    CHECK(dev_mode_app_of(ids, n, "meshcore") == 0); // nothing registered: no prefix is known
    CHECK(dev_mode_app_intern(ids, &n, CONSOLE_MAX_APPS, "meshcore") == 1 && n == 1);
    CHECK(dev_mode_app_intern(ids, &n, CONSOLE_MAX_APPS, "qrcode") == 2 && n == 2);
    CHECK(dev_mode_app_intern(ids, &n, CONSOLE_MAX_APPS, "meshcore") == 1 && n == 2); // once
    CHECK(dev_mode_app_of(ids, n, "qrcode") == 2 && dev_mode_app_of(ids, n, "meshcore") == 1);
    CHECK(dev_mode_app_of(ids, n, "mesh") == 0 && dev_mode_app_of(ids, n, "meshcor") == 0); // whole ids only
    CHECK(dev_mode_app_of(ids, n, "help") == 0);
    n = 1;
    CHECK(dev_mode_app_intern(ids, &n, 1, "other") == 0 && n == 1); // full table
}

static void app_prefix_dispatch(void) {
    int i = -1;
    // the old spelling runs every command, whoever owns it
    CHECK(dev_mode_find_app(app_table, AN, "mesh", false, 0, &i) == DEV_FIND_RUN && i == 1);
    CHECK(dev_mode_find_app(app_table, AN, "qr", false, 0, &i) == DEV_FIND_RUN && i == 4);
    CHECK(dev_mode_find_app(app_table, AN, "help", false, 0, &i) == DEV_FIND_RUN);
    // the prefixed spelling reaches the same entry
    CHECK(dev_mode_find_app(app_table, AN, "mesh", false, 1, &i) == DEV_FIND_RUN && i == 1);
    CHECK(dev_mode_find_app(app_table, AN, "qr", false, 2, &i) == DEV_FIND_RUN && i == 4);
    // another app's command, or an OS one, under the wrong prefix is unknown
    CHECK(dev_mode_find_app(app_table, AN, "qr", false, 1, &i) == DEV_FIND_NONE);
    CHECK(dev_mode_find_app(app_table, AN, "mesh", false, 2, &i) == DEV_FIND_NONE);
    CHECK(dev_mode_find_app(app_table, AN, "help", false, 1, &i) == DEV_FIND_NONE);
    CHECK(dev_mode_find_app(app_table, AN, "mdbg", true, 2, &i) == DEV_FIND_NONE); // not even a debug one's existence
    CHECK(dev_mode_find_app(app_table, AN, "", false, 1, &i) == DEV_FIND_NONE);
    // developer mode gates the prefixed spelling exactly like the plain one
    CHECK(dev_mode_find_app(app_table, AN, "mdbg", false, 0, &i) == DEV_FIND_DEV_OFF);
    CHECK(dev_mode_find_app(app_table, AN, "mdbg", false, 1, &i) == DEV_FIND_DEV_OFF);
    CHECK(dev_mode_find_app(app_table, AN, "mdbg", true, 0, &i) == DEV_FIND_RUN);
    CHECK(dev_mode_find_app(app_table, AN, "mdbg", true, 1, &i) == DEV_FIND_RUN);
}

static void count_line(char const *usage, void *ctx) {
    (void)usage;
    ++*(int *)ctx;
}

static void app_help_groups(void) {
    int n = 0;
    CHECK(dev_mode_help_app(app_table, AN, false, 0, count_line, &n) == 1 && n == 1);   // the OS's group
    n = 0;
    CHECK(dev_mode_help_app(app_table, AN, false, 1, count_line, &n) == 2 && n == 2);   // debug one hidden
    CHECK(dev_mode_help_app(app_table, AN, true, 1, NULL, NULL) == 3);                  // NULL line: only counts
    CHECK(dev_mode_help_app(app_table, AN, false, 2, NULL, NULL) == 1);
    CHECK(dev_mode_help_app(app_table, AN, false, 3, NULL, NULL) == 0);                 // an app with nothing: no group
    // every command is in exactly one group
    CHECK(dev_mode_help_app(app_table, AN, true, 0, NULL, NULL) + dev_mode_help_app(app_table, AN, true, 1, NULL, NULL) +
              dev_mode_help_app(app_table, AN, true, 2, NULL, NULL) == AN);
}

int main(void) {
    app_ids_and_intern();
    app_prefix_dispatch();
    app_help_groups();
    gate_off_and_on();
    help_hides_debug_commands();
    on_only_from_usb();
    usb_hold();
    slot_other_held();
    origin_plumbing();
    factory_reset_clears_it();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_mode_test ok\n");
    return 0;
}
