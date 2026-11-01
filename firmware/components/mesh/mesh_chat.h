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
void mesh_conv_row(mesh_conv_t const *c, uint32_t now_s, uint32_t now_epoch, char *title, size_t title_cap,
                   char *badge, size_t badge_cap, char *sub, size_t sub_cap);

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

// Search inside a chat (#48): the messages whose text has every word of query (case-insensitive, list_filter rules).
// Writes their indices into hits (oldest first, as msgs is) and returns how many, at most max. An empty or blank query
// has no hits.
#define MESH_CHAT_NO_HIT ((size_t)-1)
size_t mesh_chat_hits(mesh_msg_t const *msgs, size_t n, char const *query, uint16_t *hits, size_t max);

// The hit after pos (an index into hits) going dir: -1 is Up (older), +1 is Down (newer), wrapping at both ends. A pos
// that is not a hit (MESH_CHAT_NO_HIT) gives the newest hit, and so does dir 0. MESH_CHAT_NO_HIT when there are none.
size_t mesh_chat_hit_step(size_t pos, size_t n_hits, int dir);

// Where the message with this seq is among the hits, MESH_CHAT_NO_HIT when it is not one (the list was rebuilt)
size_t mesh_chat_hit_of_seq(mesh_msg_t const *msgs, uint16_t const *hits, size_t n_hits, uint32_t seq);

// The hits again after the chat was redrawn (new messages, the window moved on): fills hits like mesh_chat_hits and
// returns their count; *pos becomes the hit with prev_seq (when have_prev), else the newest hit (MESH_CHAT_NO_HIT when
// there are none). Never looks at the old hits: their indices belong to the old snapshot.
size_t mesh_chat_rehit(mesh_msg_t const *msgs, size_t n, char const *query, uint16_t *hits, size_t max, bool have_prev,
                       uint32_t prev_seq, size_t *pos);

// The line in place of the compose line while searching: "Search: foo_  (2 of 7)", counted from the newest hit,
// "Search: foo_  no match", or "Search: _  type to search" for an empty query
void mesh_chat_search_line(char const *query, size_t pos, size_t n_hits, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
