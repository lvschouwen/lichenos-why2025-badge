#pragma once

#include "lichen_bt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The badge-to-badge session (#73 P1, pure part of #599; Battleship first), host-tested in peer_session_test.c. No
// NimBLE, no statics, no heap: the service (#73 P1) keeps one peer_session_t (about 4.3 KB, in PSRAM), feeds it the
// frames of the session's link (an L2CAP CoC) and the time, sends what peer_session_next_frame gives and acts on
// peer_session_take_events. Not linked into the image until then.
//
// Flow: the invitee is Visible (PEER_VISIBLE_MS); the inviter (the central) connects and sends INVITE; the invitee's
// person accepts on the OS's dialog (ACCEPT) or declines (DECLINE); the central starts pairing (numeric comparison,
// both screens confirm, encrypted, never bonded); peer_session_secured opens the session on both sides. Open: DATA
// messages with sequence numbers and a cumulative ack in every frame, at most PEER_TXQ in flight (send says BUSY past
// that), go-back-N retransmission every PEER_RTO_MS up to PEER_RETRIES times, PEER_RXQ messages waiting for the app
// (past that a message is not acked, so the sender sends it again later). An ACK frame keeps the link alive after
// PEER_KEEPALIVE_MS without a frame; PEER_SILENCE_MS without one from the peer ends the session.
// The CoC already delivers in order with credit flow control; the sequence numbers and acks stay on top of it because
// they are cheap (8 B a frame) and say what the other side's app actually took: the app queue's backpressure (a full
// PEER_RXQ is not acked), a "sent" the app can trust across a lost link, and the same protocol over a GATT fallback
// for the ESP32 test peer. Over a healthy CoC the retransmission never fires. Either side's app exiting ends the
// session with END + PEER_WHY_APP_EXIT (peer_session_app_exit).
//
// Glare: both badges invite each other at once (each is central on its own link and peripheral on the other's). The
// invite with the larger nonce wins on both sides: the loser drops its own invite and link and takes the winner's
// (PEER_RX_YIELD), the winner refuses the loser's (PEER_RX_REFUSE). Equal nonces: both refuse, both invites end.
//
// Wire format, little-endian: version (1), type, seq (2), ack (2), payload length (2), payload.

#define PEER_VERSION       1u
#define PEER_HDR           8u
#define PEER_MSG_MAX       LICHEN_BT_PEER_MSG_MAX
#define PEER_FRAME_MAX     (PEER_HDR + PEER_MSG_MAX)
#define PEER_INVITE_MAX    64u
#define PEER_APP_MAX       13u // an app id (app_ns rules: a-z 0-9 _)
#define PEER_NAME_MAX      LICHEN_BT_NAME_MAX
#define PEER_PARAM_MAX     LICHEN_BT_PEER_PARAM_MAX
#define PEER_VISIBLE_MS    600000u
#define PEER_INVITE_GAP_MS 10000u // incoming invites: one shown per peer every 10 s
#define PEER_RATE_SLOTS    8u     // peers the rate limit remembers
#define PEER_ANSWER_MS     30000u // the person answers an invite
#define PEER_PAIR_MS       30000u // numeric comparison on both screens
#define PEER_KEEPALIVE_MS  5000u
#define PEER_SILENCE_MS    20000u
#define PEER_RTO_MS        2000u
#define PEER_RETRIES       5u
#define PEER_TXQ           4u
#define PEER_RXQ           4u

typedef enum {
    PEER_F_INVITE = 1,
    PEER_F_ACCEPT,
    PEER_F_DECLINE,
    PEER_F_DATA,
    PEER_F_ACK, // also the keepalive
    PEER_F_END,
} peer_ftype_t;

typedef struct {
    peer_ftype_t   type;
    uint16_t       seq;
    uint16_t       ack;
    uint16_t       len;
    uint8_t const *payload; // into the parsed buffer
} peer_frame_t;

// False for anything malformed: version, type, length field against the buffer, a payload too long for its type
bool   peer_frame_parse(uint8_t const *buf, size_t len, peer_frame_t *out);
// The frame's length, 0 when it does not fit cap or the payload is too long for the type
size_t peer_frame_build(
    peer_ftype_t type, uint16_t seq, uint16_t ack, void const *payload, size_t len, uint8_t *out, size_t cap
);

typedef struct {
    uint32_t nonce; // the glare tie-break: random per invite
    char     app[PEER_APP_MAX + 1];
    uint16_t app_version;             // the inviter's app version: the invitee's app decides what it can play with
    char     name[PEER_NAME_MAX + 1]; // the inviter's display name: printable, no control bytes
    uint8_t  param_len;
    uint8_t  param[PEER_PARAM_MAX];
} peer_invite_t;

// nonce (4), app length + app, app version (2), name length + name, param length + param: at most PEER_INVITE_MAX
// (64). 0 when invalid.
size_t peer_invite_encode(peer_invite_t const *inv, uint8_t *out, size_t cap);
bool   peer_invite_decode(uint8_t const *buf, size_t len, peer_invite_t *out);

typedef enum { PEER_IDLE, PEER_INVITING, PEER_INVITED, PEER_PAIRING, PEER_OPEN, PEER_CLOSED } peer_state_t;

typedef enum {
    PEER_END_NONE,
    PEER_END_LOCAL,       // this side ended it
    PEER_END_REMOTE,      // the peer sent END
    PEER_END_DECLINED,    // the invite was declined (either side)
    PEER_END_NO_ANSWER,   // nobody answered the invite in time
    PEER_END_PAIR_FAILED, // pairing failed or timed out
    PEER_END_SILENCE,     // nothing from the peer for PEER_SILENCE_MS
    PEER_END_RETRIES,     // a message was never acked
    PEER_END_LOST,        // the link went down
    PEER_END_APP_EXIT,    // this side's app exited
    PEER_END_REMOTE_EXIT, // the peer's app exited
} peer_end_t;

// Events (bits) for the service, taken with peer_session_take_events
#define PEER_EV_INVITE 0x01u // INVITED: ask the person (s->invite holds it)
#define PEER_EV_PAIR   0x02u // the central starts pairing now
#define PEER_EV_OPEN   0x04u
#define PEER_EV_MSG    0x08u // peer_session_recv has a message
#define PEER_EV_READY  0x10u // the window has room again after a BUSY
#define PEER_EV_CLOSED 0x20u // s->end says why; the service drops the link once next_frame gives nothing more

typedef enum {
    PEER_RX_OK,      // taken
    PEER_RX_IGNORED, // nothing to do with the session now
    PEER_RX_BAD,     // malformed: drop the link when it is not the session's
    PEER_RX_REFUSE,  // an invite refused: send DECLINE on that link and drop it
    PEER_RX_YIELD,   // glare lost: drop s->yielded_link (our own invite); the incoming one is the session now
} peer_rx_t;

typedef struct {
    bool     used;
    uint32_t key; // the service's id of the inviter (its address, hashed)
    uint32_t ms;  // when its last invite was shown
} peer_rate_t;

// END's optional reason byte
#define PEER_WHY_APP_EXIT 1u

typedef struct {
    uint16_t seq;
    uint16_t len;
    uint8_t  data[PEER_MSG_MAX];
} peer_msg_t;

typedef struct {
    peer_state_t     state;
    peer_end_t       end;
    lichen_bt_role_t role;
    uint16_t         link;
    uint16_t         yielded_link;
    uint32_t         state_ms;
    uint32_t         last_rx_ms;
    uint32_t         last_tx_ms;
    bool             visible;
    uint32_t         visible_ms;
    peer_rate_t      rate[PEER_RATE_SLOTS];
    uint8_t          why;    // the END frame's reason byte (PEER_WHY_*), 0 none
    peer_invite_t    invite; // ours while INVITING, the peer's once INVITED
    uint8_t          ctl;    // a control frame to send (peer_ftype_t), 0 none
    bool             ack_due;
    bool             was_busy;
    uint8_t          events;
    // sending: txq[0..txq_n) from tx_base on, the first tx_sent of them sent in this round
    uint16_t         tx_base;
    uint8_t          txq_n;
    uint8_t          tx_sent;
    uint8_t          tx_ever; // of txq, how many went out at least once (an ack past them is a lie)
    uint8_t          retries;
    uint32_t         rto_ms;
    peer_msg_t       txq[PEER_TXQ];
    // receiving: rx_next is the next seq to take; rxq is a ring of rxq_n from rxq_head
    uint16_t         rx_next;
    uint8_t          rxq_head;
    uint8_t          rxq_n;
    peer_msg_t       rxq[PEER_RXQ];
} peer_session_t;

void            peer_session_init(peer_session_t *s);
// Visible for PEER_VISIBLE_MS from now, or not; peer_session_visible says whether it still is
void            peer_session_set_visible(peer_session_t *s, uint32_t now_ms, bool on);
bool            peer_session_visible(peer_session_t const *s, uint32_t now_ms);
// We invite on link (we are its central): IDLE only (LICHEN_BT_BUSY otherwise), INVALID for an invite that does not
// encode. The INVITE frame follows from next_frame.
lichen_bt_err_t peer_session_invite(peer_session_t *s, uint32_t now_ms, uint16_t link, peer_invite_t const *inv);
// A frame that came in on link from the peer the service knows as peer_key (the invite rate limit's key)
peer_rx_t       peer_session_on_frame(
          peer_session_t *s, uint32_t now_ms, uint16_t link, uint32_t peer_key, uint8_t const *buf, size_t len
      );
// The person answered the invite (INVITED only; false otherwise)
bool            peer_session_answer(peer_session_t *s, uint32_t now_ms, bool accept);
// Pairing ended on the session's link: ok opens the session
void            peer_session_secured(peer_session_t *s, uint32_t now_ms, bool ok);
void            peer_session_link_down(peer_session_t *s, uint32_t now_ms, uint16_t link);
// Ends a session under way (END goes out); nothing when IDLE or CLOSED
void            peer_session_end(peer_session_t *s, uint32_t now_ms);
// The app on this side exits (or stops): END with the reason, so the other side's app sees why. Nothing when idle.
void            peer_session_app_exit(peer_session_t *s, uint32_t now_ms);
// The timeouts and retransmission: call it at least every few hundred ms
void            peer_session_tick(peer_session_t *s, uint32_t now_ms);
// OPEN only (NOT_CONNECTED), 1..PEER_MSG_MAX bytes (INVALID), BUSY with PEER_TXQ in flight
lichen_bt_err_t peer_session_send(peer_session_t *s, void const *data, size_t len);
// The next message in order: its length, or -1 when none (or cap is too small: it stays)
int             peer_session_recv(peer_session_t *s, void *buf, size_t cap);
// The next frame to send on s->link now: its length, 0 for nothing. Call until 0 after every event.
size_t          peer_session_next_frame(peer_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap);
uint8_t         peer_session_take_events(peer_session_t *s);
// After CLOSED: back to IDLE (Visible and the invite rate limit are kept)
void            peer_session_reset(peer_session_t *s);
