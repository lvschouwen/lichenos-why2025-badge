#pragma once
// What the badge sent and where it stands (#45, for #46 channel sends and #47 DMs): a fixed table of outgoing messages
// with their delivery state machine. ESP-free and without a clock of its own (now_ms is passed in, 32-bit wrap safe).
// Not locked: the caller wraps each call in one short critical section. The mesh task takes actions out with
// ob_next_action and reports MyMesh's replies back; the UI adds messages and reads states.
#include "mesh_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t mesh_msg_id_t; // 0 = not queued
typedef enum {
    MESH_OUT_QUEUED,    // waiting for the mesh task
    MESH_OUT_SENT,      // MyMesh took it (reply OK / SENT)
    MESH_OUT_ON_AIR,    // the radio transmitted it
    MESH_OUT_HEARD,     // channel: a repeat of it was heard
    MESH_OUT_DELIVERED, // DM: the ACK came
    MESH_OUT_FAILED,    // refused, the radio failed, or the retries are used up
} mesh_out_state_t;

#define OB_CAP              16
#define OB_MAX_ATTEMPTS     3
#define OB_ACK_SLOTS        8 // MyMesh's expected-ACK table; the oldest slot is overwritten when it is full
#define OB_MAX_DM_INFLIGHT  2 // each DM may register OB_MAX_ATTEMPTS ACKs: (inflight + 1) x 3 must stay below OB_ACK_SLOTS
#define OB_TARGET_MAX       33 // channel name + NUL
#define OB_TEXT_MAX         (MF_MAX_TEXT + 1)
#define OB_HASH_LEN         8
#define OB_RETRY_BACKOFF_MS 2000
#define OB_TIMEOUT_FLOOR_MS 2000

typedef enum { OB_CHANNEL, OB_DM } ob_kind_t;

typedef struct {
    bool             used, awaiting, awaiting_reset, pending_reset, flood_seen, have_hash;
    uint8_t          kind, state, attempt, repeats, n_acks;
    mesh_msg_id_t    id;
    uint32_t         seq, ts, not_before_ms, deadline_ms, sent_ms, trip_ms;
    uint32_t         acks[OB_MAX_ATTEMPTS];
    uint8_t          hash[OB_HASH_LEN];
    uint8_t          prefix[MF_PREFIX];
    char             target[OB_TARGET_MAX];
    char             text[OB_TEXT_MAX];
} ob_entry_t;

typedef struct {
    ob_entry_t    e[OB_CAP];
    mesh_msg_id_t next_id;
    uint32_t      next_seq;
} ob_t;

typedef enum { OB_ACT_NONE, OB_ACT_SEND_CHANNEL, OB_ACT_SEND_DM, OB_ACT_RESET_PATH } ob_act_kind_t;
typedef struct {
    ob_act_kind_t kind;
    mesh_msg_id_t id;
    uint8_t       attempt; // the frame's attempt byte (0-based)
    uint32_t      ts;
    uint8_t       prefix[MF_PREFIX];
    char          target[OB_TARGET_MAX];
    char          text[OB_TEXT_MAX];
} ob_action_t;

void ob_init(ob_t *ob);
// Gives a message its timestamp (the pipe takes it from the clock only when an action needs one)
void ob_set_ts(ob_t *ob, mesh_msg_id_t id, uint32_t ts);
// The id, or 0 with *why set ("busy", "empty")
mesh_msg_id_t ob_add_channel(ob_t *ob, char const *channel, char const *text, size_t text_len, uint32_t now_ms,
                             char const **why);
mesh_msg_id_t ob_add_dm(ob_t *ob, uint8_t const prefix[MF_PREFIX], char const *text, size_t text_len, uint32_t now_ms,
                        char const **why);
// The next frame to send, or OB_ACT_NONE. Expires timed-out DMs first. ts_now stamps a message that has no timestamp yet (0: the action comes back with ts 0, then ob_set_ts).
bool ob_next_action(ob_t *ob, uint32_t now_ms, uint32_t ts_now, ob_action_t *act);
// MyMesh's answer to the action's frame. A kind other than OK/ERR/SENT counts as an error.
void ob_on_reply(ob_t *ob, mesh_msg_id_t id, mf_msg_t const *m, uint32_t now_ms);
// The pipe could not even build the frame (unknown channel or contact): the message fails
void ob_fail(ob_t *ob, mesh_msg_id_t id);
// A packet of ours left the radio (payload_is_dm: TXT_MSG, else GRP_TXT) or the radio refused it
void ob_on_tx(ob_t *ob, bool payload_is_dm, uint8_t const hash[OB_HASH_LEN]);
void ob_on_tx_fail(ob_t *ob, bool payload_is_dm, uint32_t now_ms);
// A packet was received whose hash may be a repeat of one of ours
void ob_on_rx(ob_t *ob, uint8_t const hash[OB_HASH_LEN]);
void ob_on_confirm(ob_t *ob, uint32_t ack, uint32_t trip_ms);
// A failed message starts a new cycle under the same id (a new timestamp). false when it is not failed or busy.
bool ob_retry(ob_t *ob, mesh_msg_id_t id, uint32_t now_ms);
// Where the pipe takes its next frame from while its internal FIFO (a drain refills it after every reply) and the
// outbox both have work: they alternate, so a send never waits for a whole drain.
typedef struct {
    bool last_internal;
} ob_arb_t;
// true: try the outbox before the FIFO
bool ob_arb_outbox_first(ob_arb_t const *arb, bool fifo_has);
void ob_arb_took(ob_arb_t *arb, bool internal);

// ob_get false for an id means finished and evicted (the table keeps the newest OB_CAP) or never queued
bool ob_get(ob_t const *ob, mesh_msg_id_t id, mesh_out_state_t *st, uint8_t *attempt, uint8_t *repeats,
            uint32_t *trip_ms);

#ifdef __cplusplus
}
#endif
