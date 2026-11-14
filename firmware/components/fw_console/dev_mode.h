#pragma once
// Developer mode (#373): debug and test commands run only while it is on, and are hidden from "fw help" while it is
// off. Release builds (LICHEN_DEV_COMMANDS=n) leave them out of the image. The rules are here, pure and host-tested
// (host_tests/dev_mode_test.c); console.c holds the table, its lock and the saved state.
//
// Why on is refused over Wi-Fi: the Wi-Fi console is authenticated by the OTA token, which also installs firmware.
// Developer mode is release hygiene, so turning it on needs a person at the USB cable or the owner's taps in About.
// Off works from anywhere.

#include <stdbool.h>
#include <stddef.h>

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
#define DEV_MODE_ENTER_HINT "About: press Enter 7 times" // what the About screen counts (the one place that says it)
#define DEV_MODE_OFF_NOTE   "developer mode is off (" DEV_MODE_ENTER_HINT ", or USB: fw dev on)"

// One console table entry. fn takes the text after the command name.
typedef struct {
    char const *name;
    char const *usage;
    void (*fn)(char const *args);
    bool dev; // registered with console_register_dev
    unsigned char app; // 0: the OS's; else 1 + the index of the app id in the app table (#423), in the padding after dev
    unsigned char rules; // 0: none; else 1 + the index of its sub-command table in console.c's list (#464), padding too
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

// Per-app prefix (#423): an app with the CONSOLE capability registers its commands with its app id, and "fw <app id>
// <command>" runs them. The plain "fw <command>" spelling keeps working for every command (a script, badge.sh and the
// on-air guard use it), so each command has two spellings and the same gate: dev_mode_find_app is dev_mode_find
// plus the owner check, and everything after it (developer mode, the USB hold, origins) is the command's own.
#define CONSOLE_MAX_APPS 8
// 1 + the index of word in ids[0..n), or 0 when word is no app id
int dev_mode_app_of(char const *const *ids, int n, char const *word);
// Adds id to ids (a string literal, kept by pointer) unless it is there; returns 1 + its index, 0 when the table of max
// is full. *n counts the ids.
int dev_mode_app_intern(char const **ids, int *n, int max, char const *id);
// True when registering the command name (of app, NULL: the OS's) would make a word mean two things: name is an app id,
// or app is a command name (or the name itself). The prefix is read before the command, so either would hide one.
bool dev_mode_name_clash(console_entry_t const *table, int n, char const *const *ids, int n_ids, char const *app,
                         char const *name);
// Like dev_mode_find for "fw <app> <name>": app is dev_mode_app_of's result (0: the plain spelling, any command). A
// command of another app, or the OS's, is NONE under the wrong prefix; a debug command of the right app is DEV_OFF.
dev_find_t dev_mode_find_app(console_entry_t const *table, int n, char const *name, bool on, int app, int *index);
// dev_mode_help for the entries of one app (0: the OS's); line may be NULL to only count
int dev_mode_help_app(console_entry_t const *table, int n, bool on, int app, void (*line)(char const *usage, void *ctx),
                      void *ctx);

// "fw help" (#439): the names of one app's (0: the OS's) listed commands, head first, wrapped to width characters per
// line (the full usages of every command do not fit the Wi-Fi console's 4 KB reply). Debug commands only while on.
// Calls line once per output line; returns the commands listed (0: no line at all).
#define DEV_HELP_LINE_MAX 128
typedef struct {
    console_entry_t const *table;
    int                    n;
    bool                   on; // developer mode
} help_view_t;
// A name longer than a line is printed cut.
int dev_mode_help_names(help_view_t const *v, int app, char const *head, size_t width,
                        void (*line)(char const *text, void *ctx), void *ctx);
// "fw help <word>": the usages of the command word, or of every listed command of the app whose id is word. A debug
// command while off counts as unknown (what the help hides stays hidden). Returns the usages given, 0 for no topic.
int dev_mode_help_topic(help_view_t const *v, char const *const *ids, int n_ids, char const *word,
                        void (*line)(char const *usage, void *ctx), void *ctx);

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

// A command that erases or wipes storage (factory-reset ... YES, sd format) runs only from the USB console (#455,
// epic-395 rule 4): a person at the cable. Wi-Fi, the Console app and any other task are refused, held or not.
bool console_erase_refuses(console_origin_t origin);

// The hold also covers booting another image (#420): the other slot's image, or one installed over Wi-Fi, may not keep
// what holds (an older image has no on-air list), so "fw slot other" and the update server's firmware refuse while it
// holds. Every refusal ends in this one string (passed as an argument, so the image keeps one copy).
#define HOLD_OFF_FIRST "switch it off at the USB console first (fw onair off)"

typedef enum {
    SLOT_REQ_SHOW,    // "": show the slots
    SLOT_REQ_OTHER,   // "other": boot the other slot
    SLOT_REQ_REFUSED, // "other" while the hold is on, from anywhere but the USB console
    SLOT_REQ_USAGE,   // anything else
} slot_req_t;

// What "fw slot <args>" asks, given the hold and where it came from
slot_req_t console_slot_request(char const *args, bool held, console_origin_t origin);

#ifdef __cplusplus
}
#endif
