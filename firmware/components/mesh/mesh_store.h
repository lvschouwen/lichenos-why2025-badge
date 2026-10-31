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
} mesh_msg_t;

// Allocates the ring (in PSRAM through malloc on the badge); false when out of memory
bool mesh_store_init(size_t max_messages);

void mesh_store_add_message(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s);

size_t mesh_store_message_count(void);
bool   mesh_store_message(size_t index, mesh_msg_t *out); // 0 = oldest
// Changes with every message added, also once the ring is full: readers rebuild when it differs from what they showed
uint32_t mesh_store_generation(void);

// Copies in[0..in_len) to out as printable UTF-8: invalid sequences become '?', control characters ' ', cut on a
// character boundary to fit cap - 1 bytes. Returns the length written.
size_t mesh_sanitize(char const *in, size_t in_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
