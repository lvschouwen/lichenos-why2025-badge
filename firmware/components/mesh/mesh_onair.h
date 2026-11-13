#pragma once
// The console's on-air allow list (#289): with it on, a console line may transmit only to the channels and contacts
// on the list. The host guard (tools/onair_guard.sh) checks the lines it sends; this is the check that also covers a
// line that reaches the badge another way (a direct POST /console, a serial write, keys typed into the Console app).
// It applies to every console origin and never to the screens: the person's own sends are untouched.
//
// Off by default, so a user badge behaves as before. Switching it on works from anywhere (it only narrows); switching
// it off and editing the list work only from the USB console, a person at the cable: the Wi-Fi console's token also
// installs firmware, and the Console app counts as remote because injected keys can type there (epic-395 rule 4).
// The list lives on the badge only, never in the repo (it is public).
//
// The two cases without a plain target (owner decision 2026-11-13, see mesh_onair_allows):
// - an advert (fw advert, fw mesh advert): it has no target and carries only the badge's own identity; a flood one
//   is already bounded by the console scope (#290). It passes, so a one-off advert from the bench works.
// - the status requests that "fw mesh history <who> on" schedules: checked twice, when the line switches them on (on
//   the console, with the reason) and at every poll the mesh task sends later (a poll whose target left the list, or
//   that was switched on while the list was off, is refused at the radio's door, not sent).
//
// Pure logic, no ESP headers (host_tests/mesh_onair_test.c); mesh_onair_run.c holds the live copy, its lock, the
// setting and the console command.

#include "fw_console.h" // console_origin_t (the peer header mesh may use)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONAIR_MAX          8  // entries, channels and contacts together
#define ONAIR_NAME_MAX     31 // a channel name, as MESH_CHANNEL_MAX
#define ONAIR_PREFIX       6  // a contact by the first 6 bytes of its key: what the senders address it by
#define ONAIR_HEX_MIN      12 // key digits: at least the whole prefix, so one entry never matches a range of nodes
#define ONAIR_HEX_MAX      64 // a whole public key
#define ONAIR_TEXT_MAX     (ONAIR_MAX * (ONAIR_NAME_MAX + 3) + 1) // the saved text: "c <name>\n" or "k <12 hex>\n" each
#define ONAIR_REFUSED_MAX  96 // the refusal text with its target
#define ONAIR_REFUSED_POLL "on air refused: the target is not on the console allow list"

typedef enum { ONAIR_CHANNEL, ONAIR_CONTACT } onair_kind_t;

typedef struct {
    onair_kind_t kind;
    char         name[ONAIR_NAME_MAX + 1]; // ONAIR_CHANNEL
    uint8_t      prefix[ONAIR_PREFIX];     // ONAIR_CONTACT
} onair_entry_t;

typedef struct {
    size_t        n;
    onair_entry_t e[ONAIR_MAX];
} onair_list_t;

// What a console line wants to transmit
typedef enum {
    ONAIR_TX_CHANNEL, // a channel message: channel is its stored name
    ONAIR_TX_CONTACT, // a DM, trace, discover, remote request, telemetry or status poll: prefix is the target's
    ONAIR_TX_ADVERT,  // the badge's own advert: no target
} onair_tx_t;

typedef enum {
    ONAIR_OK,
    ONAIR_BAD,       // not a channel name nor 12 to 64 hex digits (even)
    ONAIR_FULL,      // ONAIR_MAX entries already
    ONAIR_DUPLICATE, // already on the list
    ONAIR_NOT_FOUND, // remove: not on the list
} onair_result_t;

// What "fw onair ..." asks
typedef enum {
    ONAIR_OP_STATUS, // "onair": the setting and the list
    ONAIR_OP_ON,     // "onair on"
    ONAIR_OP_OFF,    // "onair off"
    ONAIR_OP_ALLOW,  // "onair allow <channel|key digits>"
    ONAIR_OP_REMOVE, // "onair remove <channel|key digits>"
    ONAIR_OP_CLEAR,  // "onair clear": empties the list
    ONAIR_OP_USAGE,  // anything else
} onair_op_t;

// One entry from typed text (spaces around it ignored): only hex digits is a contact's key, valid as 12 to 64 of them
// (an even count) and kept as its first ONAIR_PREFIX bytes; anything else a channel name of 1 to ONAIR_NAME_MAX bytes
// without control characters. A channel whose name is only hex digits cannot be listed (a typo in key digits must
// never become a channel entry).
onair_result_t onair_entry_parse(char const *text, onair_entry_t *out);
onair_result_t onair_add(onair_list_t *list, char const *text);
onair_result_t onair_remove(onair_list_t *list, char const *text);
// The one decision: may a send of kind tx (channel or prefix as onair_tx_t says) go out? A screen's (from_console
// false) always; with the setting off always; else a channel or contact only when listed (a NULL or empty target is
// never listed) and an advert always (see the top of this file).
bool mesh_onair_allows(onair_list_t const *list, bool on, bool from_console, onair_tx_t tx, char const *channel,
                       uint8_t const *prefix);
// "on air refused: <target> is not on the console allow list": a channel's name, a contact's 12 hex digits
void onair_refused_text(onair_tx_t tx, char const *channel, uint8_t const *prefix, char *out, size_t cap);

onair_op_t onair_op_parse(char const *args, char const **rest);
// Whether a console line from origin may run op: off and every change of the list only from the USB console; the
// status and "on" (it only narrows) from anywhere
bool onair_op_permitted(onair_op_t op, console_origin_t origin);

// The saved text and back: one "c <name>" or "k <12 hex>" line per entry. Decoding is strict: a damaged, duplicated
// or overlong text gives false and an empty list (with the setting on, an empty list refuses every console send).
// Encoding is false when cap is too small.
bool onair_encode(onair_list_t const *list, char *out, size_t cap);
bool onair_decode(char const *text, onair_list_t *out);
// One entry as "fw onair" prints it: "channel <name>" or "contact <12 hex>"
void onair_entry_text(onair_entry_t const *e, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
