#pragma once

#include "mesh_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What MeshCore tells the notification service (#50, lichen_notify.h). Pure logic, no ESP headers
// (host_tests/mesh_notify_test.c): the decision, the mention test, the targets, the settings cache and the queue of
// notes the feed fills under the mesh lock and the mesh task posts after it. mesh_notify_run.c is the badge side.

typedef enum { MN_DM, MN_CHANNEL, MN_FAILED } mn_kind_t;
typedef enum { MN_MODE_ALL, MN_MODE_MENTIONS, MN_MODE_OFF, MN_MODE_COUNT } mn_mode_t;
typedef enum { MN_SKIP, MN_SILENT, MN_POST } mn_verdict_t;
typedef enum {
    MN_R_OK,
    MN_R_CHAT_OPEN,
    MN_R_DM_OFF,
    MN_R_FAIL_OFF,
    MN_R_MODE_OFF,
    MN_R_NOT_MENTION,
    MN_R_MENTIONS_OFF,
    MN_R_DND,
    MN_R_SOURCE_OFF,
    MN_R_COUNT
} mn_reason_t;

typedef struct {
    mn_verdict_t verdict;
    mn_reason_t  reason;
} mn_out_t;

// The switches: direct messages, delivery failed, mentions of my name
typedef struct {
    bool dm, fail, mentions;
} mn_switches_t;

// Defaults: everything on; Public is "Mentions" (its traffic would bury everything else), other channels "All"
#define MN_DEFAULT_SWITCHES {true, true, true}
mn_mode_t mn_default_mode(mesh_chat_key_t const *key);

// chat_open: that chat is on screen (the store counts it read). A failed delivery has its own switch and ignores
// mode; a DM only the dm switch. A channel: mode All posts; Mentions posts a mention when the mentions switch is on;
// Off nothing. Do-not-disturb keeps the history but silences the toast (SILENT).
mn_out_t    mn_decide(mn_kind_t kind, mn_mode_t mode, mn_switches_t const *sw, bool chat_open, bool mention, bool dnd);
char const *mn_reason_text(mn_reason_t reason);

// Is my_name mentioned in the message part of a channel line "sender: message"? "@[name]" or "@name" as a whole word,
// without case (ASCII); the sender before the first ": " never counts, nor does the bare name without @.
bool mn_is_mention(char const *text, char const *my_name);

// Chat targets for lichen_notify_to (32 bytes at most): "ch:<name>" and "dm:<12 hex>", and "fc:" / "fd:" for a failed
// delivery (its own target, so it never merges into "new messages"). A channel name is cut to 28 bytes.
#define MN_TARGET_MAX 32
size_t mn_target(mn_kind_t kind, mesh_chat_key_t const *key, char *out, size_t cap);
#define MN_TARGET_NAME_MAX 28 // a channel name is cut to this in a target
// Which of the configured channels (names[0..n), stored case) a parsed target's chan means: -1 for none. An exact
// match without case wins; only a chan of MN_TARGET_NAME_MAX bytes (cut) may match by prefix, and only one channel.
int mn_channel_match(char const names[][MESH_CHANNEL_MAX + 1], size_t n, char const *chan);
// false when target is not one of ours. key->chan is the (possibly cut) lower-case name; *failed for fc:/fd:
bool mn_target_parse(char const *target, mesh_chat_key_t *key, bool *failed);

// The persistent per-chat key: "m" and the 8 hex digits of an FNV-1a hash of the chat key
uint32_t mn_key_hash(mesh_chat_key_t const *key);
void     mn_nvs_key(mesh_chat_key_t const *key, char out[10]);

// Per-chat modes already read from storage: a small table, a miss means "read it once, then store it"
#define MN_CACHE_MAX 48
typedef struct {
    uint32_t hash[MN_CACHE_MAX];
    uint8_t  mode[MN_CACHE_MAX];
    uint8_t  n, next;
} mn_cache_t;
bool mn_cache_lookup(mn_cache_t const *c, mesh_chat_key_t const *key, mn_mode_t *mode);
void mn_cache_store(mn_cache_t *c, mesh_chat_key_t const *key, mn_mode_t mode); // replaces, or round-robin when full

// Does this chat's unread count toward the home badge? DMs always; a channel only in mode All (the same number the
// tile line and the grid show). Pure of the mode, which the caller looks up.
bool mn_counts_unread(mesh_chat_key_t const *key, mn_mode_t mode);

// ---- notes: one per event, merged per (kind, chat) while they wait ----------------------------------------------------
typedef struct {
    mn_kind_t       kind;
    mesh_chat_key_t key;
    bool            mention;
    uint8_t         count; // merged events, at most 255
    char            title[40];
    char            body[120];
} mn_note_t;

#define MN_QUEUE_MAX 8
typedef struct {
    mn_note_t note[MN_QUEUE_MAX];
    uint8_t   n;
    uint32_t  lost; // notes dropped because the queue was full of other chats
} mn_queue_t;

// Cuts title/body on a character boundary. A note for a chat that waits already merges into it (count++, the latest
// body wins unless it would replace a mention with a plain line). false when it was dropped (full of other chats).
bool mn_queue_push(mn_queue_t *q, mn_kind_t kind, mesh_chat_key_t const *key, char const *title, char const *body,
                   bool mention);
// A title and body as mn_queue_push keeps them (sanitised, cut on a character boundary), made before the caller takes
// its spinlock (#271: the title can be a channel name in MeshCore's PSRAM tables). Pushing them gives the same note as
// pushing the originals: the push's own pass leaves them as they are.
typedef struct {
    char title[sizeof(((mn_note_t *)0)->title)];
    char body[sizeof(((mn_note_t *)0)->body)];
} mn_text_t;
void mn_text_prepare(mn_text_t *out, char const *title, char const *body);
// Moves everything out (oldest first), returns how many
size_t mn_queue_take(mn_queue_t *q, mn_note_t *out, size_t max);

// One failed-delivery note per message: true when id has just become failed (a retry that fails again counts again)
#define MN_FAILTRACK_MAX 32
typedef struct {
    uint32_t id[MN_FAILTRACK_MAX];
    uint8_t  failed[MN_FAILTRACK_MAX];
    uint8_t  next;
} mn_failtrack_t;
bool mn_failtrack_step(mn_failtrack_t *t, uint32_t id, bool is_failed);

#ifdef __cplusplus
}
#endif
