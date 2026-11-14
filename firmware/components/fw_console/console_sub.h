#pragma once
// A command's sub-command table (#464): which forms of a command need developer mode, which origins may run them, and
// what they do (transmit, restart, erase). run_command checks the row a line matches once, before the command's own
// code runs, so no handler carries a gate or an origin check of its own. The same rows can drive the command's own
// dispatch (MeshCore's "fw mesh" table): a row's match also says what the handler gets. Pure and host-tested
// (host_tests/console_sub_test.c, and the pin of every command in host_tests/console_pin_test.c).
//
// tools/check_console_rules.py fails the build for a row that transmits or restarts with origin ANY unless the row is
// on its commented allow list.

#include "dev_mode.h"

#include <stdbool.h>
#include <stdint.h>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// How a row's word matches the arguments (the text after the command name), and what the handler gets:
// EXACT: the whole text, nothing; PREFIX: the word with its space ("send "), what follows; WORD: the word alone or with
// a space, what follows that space; WORD_RAW: as WORD, with the space; REST: anything (the row has no word), all of it.
// A command with a rule lists its open forms first and ends with a REST row when every other form takes the rule
// (fail closed: a form added later, or a spelling the command's own parser also takes, is covered).
// END ends the table.
typedef enum {
    CON_MATCH_EXACT,
    CON_MATCH_PREFIX,
    CON_MATCH_WORD,
    CON_MATCH_WORD_RAW,
    CON_MATCH_REST,
    CON_MATCH_END,
} console_match_t;

typedef enum {
    CON_GATE_OPEN,
    CON_GATE_DEV, // developer mode; a release build has it off, so the row is refused there ("not in this build")
} console_gate_t;

typedef enum {
    CON_ORIGIN_ANY,   // the USB console, Wi-Fi and the Console app
    CON_ORIGIN_LOCAL, // on the badge: the USB console or the Console app, not Wi-Fi
    CON_ORIGIN_UART,  // the USB console only: a person at the cable (epic-395 rule 4)
} console_origin_rule_t;

// What a row does: the build check reads CON_F_TX and CON_F_RESTART; the others shape the refusal and document
#define CON_F_TX       0x01 // makes the badge transmit (LoRa, BLE, Wi-Fi beyond replies), or switches a radio on
#define CON_F_RESTART  0x02 // restarts the badge or the C6
#define CON_F_ERASE    0x04 // erases or wipes storage
#define CON_F_USB_HOLD 0x08 // refused while the USB hold is on (#289), except from the USB console
#define CON_F_SECRET   0x10 // prints a secret (console_reply_secret)
#define CON_F_OWNER    0x80 // the owner's own bit for its dispatch (MeshCore: needs MeshCore running)

typedef struct console_sub { // named, so a header can pass a table without including this one
    char const *word;   // NULL: a row compiled out of this image, it matches nothing (REST and END have none)
    uint8_t     match;  // console_match_t
    uint8_t     gate;   // console_gate_t
    uint8_t     origin; // console_origin_rule_t
    uint8_t     flags;  // CON_F_*
} console_sub_t;

#define CONSOLE_SUB_END {NULL, CON_MATCH_END, CON_GATE_OPEN, CON_ORIGIN_ANY, 0}

// A row's word only in the developer image: the release row has no word, so the string is not in the image and nothing
// matches it there (the form then reaches the command, which has no such arm in a release)
#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define CON_DEV_WORD(w) (w)
#else
#define CON_DEV_WORD(w) NULL
#endif

// What the handler of sub gets of args, or NULL when it does not match
char const *console_sub_match(console_sub_t const *sub, char const *args);

// The first row of table (ended by END) that matches args, NULL for none or a NULL table
console_sub_t const *console_sub_find(console_sub_t const *table, char const *args);

// Whether a row with origin rule may run from origin; UNKNOWN (another task) is refused by every rule but ANY
bool console_origin_allows(uint8_t rule, console_origin_t origin);

typedef enum {
    CON_RULE_RUN,    // call the command
    CON_RULE_DEV,    // developer mode is off (or not in this build)
    CON_RULE_ORIGIN, // not from this origin
    CON_RULE_HOLD,   // the USB hold is on and this is not the USB console
} console_rule_t;

typedef struct {
    bool             dev_on; // developer mode now
    console_origin_t origin; // where the line came from
    bool             held;   // the USB hold is on (#289)
} console_ask_t;

// What the console does with row (NULL: no row matched, it runs): the gate first, then the origin, then the hold
console_rule_t console_sub_rule(console_sub_t const *row, console_ask_t const *ask);

#ifdef __cplusplus
}
#endif
