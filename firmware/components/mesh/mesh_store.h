#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The messages the badge shows (#61, #65): channel messages and DMs, newest kept, oldest dropped (contacts live in
// MeshCore's own table). Pure logic, no ESP headers (also built into the host tests); not thread-safe, the mesh
// component wraps it in a lock.

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
} mesh_msg_t;

// Allocates the ring (in PSRAM through malloc on the badge); false when out of memory
bool mesh_store_init(size_t max_messages);

void mesh_store_add_message(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s);

// A direct message (#47): channel is "@<name>", peer the sender's 6-byte key prefix
void mesh_store_add_dm(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s, uint8_t const peer[6]);
void mesh_store_add_outgoing_dm(char const *channel, char const *text, uint32_t id, uint32_t heard_s,
                                uint8_t const peer[6]);
// A message we send: not an unread one (it does not move mesh_store_generation, only the view generation)
void mesh_store_add_outgoing(char const *channel, char const *text, uint32_t id, uint32_t heard_s);
// The outgoing message's new state; true when something changed (the view generation moves then)
bool mesh_store_update_out(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms);

size_t mesh_store_message_count(void);
bool   mesh_store_message(size_t index, mesh_msg_t *out); // 0 = oldest
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
