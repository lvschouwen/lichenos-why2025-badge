#pragma once

#include "mesh_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the channel chat screen shows (#46). Pure logic, no ESP headers (host_tests/mesh_chat_test.c).

typedef enum { MESH_STATUS_NONE, MESH_STATUS_PENDING, MESH_STATUS_OK, MESH_STATUS_FAILED } mesh_status_kind_t;

// "1.8 s" / "420 ms" for a trip time
void mesh_format_trip(uint32_t ms, char *out, size_t cap);

// The words under one of our messages: "queued", "on air", "heard repeated" (x2...), "delivered", "not sent". Empty for
// an incoming message. Returns how the screen should colour it.
mesh_status_kind_t mesh_msg_status(mesh_msg_t const *m, char *out, size_t cap);

// A DM's words (#47): "queued", "waiting for ACK" ("..., try 2/3" from the second attempt), "delivered, 1.8 s",
// "not delivered". Same kinds as above. Called by mesh_msg_status for DMs.
// Is this message part of the chat on that channel (names compare without case)
bool mesh_msg_in_channel(mesh_msg_t const *m, char const *channel);

// Is this message a DM with the contact whose key starts with peer (6 bytes), either direction
bool mesh_msg_in_dm(mesh_msg_t const *m, uint8_t const peer[6]);

// The compose line's counter: "12 / 138 bytes", or "170 / 138 bytes, the last 32 are cut" when it is too long.
// Returns the bytes that would be sent (cut on a UTF-8 boundary).
size_t mesh_compose_status(char const *text, size_t max_bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
