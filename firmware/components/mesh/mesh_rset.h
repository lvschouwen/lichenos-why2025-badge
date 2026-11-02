#pragma once
// A repeater's settings over its CLI (#117): the rows, the commands for them, the replies, and the commands the badge
// never sends or sends only with care. ESP-free and without a clock (host-tested in host_tests/mesh_rset_test.c).
//
// The keys and the reply formats are upstream simple_repeater's (meshcore/src/helpers/CommonCLI.cpp and
// CommonRadioPrefs.cpp): "get <key>" answers "> <value>", "set <key> <value>" answers "OK..." or an error text. Every
// command goes out with simple_repeater's two-character tag ("a7|get name"), which the repeater reflects in its reply
// ("a7|> NL-..."), so a late reply to an earlier get is never taken for the answer to this one.
//
// rs_t reads and changes the values one command at a time: the caller sends what rs_next gives through the remote CLI
// (mesh_remote.h) and hands the reply back to rs_on_answer. Nothing is set unless rs_queue_set was called, which the
// settings screen does only on an Enter of the person (and, for the radio and a reboot, after a careful confirm).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_TEXT,     // the node name: 1..31 characters, none of []\:,?*
    RS_INT,      // a whole number within min..max (or 0 when zero_ok)
    RS_DEC,      // a number with decimals within min..max
    RS_ONOFF,    // on / off
    RS_CHOICE,   // one of choices ("off|minimal|...")
    RS_RADIO,    // freq,bw,sf,cr (the careful one: a wrong value cuts the repeater off the mesh)
    RS_READONLY, // shown, never set (get role, ver)
    RS_PASSWORD, // the admin password: set only ("password <new>"), never read back
    RS_REBOOT,   // an action ("reboot"), no value
} rs_kind_t;

typedef struct {
    char const *key;   // the CLI word after get/set
    char const *label; // the row's label on the screen
    uint8_t     kind;  // rs_kind_t
    bool        zero_ok;
    float       min, max;
    char const *unit;    // shown after the value ("dBm"), NULL for none
    char const *choices; // RS_CHOICE: "a|b|c"
} rs_row_t;

#define RS_ROWS      19
#define RS_VALUE_MAX 48
#define RS_CMD_MAX   100 // = RR_CMD_MAX
#define RS_TEXT_MAX  96

size_t          rs_row_count(void);
rs_row_t const *rs_row_at(size_t i); // NULL past the end
int             rs_row_find(char const *key); // -1 for none
// Whether the row is read with a command (all but the password and the reboot)
bool rs_row_readable(size_t i);
bool rs_row_editable(size_t i);

// NULL when value is fine for row i, else why not (short, for the screen). The value is as typed: spaces around it are
// ignored.
char const *rs_check(size_t i, char const *value);
// The commands, tagged with the two characters of tag (NULL: untagged, for a repeater that does not reflect tags).
// false when the row has no such command, the value is refused (rs_check) or it does not fit.
bool rs_get_cmd(size_t i, char const *tag, char *out, size_t cap);
bool rs_set_cmd(size_t i, char const *value, char const *tag, char *out, size_t cap);

// The reply to one command, as rs_parse_reply reads it
typedef enum {
    RS_R_VALUE,       // a get's value (in out)
    RS_R_OK,          // a set was taken (out: the repeater's words, "OK - reboot to apply")
    RS_R_ERROR,       // a set was refused (out: the repeater's words)
    RS_R_UNSUPPORTED, // the repeater does not know this get ("??: key", "unknown config", "Unknown command")
    RS_R_NOT_OURS,    // no line of the reply carries our tag (yet): a late reply to an earlier command
} rs_reply_t;
// reply: the CLI text, possibly several messages joined with '\n'. tag NULL: untagged mode, the first line counts.
rs_reply_t rs_parse_reply(size_t i, bool is_set, char const *tag, char const *reply, char *out, size_t cap);

// ---- commands the badge filters (the free CLI line too: mesh_remote.c) --------------------------------------------
typedef enum {
    RS_CMD_PLAIN,
    RS_CMD_FORBIDDEN, // erase: wipes the repeater's file system. Never sent from the badge.
    RS_CMD_RADIO,     // set radio / set freq / tempradio: a wrong value cuts it off the mesh
    RS_CMD_SECRET,    // password / set guest.password / set prv.key: never shown or kept in the CLI history
    RS_CMD_REBOOT,    // reboot / clkreboot / poweroff / shutdown / start ota
} rs_cmd_class_t;
// As the repeater reads the command: leading spaces and the optional "xx|" tag are skipped
rs_cmd_class_t rs_cmd_class(char const *cmd);
// A secret command for the CLI history: "password (hidden)", "a7|set guest.password (hidden)"
void rs_redact_cmd(char const *cmd, char *out, size_t cap);
// A reply to a secret command: "password now: <pw>" (the repeater echoes it) becomes "password now: (hidden)"
void rs_redact_reply(char const *reply, char *out, size_t cap);

// ---- reading and changing the values, one command at a time -------------------------------------------------------
typedef enum { RS_V_UNKNOWN, RS_V_ASKING, RS_V_VALUE, RS_V_NONE } rs_vstate_t;

typedef struct {
    uint8_t  st[RS_ROWS]; // rs_vstate_t
    char     value[RS_ROWS][RS_VALUE_MAX];
    uint32_t want;     // rows still to read (bit i)
    int8_t   set_row;  // the queued set, -1 for none
    char     set_value[RS_CMD_MAX + 1];
    int8_t   cur_row;  // the command in flight, -1 for none
    bool     cur_set;
    char     cur_tag[3];
    uint8_t  tag_n;
    bool     untagged; // the repeater did not reflect the tag: the rest goes untagged
    bool     tag_probe; // the command in flight is the first one: an untagged "Unknown command" means no tags
    bool     probed;
    // the last set's outcome, for the note under the rows
    int8_t   last_row;
    uint8_t  last; // rs_reply_t, RS_R_NOT_OURS when none yet
    char     last_text[RS_TEXT_MAX];
} rs_t;

void rs_init(rs_t *s);
// Reads every readable row again (the screen opening, "Read again", the console)
void rs_read_all(rs_t *s);
// Queues a set of row i (the person's Enter). NULL when queued, else why not: the value (rs_check), or another set
// still waiting. A set is sent before the gets still to read; its row is read back once the repeater took it. The last
// outcome (last_row) is cleared: the next one is this set's.
char const *rs_queue_set(rs_t *s, size_t i, char const *value);
bool        rs_busy(rs_t const *s);
// The next command to send: true and the command in out, false when nothing is to be sent or one is in flight. The
// queued value (a password) is wiped from s once its command is made.
bool rs_next(rs_t *s, char *out, size_t cap);
// The answer to the command in flight: its text (NULL: none came, the request failed or timed out). false when the
// text is not ours (RS_R_NOT_OURS): the command stays in flight, call again with a later text or NULL to give up.
bool rs_on_answer(rs_t *s, char const *reply);
// The command in flight could not even be sent: back in the queue (a get) or dropped with why (a set)
void rs_on_unsent(rs_t *s, char const *why);
// What a row shows: the value (and unit), "..." while unknown or asked, "—" without an answer; "" for the reboot and
// "change" for the password
void rs_value_text(rs_t const *s, size_t i, char *out, size_t cap);
// The value as the edit field starts with it ("" when none)
void rs_edit_text(rs_t const *s, size_t i, char *out, size_t cap);
// How many readable rows are read (answered or not), of how many
void rs_progress(rs_t const *s, size_t *done, size_t *total);

// The radio as typed or as "get radio" gives it ("869.618,62.5,7,5"; spaces also separate): false when it is not four
// numbers within simple_repeater's limits (150-2500 MHz, 7-500 kHz, SF 5-12, CR 5-8)
bool rs_radio_parse(char const *text, float *freq, float *bw, uint8_t *sf, uint8_t *cr);

#ifdef __cplusplus
}
#endif
