#pragma once
// Developer mode (#373): debug and test commands run only while it is on, and are hidden from "fw help" while it is
// off. Release builds (LICHEN_DEV_COMMANDS=n) leave them out of the image. The rules are here, pure and host-tested
// (host_tests/dev_mode_test.c); console.c holds the table, its lock and the saved state.
//
// Why on is refused over Wi-Fi: the Wi-Fi console is authenticated by the OTA token, which also installs firmware.
// Developer mode is release hygiene, so turning it on needs a person at the USB cable or the owner's taps in About.
// Off works from anywhere.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where the command line being run came from
typedef enum {
    CONSOLE_ORIGIN_UART,    // typed on the USB serial console: a person at the cable
    CONSOLE_ORIGIN_WIFI,    // POST /console, authenticated by the OTA token
    CONSOLE_ORIGIN_LOCAL,   // the Console app on the badge's own screen (console_exec)
    CONSOLE_ORIGIN_UNKNOWN, // not a console command's task: refused wherever an origin matters
} console_origin_t;

// The origin a console_exec run records: keep_secret is console_exec_with_secrets, POST /console's (#123)
console_origin_t dev_mode_exec_origin(bool keep_secret);
// The origin of the calling task: captured is true when it runs the console_exec whose origin is exec; uart_task when
// it is the console task that reads the UART. Anything else is UNKNOWN, so a new caller fails closed.
console_origin_t dev_mode_origin(bool captured, console_origin_t exec, bool uart_task);

#define DEV_MODE_KEY      "dev.mode" // u8 in badge_fw through the settings service; a factory reset erases it
#define DEV_MODE_OFF_NOTE "developer mode is off (About: tap Version 7 times, or USB: fw dev on)"

// One console table entry. fn takes the text after the command name.
typedef struct {
    char const *name;
    char const *usage;
    void (*fn)(char const *args);
    bool dev; // registered with console_register_dev
} console_entry_t;

typedef enum {
    DEV_FIND_NONE,    // no such command
    DEV_FIND_RUN,     // run it
    DEV_FIND_DEV_OFF, // a debug command while developer mode is off: answer DEV_MODE_OFF_NOTE
} dev_find_t;

// Looks name up in the first n entries of table; *index is the entry found (RUN and DEV_OFF)
dev_find_t dev_mode_find(console_entry_t const *table, int n, char const *name, bool on, int *index);

// Calls line(usage, ctx) for every entry "fw help" lists: all of them while on, the plain ones while off. Returns how
// many it listed.
int dev_mode_help(console_entry_t const *table, int n, bool on, void (*line)(char const *usage, void *ctx), void *ctx);

typedef enum {
    DEV_REQ_STATUS,  // "", "status"
    DEV_REQ_ON,      // "on" from the USB console
    DEV_REQ_OFF,     // "off", from anywhere
    DEV_REQ_REFUSED, // "on" from Wi-Fi or the Console app
    DEV_REQ_USAGE,   // anything else
} dev_req_t;

// What "fw dev <args>" asks, given where it came from
dev_req_t dev_mode_request(char const *args, console_origin_t origin);

// The USB hold (#289): while a bound guard is on (held), a console command that would erase it (factory-reset ...
// YES) runs only from the USB console; Wi-Fi, the Console app (injected keys) and other tasks are refused
bool console_hold_refuses(bool held, console_origin_t origin);

#ifdef __cplusplus
}
#endif
