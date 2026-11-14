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
    RS_REBOOT,   // an action (the CLI word "reboot", shown as Restart), no value
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
#define RS_MISSES_MAX 3 // gets in a row without an answer stop the read
#define RS_UNSENT_MAX 2 // a get that could not be sent is tried this often

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
// The commands that need a yes before they go (a radio change, a reboot): the screen asks with a dialog, the console
// wants the word YES after the command. One predicate for both so they cannot drift (#123).
bool rs_cmd_needs_confirm(char const *cmd);
typedef enum {
    RS_CONF_NOT_NEEDED, // out = cmd as it is
    RS_CONF_MISSING,    // needs a yes and has none
    RS_CONF_GIVEN,      // out = cmd without the trailing " YES"
} rs_confirm_t;
// The console's form: "reboot YES". Only a command that needs a confirm loses the word; "set name YES" is a name.
rs_confirm_t rs_cmd_confirm(char const *cmd, char *out, size_t cap);
// A secret command for the CLI history: "password (hidden)", "a7|set guest.password (hidden)"
void rs_redact_cmd(char const *cmd, char *out, size_t cap);
// Whether the line is a secret command under a looser reading than rs_cmd_class: any case, any run of blanks (#449).
// For what is shown and kept only: the line is sent as typed, and the repeater refuses such a form.
bool rs_cmd_secret_loose(char const *cmd);
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
    bool     tag_probe; // the command in flight is the first get: an untagged "Unknown command" means no tags
    bool     probed;
    uint8_t  tries[RS_ROWS]; // how often a row's get could not be sent
    uint8_t  misses;         // gets in a row without an answer
    bool     stopped;        // the read stopped after RS_MISSES_MAX of them: rs_read_all starts again
    // the last set's outcome, for the note under the rows
    int8_t   last_row;
    uint8_t  last; // rs_reply_t, RS_R_NOT_OURS when none yet
    char     last_text[RS_TEXT_MAX];
} rs_t;

void rs_init(rs_t *s);
// Reads every readable row again (the screen opening, "Read again", the console). The read stops by itself after
// RS_MISSES_MAX gets in a row without an answer (stopped; the rows not asked show "—").
void rs_read_all(rs_t *s);
// Queues a set of row i (the person's Enter). NULL when queued, else why not: the value (rs_check), or another set
// still waiting. A set is sent before the gets still to read; its row is read back once the repeater took it. The last
// outcome (last_row) is cleared: the next one is this set's.
char const *rs_queue_set(rs_t *s, size_t i, char const *value);
bool        rs_busy(rs_t const *s);
// The next command to send: true and the command in out. The first command is a get (it probes the tags), also when a
// set was queued first., false when nothing is to be sent or one is in flight. The
// queued value (a password) is wiped from s once its command is made.
bool rs_next(rs_t *s, char *out, size_t cap);
// The answer to the command in flight: its text (NULL: none came, the request failed or timed out). false when the
// text is not ours (RS_R_NOT_OURS): the command stays in flight, call again with a later text or NULL to give up.
bool rs_on_answer(rs_t *s, char const *reply);
// The command in flight could not even be sent: a get goes back in the queue (up to RS_UNSENT_MAX tries in all, then
// it shows "—"); a set is dropped with why
void rs_on_unsent(rs_t *s, char const *why);
// What a row shows: the value (and unit), "..." while unknown or asked, "No answer" without one; "" for the restart and
// the password (their rows are actions, not states)
void rs_value_text(rs_t const *s, size_t i, char *out, size_t cap);
// A raw reply value as a row shows it: the radio as "869.618 MHz, 62.5 kHz, SF7, CR5", the decimal rows to a fixed number
// of places (#131); anything else, or text that does not parse, as it is (trimmed)
void rs_format_value(size_t i, char const *raw, char *out, size_t cap);
// The last set's outcome (rs_t.last / last_text) in plain words: "Saved", "Saved. It applies after a restart", "The
// repeater refused it: max 64" (R4). RS_R_OK for a restart reads "Restart sent".
void rs_outcome_words(size_t i, rs_reply_t result, char const *raw, char *out, size_t cap);
// The value as the edit field starts with it ("" when none)
void rs_edit_text(rs_t const *s, size_t i, char *out, size_t cap);
// How many readable rows are read (answered or not), of how many
void rs_progress(rs_t const *s, size_t *done, size_t *total);

// Which line the note under Read again shows (#140). The tick redraws it every 250 ms, so a one-off note set by an
// action ("Unchanged: nothing sent.") only stays if it is one of the sources: in order, a change on its way, the note
// of the last action (cleared by the next action), the last change's outcome, the last read problem, nothing.
typedef enum { RS_NOTE_SENDING, RS_NOTE_LOCAL, RS_NOTE_OUTCOME, RS_NOTE_PROBLEM, RS_NOTE_NONE } rs_note_src_t;
rs_note_src_t rs_note_pick(bool waiting, bool local, bool outcome, bool problem);

// The radio as typed or as "get radio" gives it ("869.618,62.5,7,5"; spaces also separate): false when it is not four
// numbers within simple_repeater's limits (150-2500 MHz, 7-500 kHz, SF 5-12, CR 5-8)
bool rs_radio_parse(char const *text, float *freq, float *bw, uint8_t *sf, uint8_t *cr);

#ifdef __cplusplus
}
#endif
