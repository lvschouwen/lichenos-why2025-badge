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
// Is this message a DM with the contact whose key starts with peer (6 bytes), either direction
bool mesh_msg_in_dm(mesh_msg_t const *m, uint8_t const peer[6]);

// One row of the Messages tab (#48): the title ("#badgetest", "@bob"), the unread badge ("" for none, "3", "99+") and
// the dim line under it: "3 min ago: bob: hello", or "no messages yet" for a chat without any. now_s is the
// mesh_uptime_s() the chat's last_heard_s is measured against.
void mesh_conv_row(mesh_conv_t const *c, uint32_t now_s, char *title, size_t title_cap, char *badge, size_t badge_cap,
                   char *sub, size_t sub_cap);

// While the clock is not set (#8) other nodes see a wrong time on what we send: adverts are refused, messages are sent
// with this hint shown in the composer. NULL when the clock is set / when nothing is refused. An approximate clock (set
// from mesh contacts, unverified, #83) never runs backwards, so adverts go out, with a hint (mesh_advert_note).
char const *mesh_advert_refusal(bool clock_set);
char const *mesh_clock_hint(bool clock_set, bool approximate);
// "approximate clock" while the clock is only approximate, else NULL: shown next to "advert queued"
char const *mesh_advert_note(bool approximate);

// The compose line's counter: "12 / 138 bytes", or "170 / 138 bytes, the last 32 are cut" when it is too long.
// Returns the bytes that would be sent (cut on a UTF-8 boundary).
size_t mesh_compose_status(char const *text, size_t max_bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
