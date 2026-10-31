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

// The words under one of our messages: "queued", "on air", "heard repeated" (x2...), "delivered", "not sent". Empty for
// an incoming message. Returns how the screen should colour it.
mesh_status_kind_t mesh_msg_status(mesh_msg_t const *m, char *out, size_t cap);

// Is this message part of the chat on that channel (names compare without case)
bool mesh_msg_in_channel(mesh_msg_t const *m, char const *channel);

// The compose line's counter: "12 / 138 bytes", or "170 / 138 bytes, the last 32 are cut" when it is too long.
// Returns the bytes that would be sent (cut on a UTF-8 boundary).
size_t mesh_compose_status(char const *text, size_t max_bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
