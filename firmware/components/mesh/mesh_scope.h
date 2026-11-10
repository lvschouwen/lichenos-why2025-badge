#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The default flood scope (#163): the region name MeshCore's companion keeps in its prefs (default_scope_name, 30
// characters at most; the key is SHA-256 of "#" + name, like a hashtag channel's). No name = "None (everywhere)": the
// adverts and messages flood without a region, as before. Pure logic, no ESP headers (host_tests/mesh_scope_test.c).

#define MESH_SCOPE_MAX 30

// NULL when the input is a scope: out is the region name, lowercased, with a leading '#' and surrounding spaces gone, or
// "" for None (empty input). Else the reason (out is ""). A name takes letters, digits and '-' (accented letters too);
// MeshCore also lets '$' mark a private region, but that needs a stored key the badge has no way to enter.
char const *mesh_scope_parse(char const *in, char *out, size_t cap);
// What the Settings row shows: "None (everywhere)" for no name, else the name
void mesh_scope_text(char const *name, char *out, size_t cap);
// The question before a region is set (not asked for None)
void mesh_scope_ask_text(char const *name, char *out, size_t cap);
// The text SHA-256 turns into the region key: "#" + name
void mesh_scope_key_text(char const *name, char *out, size_t cap);

// The console scope (#290): every flood a console line starts (send, dm, remote, telemetry, trace, discover) goes out
// under a region of its own, so Claude and the agents never flood further than the bench allows; the screens keep the
// default scope. Unlike the default, an empty console scope is no "None (everywhere)": the console flood is refused.
#define MESH_SCOPE_CONSOLE_UNSET "console scope not set: fw mesh scope console <region> YES"
typedef enum {
    MESH_SCOPE_USE_DEFAULT,
    MESH_SCOPE_USE_CONSOLE,
    MESH_SCOPE_USE_REFUSE,
    MESH_SCOPE_USE_CHANNEL,
    MESH_SCOPE_USE_DAMAGED, // a screen's message on a channel whose saved scope is damaged: refused, never wider
} mesh_scope_use_t;
// Which scope a flood takes. A console line's: the console scope, refused when unset, whatever the channel (#290 wins).
// A screen's: the channel's own scope when it has one (#291; channel_name NULL or "" = none, and every flood that is
// not a channel message), refused for MESH_CHSCOPE_DAMAGED, else the default.
mesh_scope_use_t mesh_scope_choose(bool from_console, char const *console_name, char const *channel_name);
// "fw mesh scope console <region> YES": the text after "console". NULL when out holds the region (mesh_scope_parse's
// rules, never empty), else why not (out is "").
char const *mesh_scope_console_arg(char const *args, char *out, size_t cap);
// What "fw mesh scope" prints: "default <name|None> | console <name|not set>" (tools/badge.sh reads the console part)
void mesh_scope_status_text(char const *default_name, char const *console_name, char *out, size_t cap);

// A channel's own flood scope (#291): a channel message from the screens floods in that region instead of the default.
// Badge-local (MeshCore's share format has no room for it), kept in MeshCore's directory next to the console scope. A
// channel is known by an id from its key (the first MESH_CHSCOPE_ID bytes of SHA-256 of the 32-byte secret), so a
// rename keeps the scope and a channel added again with another key does not inherit it. Two channels with the same
// secret (the same channel under two names) share one scope.
#define MESH_CHSCOPE_ID  8
#define MESH_CHSCOPE_MAX 40 // MAX_GROUP_CHANNELS in the mesh component's build
// The file: one "<16 hex digits of the id> <region>" line per channel, at most this long in all
#define MESH_CHSCOPE_TEXT_MAX (MESH_CHSCOPE_MAX * (2 * MESH_CHSCOPE_ID + MESH_SCOPE_MAX + 2) + 1)
// The name of a tombstone: a line whose id reads but whose region does not (or that a cut write left without its line
// end). Never a region (mesh_scope_parse refuses '!'): that channel's screen sends are refused until it is set again,
// so a damaged scope never floods wider than the region it was meant to be.
#define MESH_CHSCOPE_DAMAGED  "!"
#define MESH_CHSCOPE_REFUSED    "flood scope damaged: set it again (channel details, Flood scope)"
// The whole file did not read (it is there but would not open, is empty, or a line of it did not read): every
// channel's screen sends are refused until a scope is saved again, since the badge cannot tell which were meant narrow
#define MESH_CHSCOPE_UNREADABLE "flood scopes unreadable: set them again (channel details, Flood scope)"
typedef struct {
    uint8_t id[MESH_CHSCOPE_ID];
    char    name[MESH_SCOPE_MAX + 1];
} mesh_chscope_entry_t;
typedef struct {
    mesh_chscope_entry_t e[MESH_CHSCOPE_MAX];
    size_t               n;
} mesh_chscope_t;
void        mesh_chscope_clear(mesh_chscope_t *t);
// The channel's own region, "" when it has none (it floods under the default)
char const *mesh_chscope_find(mesh_chscope_t const *t, uint8_t const id[MESH_CHSCOPE_ID]);
// Sets (name, mesh_scope_parse's output) or with "" drops the channel's own scope. false when the table is full.
bool        mesh_chscope_put(mesh_chscope_t *t, uint8_t const id[MESH_CHSCOPE_ID], char const *name);
// Drops entry i (the start drops the ones whose channel is gone)
void        mesh_chscope_drop_at(mesh_chscope_t *t, size_t i);
// The file text of the table; its length (0 for an empty table, or when cap is too small: out is "" then)
size_t      mesh_chscope_write(mesh_chscope_t const *t, char *out, size_t cap);
// The table from a file text; the number of lines read (tombstones included). A line whose id reads but whose region
// does not, or the last line without its line end, becomes a tombstone (MESH_CHSCOPE_DAMAGED); a line without a
// readable id names no channel and is left out. *whole: the text was not empty, ended with its line end, and every
// non-empty line of it went into the table (false means the file as a whole is unreadable).
size_t      mesh_chscope_read(mesh_chscope_t *t, char const *text, bool *whole);
// A channel name's FNV-1a hash, ASCII lower-cased as channel_same_name compares: what the senders' copy of the
// damaged channels holds (a collision only refuses a healthy channel, never lets a damaged one through)
uint32_t    mesh_chscope_name_hash(char const *name);
// Which file a start reads (a cut between the write's remove and rename leaves only the read-back temp file)
typedef enum { MESH_CHSCOPE_FILE_NONE, MESH_CHSCOPE_FILE_READ, MESH_CHSCOPE_FILE_PROMOTE } mesh_chscope_file_t;
mesh_chscope_file_t mesh_chscope_file_pick(bool have_file, bool have_tmp);
// The channel details row: the own region, "Damaged: set it again" for a tombstone, else "Default (<default name>)"
// ("Default (None, everywhere)" without one)
void        mesh_scope_channel_text(char const *own, char const *default_name, char *out, size_t cap);
// The question before a channel gets its own region (not asked for going back to the default)
void        mesh_scope_channel_ask_text(char const *channel, char const *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
