#pragma once
// Settings backup and restore (#169): the text format of the file on the SD card and the table of the settings that may
// go in it. Pure, no ESP headers; host-tested in host_tests/settings_backup_test.c. settings_backup_io.c reads and writes
// NVS for these keys.
//
// The table is an allow-list. A key that is not in it is never exported and is ignored (not an error) on import, so a
// file edited by hand, or made by another firmware, cannot reach a Wi-Fi password, the update token, the MeshCore
// identity, channel keys or any key nobody classified. A new setting is added here on purpose or not at all. Left out
// on purpose besides secrets: everything that changes what the badge transmits (radio switches, MeshCore advert and
// autostart), switches that open the network (Updates over Wi-Fi) and state the OS keeps for itself (seen markers,
// counters, hardware records).
// Not yet in the table: badge_fw.home_order (#189, the arranged home tiles), whose text (up to ~400 characters) is
// longer than SB_STR_MAX; it needs a larger value cap first, which costs internal RAM in settings_backup_io.c.
// Likewise a.meshcore.mesh_order (#189, the arranged MeshCore tiles) is not in the backup yet.
//
// File: lines "namespace.key = value"; blank lines and lines starting with '#' are comments. Numbers are decimal,
// text is the rest of the line.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SB_U8, SB_STR } sb_type_t;

typedef struct {
    char const *ns;    // NVS namespace: "badge_fw" or "a.<app id>"
    char const *key;   // at most 15 characters (NVS)
    char const *label; // what the person calls it (the summary dialog)
    sb_type_t   type;
    uint16_t    min, max; // SB_U8: the range; SB_STR: min 1, max the longest text
    uint16_t    def;      // SB_U8: the value when the key is not stored
    char const *def_str;  // SB_STR: the same
} sb_key_t;

#define SB_STR_MAX 64 // longest text value including the NUL
// Most keys the table may hold: callers size their value arrays by it (settings_backup_io.c, in PSRAM), and
// settings_backup.c fails the build when the table outgrows it
#define SB_KEYS_MAX 32

size_t          sb_key_count(void);
sb_key_t const *sb_key_at(size_t index);
sb_key_t const *sb_find(char const *ns, char const *key); // NULL: not in the allow-list

typedef struct {
    sb_key_t const *key;
    uint32_t        num;
    char            str[SB_STR_MAX];
} sb_value_t;

// Whether text (a value as written in the file, already trimmed) is valid for key; *out gets the value
bool sb_value_parse(sb_key_t const *key, char const *text, sb_value_t *out);
// The value as the file writes it
void sb_value_text(sb_value_t const *value, char *out, size_t cap);
bool sb_value_equal(sb_value_t const *a, sb_value_t const *b);

// The file's text: a header naming the firmware, then every value in vals (cap too small: 0, nothing usable)
size_t sb_format(char const *firmware, sb_value_t const *vals, size_t count, char *out, size_t cap);

typedef enum {
    SB_OK,
    SB_ERR_SYNTAX,   // not a comment, not "name = value"
    SB_ERR_VALUE,    // a known setting with a value that is not allowed
    SB_ERR_DUP,      // the same setting twice
    SB_ERR_TOO_LONG, // a line over SB_LINE_MAX
    SB_ERR_EMPTY,    // no setting from the allow-list in the file at all
} sb_err_t;

#define SB_LINE_MAX 200

typedef struct {
    sb_err_t err;
    size_t   line;    // 1-based line of the first problem (0 when err is SB_OK or SB_ERR_EMPTY)
    size_t   ignored; // lines of keys that are not in the allow-list
} sb_result_t;

// Parses and validates the whole file: either every line is fine and out gets one value per known setting (at most max,
// *count of them), or err says what is wrong and nothing may be applied.
sb_result_t sb_parse(char const *text, size_t len, sb_value_t *out, size_t max, size_t *count);
// "Line 7: 'saver' must be 0 or 1." style text for the person
void sb_error_text(sb_result_t const *r, char *out, size_t cap);

// "3 settings will change: Home layout, Time zone, Screen off after" (at most four names, then "and N more");
// ignored > 0 adds how many lines were skipped
void sb_summary(sb_value_t const *changed, size_t count, size_t ignored, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
