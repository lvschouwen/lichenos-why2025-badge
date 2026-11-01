#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The messages the badge shows (#61, #65): channel messages and DMs, one ring per chat (#48, #59: a quiet chat keeps its
// messages however much Public says), newest kept, oldest dropped (contacts live in MeshCore's own table). Pure logic,
// no ESP headers (also built into the host tests); not thread-safe, the mesh component wraps it in a lock.

#define MESH_TEXT_MAX    160 // bytes of message text kept (UTF-8, cut on a character boundary)
#define MESH_NAME_MAX    32
#define MESH_CHANNEL_MAX 31

typedef struct {
    char     channel[MESH_CHANNEL_MAX + 1];
    char     text[MESH_TEXT_MAX + 1]; // as MeshCore delivers it: "sender: message"
    int16_t  rssi_x10;
    uint32_t heard_s; // uptime when received
    // Our own channel messages (#46): outgoing, with the outbox id and state (mesh_out_state_t) kept up to date by the
    // mesh task. text is "me: <text>". Incoming messages have all three zero.
    uint32_t id;
    uint8_t  outgoing, state, repeats;
    // DMs (#47): dm is set for a direct message either way, peer is the other side's public key prefix; channel then
    // holds "@<name>". attempt is the DM try in progress (1-3), trip_ms the time to the ACK once delivered.
    uint8_t  dm, attempt, peer[6];
    uint32_t trip_ms;
    uint32_t seq;   // store-wide counter, grows with every message: the order across chats (also the journal's, #48)
    uint32_t epoch; // wall clock (Unix seconds) when it was added, 0 when the clock was not set
    uint8_t  loaded; // replayed from the history journal at boot (#48): heard_s means nothing then, epoch is the age
} mesh_msg_t;

// A chat: a channel (by lower-cased name, slots get reused) or a DM (by the contact's 6-byte key prefix)
#define MESH_CHAT_CHANNEL 1
#define MESH_CHAT_DM      2
#define MESH_CONV_MAX     64 // chats kept; when full the DM chat quiet for longest goes (channels never)
typedef struct {
    uint8_t kind;
    uint8_t id[6];                      // DM: key prefix
    char    chan[MESH_CHANNEL_MAX + 1]; // channel: lower-cased name
} mesh_chat_key_t;
void mesh_chat_key_channel(mesh_chat_key_t *key, char const *name);
void mesh_chat_key_dm(mesh_chat_key_t *key, uint8_t const peer[6]);

typedef struct {
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 1]; // channel name as last given, or "@<contact>"
    uint16_t        unread;
    uint32_t        last_heard_s;
    uint32_t        last_seq; // seq of its newest message: the order across chats (0 for a chat without messages)
    char            last[MESH_TEXT_MAX + 1];
    uint8_t         last_outgoing;
    uint8_t         last_loaded; // the newest message came from the journal: last_epoch is its age, not last_heard_s
    uint32_t        last_epoch;
    uint16_t        count; // messages kept
} mesh_conv_t;

// Resets the store. Every chat gets a ring of chat_keep messages, allocated on its first message (PSRAM through malloc
// on the badge); false when chat_keep is 0.
bool mesh_store_init(size_t chat_keep);

// Hooks (NULL = default): alloc_ring gets (count, size) for a chat's ring and returns zeroed memory (the badge gives
// PSRAM explicitly, host tests can fail it); warn is told when a message is dropped (out of memory, or a table full of
// channels). Set before mesh_store_init.
void mesh_store_set_hooks(void *(*alloc_ring)(size_t count, size_t size), void (*warn)(char const *what));
// The wall clock for mesh_msg_t.epoch (NULL or returning 0: not set). Kept across mesh_store_init.
void mesh_store_set_clock(uint32_t (*epoch_now)(void));
// Messages dropped since mesh_store_init
uint32_t mesh_store_dropped(void);

// The add functions return false when the message was dropped: the chat table is full of channels, or a new chat's
// ring could not be allocated (the chat that would have made room is then kept, with its messages).
bool mesh_store_add_message(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s);

// A direct message (#47): channel is "@<name>", peer the sender's 6-byte key prefix
bool mesh_store_add_dm(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s, uint8_t const peer[6]);
bool mesh_store_add_outgoing_dm(char const *channel, char const *text, uint32_t id, uint32_t heard_s,
                                uint8_t const peer[6]);
// A message we send: not an unread one (it does not move mesh_store_generation, only the view generation)
bool mesh_store_add_outgoing(char const *channel, char const *text, uint32_t id, uint32_t heard_s);
// The outgoing message's new state; true when something changed (the view generation moves then)
bool mesh_store_update_out(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms);

// A message replayed from the history journal (#48): it goes into its chat in seq order (the journal can hold an
// outgoing message after later incoming ones, it is written once its state is final), and a message with a seq the
// chat already holds replaces it (a newer state of the same message). It counts as read, gets loaded = 1, heard_s = 0
// and id = 0 (outbox ids do not survive a restart), and the store's seq counter continues after it (also when it is
// dropped, so a new message never reuses a journal seq). false when it was
// dropped: table full of channels, no memory, or older than everything in a full ring.
bool mesh_store_add_loaded(mesh_chat_key_t const *key, char const *title, mesh_msg_t const *m);
// The newest seq handed out (or loaded): 0 for an empty store
uint32_t mesh_store_last_seq(void);

// All chats together, ordered by seq (the Messages tab until it lists chats, and the console)
size_t mesh_store_message_count(void);
bool   mesh_store_message(size_t index, mesh_msg_t *out); // 0 = oldest
// The max newest messages over all chats, oldest first; returns how many
size_t mesh_store_recent(mesh_msg_t *out, size_t max);

// One chat's messages, oldest first, at most the newest max; with mark_read its unread count goes to 0
size_t mesh_store_chat(mesh_chat_key_t const *key, mesh_msg_t *out, size_t max, bool mark_read);
// The chats in table order (the caller sorts), up to max; returns how many
size_t mesh_store_convs(mesh_conv_t *out, size_t max);
// The Messages tab's order (#48): chats with messages first, the newest on top; chats without messages after them, in
// the order given. Stable.
void mesh_conv_sort(mesh_conv_t *convs, size_t n);
// Makes the table match the configured channels (names, in slot order): a channel chat that is not configured any more
// drops out of the list (its messages stay in the store), a configured channel without a chat is added empty. DM chats
// stay. With no names (MeshCore not up) nothing is filtered. Returns the new count, at most max. Call before the sort.
size_t mesh_conv_merge_channels(mesh_conv_t *convs, size_t n, size_t max, char const names[][MESH_CHANNEL_MAX + 1],
                                size_t name_count);
// Unread incoming messages over all chats
uint32_t mesh_store_unread_total(void);
// Changes with every message added, also once the ring is full: readers rebuild when it differs from what they showed
uint32_t mesh_store_generation(void);
// Moves with every change a screen shows: incoming messages as the generation, plus our own messages and their states
uint32_t mesh_store_view_generation(void);

// Copies in[0..in_len) to out as printable UTF-8: invalid sequences become '?', control characters ' ', cut on a
// character boundary to fit cap - 1 bytes. Returns the length written.
size_t mesh_sanitize(char const *in, size_t in_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
