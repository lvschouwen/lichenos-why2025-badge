#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bluetooth LE as a Lichen OS service (#73, API of #599). Declarations only: nothing defines these calls yet, the
// service behind them is #73's B2 (server), C1 (central and GATT client), C3 (SD apps) and P1 (peer session).
//
// The OS owns the stack, the advertising set, the name, the address (RPA privacy), pairing and bonds. An app (CAP_BT)
// adds value-backed GATT services, asks for advertising, connects as a central and uses the peer session; scanning
// needs CAP_RADIO_SCAN. The rules every call keeps:
// - No app code ever runs on the Bluetooth host task. A peer's read is answered from the value copy the app last set
//   (lichen_bt_value_set); a peer's write, a subscription, a scan result or a GATT client result is an event queued to
//   the app and handed to it on the task it chose in lichen_bt_open (its own task, or the LVGL task).
// - Nothing blocks on the radio: a call that would have to wait returns LICHEN_BT_BUSY (notify with a full queue, a
//   peer message with a full window) and the app tries again on a LICHEN_BT_EV_READY event.
// - Every service, scan and connection belongs to the app that made it. When the app stops, or before an SD app's
//   memory is freed, the OS removes them all (revoke); a handle of an app that stopped is refused, never reused.
// - Pairing, bonds and Forget are the OS's (Settings > Bluetooth): an app never sees a passkey or a bond.
// Pure types and constants: bt_registry.c, ble_policy.c and peer_session.c (lichen_os, host-tested) share them.

// ---- Types shared with the OS's pure parts ----

// A UUID as it goes on air (little-endian): len 2 (a 16-bit SIG UUID) or 16 (a 128-bit one)
typedef struct {
    uint8_t len;
    uint8_t b[16];
} lichen_bt_uuid_t;

// What a characteristic needs from the link before a peer may read, write or subscribe (ble_policy.c enforces it):
// OPEN anyone connected; ENCRYPTED an encrypted link (the peer session's numeric-comparison links); BONDED an
// authenticated (MITM: passkey or numeric comparison) and bonded link (a paired phone).
typedef enum { LICHEN_BT_OPEN, LICHEN_BT_ENCRYPTED, LICHEN_BT_BONDED } lichen_bt_level_t;

// Our side of a connection: CENTRAL when we connected out, PERIPHERAL when the peer connected to us
typedef enum { LICHEN_BT_ROLE_CENTRAL, LICHEN_BT_ROLE_PERIPHERAL } lichen_bt_role_t;

typedef enum {
    LICHEN_BT_OK,
    LICHEN_BT_BUSY,          // try again on LICHEN_BT_EV_READY (a queue or window is full, a scan is running)
    LICHEN_BT_OFF,           // Bluetooth is off (the switch, airplane mode, a hold)
    LICHEN_BT_NO_CAP,        // the app did not declare CAP_BT (or CAP_RADIO_SCAN for a scan)
    LICHEN_BT_INVALID,       // a bad argument: length, UUID, properties, a reserved UUID
    LICHEN_BT_FULL,          // no room: services, characteristics, connections (the cap), the bond store
    LICHEN_BT_EXISTS,        // a service with this UUID is already there
    LICHEN_BT_NOT_FOUND,     // the handle is unknown, stale or another app's
    LICHEN_BT_NOT_CONNECTED, // the connection is gone, or the peer session is not open
    LICHEN_BT_REFUSED,       // the peer refused (declined, auth failed), or the person did
} lichen_bt_err_t;

// Characteristic properties, the ATT bits
#define LICHEN_BT_PROP_READ     0x02u
#define LICHEN_BT_PROP_WRITE_NR 0x04u // write without response
#define LICHEN_BT_PROP_WRITE    0x08u
#define LICHEN_BT_PROP_NOTIFY   0x10u
#define LICHEN_BT_PROP_INDICATE 0x20u
#define LICHEN_BT_PROP_ALL      0x3eu

#define LICHEN_BT_VALUE_MAX       512u // the ATT maximum attribute length
#define LICHEN_BT_SVC_CHRS_MAX    8u   // characteristics per service
#define LICHEN_BT_PEER_MSG_MAX    512u // one peer-session message
// The app's part of a peer invite: the OS adds the app id and version, the name and a nonce, 64 B in all
#define LICHEN_BT_PEER_PARAM_MAX  14u
// A peer's display name in bytes (UTF-8), without the NUL
#define LICHEN_BT_NAME_MAX        28u
// L2CAP channels (LE credit-based, CoC): the dynamic PSM range, and the one the OS keeps for the peer session
#define LICHEN_BT_PSM_MIN         0x0080u
#define LICHEN_BT_PSM_MAX         0x00ffu
#define LICHEN_BT_PSM_PEER        0x00c1u
#define LICHEN_BT_CHANNEL_MTU_MAX 512u

// The Lichen peer service (badge to badge, #73 P1): 98b8aa8d-5357-4fa1-92d2-f5fb04e3d68c, on-air byte order.
// Reserved for the OS: an app's lichen_bt_service_add with it is LICHEN_BT_INVALID.
#define LICHEN_BT_PEER_UUID                                                                                            \
    {16, {0x8c, 0xd6, 0xe3, 0x04, 0xfb, 0xf5, 0xd2, 0x92, 0xa1, 0x4f, 0x57, 0x53, 0x8d, 0xaa, 0xb8, 0x98}}

// ---- The peer session's wire format: the contract another device implements (the ESP32 test peer, #73 A7) ----
// Every frame on the peer session's L2CAP channel (PSM LICHEN_BT_PSM_PEER), all fields little-endian:
//   byte 0    version: LICHEN_BT_PEER_WIRE_VERSION. A receiver drops a frame of any other version (a later version
//             changes this byte; nothing else of the header is reserved).
//   byte 1    type, below; a receiver drops an unknown one (7..255 are reserved)
//   bytes 2-3 seq: DATA only, 0 for the others
//   bytes 4-5 ack: in every frame, the next DATA seq the sender expects (cumulative)
//   bytes 6-7 payload length; the frame is exactly 8 + length bytes
//   types: 1 INVITE (1..64 B: nonce u32, app id length u8 + id [a-z0-9_]{1,13}, app version u16, name length u8 +
//          name (no bytes below 0x20 or 0x7f, up to LICHEN_BT_NAME_MAX), param length u8 + param (up to
//          LICHEN_BT_PEER_PARAM_MAX), nothing after); 2 ACCEPT (0 B); 3 DECLINE (0 B, or 1 B = 2: the receiver lost a
//          glare and should wait for the sender's invite); 4 DATA (1..LICHEN_BT_PEER_MSG_MAX B); 5 ACK (0 B, also the
//          keepalive); 6 END (0 B, or 1 B = 1: the sender's app exited)
// The channel after both sides are secured: DATA seq starts at 0 and adds 1 per message (mod 2^16). At most
// LICHEN_BT_PEER_WINDOW messages are unacked; past that the sender's app gets BUSY. A receiver takes only the seq it
// expects; a duplicate (a resend) is acked again and dropped, never delivered twice; a message it has no room for is
// not acked. The sender resends from its oldest unacked message after LICHEN_BT_PEER_RTO_MS without progress, at
// most LICHEN_BT_PEER_RETRIES times, then ends the session. A side sends an ACK when LICHEN_BT_PEER_KEEPALIVE_MS
// passed without a frame of its own, and ends the session after LICHEN_BT_PEER_SILENCE_MS without one from the peer.
#define LICHEN_BT_PEER_WIRE_VERSION 1u
#define LICHEN_BT_PEER_HDR          8u
#define LICHEN_BT_PEER_INVITE_MAX   64u
#define LICHEN_BT_PEER_WINDOW       4u
#define LICHEN_BT_PEER_RTO_MS       2000u
#define LICHEN_BT_PEER_RETRIES      5u
#define LICHEN_BT_PEER_KEEPALIVE_MS 5000u
#define LICHEN_BT_PEER_SILENCE_MS   20000u
#define LICHEN_BT_PEER_F_INVITE     1u
#define LICHEN_BT_PEER_F_ACCEPT     2u
#define LICHEN_BT_PEER_F_DECLINE    3u
#define LICHEN_BT_PEER_F_DATA       4u
#define LICHEN_BT_PEER_F_ACK        5u
#define LICHEN_BT_PEER_F_END        6u

// ---- Handles. 0 is never valid; a handle carries a generation, so one of an app that stopped never matches. ----
typedef uint32_t lichen_bt_svc_t;
typedef uint32_t lichen_bt_chr_t;
typedef uint32_t lichen_bt_conn_t;     // one connection (server or client side)
typedef uint32_t lichen_bt_rchr_t;     // a characteristic on a peer we connected to (GATT client)
typedef uint32_t lichen_bt_channel_t;  // an L2CAP channel
typedef uint32_t lichen_bt_peer_t;     // a badge seen by peer discovery (the OS keeps its address)
#define LICHEN_BT_CONN_ALL 0xffffffffu // notify: every subscribed connection

typedef struct {
    uint8_t type; // 0 public, 1 random (a phone's or badge's RPA changes: keep it only for the session)
    uint8_t a[6];
} lichen_bt_addr_t;

// ---- Opening and events ----

typedef enum {
    LICHEN_BT_EV_READY,        // room again after a BUSY (notify queue, peer window)
    LICHEN_BT_EV_STATE,        // Bluetooth on/off changed (lichen_bt_on)
    LICHEN_BT_EV_CONNECTED,    // conn, addr, role: a peer connected to the app's service, or lichen_bt_connect made it
    LICHEN_BT_EV_DISCONNECTED, // conn
    LICHEN_BT_EV_WRITE,        // conn, chr, data/len: a peer wrote; the value copy holds it too
    LICHEN_BT_EV_SUBSCRIBE,    // conn, chr, len = 1 subscribed, 0 unsubscribed
    LICHEN_BT_EV_SCAN_RESULT,  // addr, rssi, data/len: the advertising data, then the scan response
    LICHEN_BT_EV_SCAN_DONE,
    LICHEN_BT_EV_GATTC_FOUND,    // conn, rchr, status: lichen_bt_gattc_find's answer
    LICHEN_BT_EV_GATTC_READ,     // conn, rchr, status, data/len
    LICHEN_BT_EV_GATTC_WRITTEN,  // conn, rchr, status
    LICHEN_BT_EV_GATTC_NOTIFY,   // conn, rchr, data/len: a notification or indication from the peer
    LICHEN_BT_EV_CHANNEL_OPEN,   // chan, conn, status: lichen_bt_channel_open's answer, or a peer opened a listened PSM
    LICHEN_BT_EV_CHANNEL_DATA,   // chan, data/len: one SDU, in order
    LICHEN_BT_EV_CHANNEL_CLOSED, // chan
    LICHEN_BT_EV_PEER_FOUND,     // peer, rssi, data/len: the badge's name
    LICHEN_BT_EV_PEER_INVITE,    // version, data/len: the inviter's app version and param; the person accepted already
    LICHEN_BT_EV_PEER_OPEN,      // role: the session is open both ways
    LICHEN_BT_EV_PEER_MSG,       // data/len: one message, in order, exactly once
    LICHEN_BT_EV_PEER_CLOSED, // status: LICHEN_BT_OK ended by either side, REFUSED declined, NOT_CONNECTED lost/timeout
} lichen_bt_ev_type_t;

typedef struct {
    lichen_bt_ev_type_t type;
    lichen_bt_err_t     status;
    lichen_bt_conn_t    conn;
    lichen_bt_chr_t     chr;
    lichen_bt_rchr_t    rchr;
    lichen_bt_peer_t    peer;
    lichen_bt_channel_t chan;
    uint16_t            version; // PEER_INVITE: the inviting app's version
    lichen_bt_addr_t    addr;
    lichen_bt_role_t    role;
    int8_t              rssi;
    uint16_t            len;
    uint8_t const      *data; // the OS's copy, valid until the callback returns
} lichen_bt_event_t;

typedef void (*lichen_bt_on_event_t)(lichen_bt_event_t const *ev, void *arg);

// Where the app's events run: on the LVGL task (through lichen_ui_call, for a screen) or on the app's own task, which
// calls lichen_bt_pump (needs CAP_BACKGROUND for the task)
typedef enum { LICHEN_BT_DELIVER_UI, LICHEN_BT_DELIVER_TASK } lichen_bt_deliver_t;

// Becomes a Bluetooth client: from any task, once per app (again replaces the callback). Events queued before a
// callback runs are bounded per app; the oldest scan results go first when the queue is full, never a write.
lichen_bt_err_t lichen_bt_open(lichen_bt_on_event_t on_event, void *arg, lichen_bt_deliver_t deliver);
// Ends it: every service, scan, connection and peer session of the app goes, and its queued events are dropped.
// Never on the LVGL task (it waits for the stack to confirm); the OS calls it itself when the app stops.
void            lichen_bt_close(void);
// From the app's own task (DELIVER_TASK): runs the queued events' callbacks there, waiting up to wait_ms for the
// first. The number run. Never on the LVGL task.
int             lichen_bt_pump(uint32_t wait_ms);
// Bluetooth is on (the switch, airplane mode and holds; the person's choice, never an app's). Any task.
bool            lichen_bt_on(void);

// ---- GATT server: value-backed services ----

typedef struct {
    lichen_bt_uuid_t  uuid;
    uint8_t           props;   // LICHEN_BT_PROP_*
    lichen_bt_level_t level;   // the least the link must have for every access
    uint16_t          max_len; // the value copy's size, 1..LICHEN_BT_VALUE_MAX
} lichen_bt_chr_def_t;

typedef struct {
    lichen_bt_uuid_t           uuid;
    lichen_bt_chr_def_t const *chrs;
    uint8_t                    chr_count; // 1..LICHEN_BT_SVC_CHRS_MAX
    bool                       advertise; // put the UUID in the advert (rotated with others when they do not fit)
} lichen_bt_svc_def_t;

// Adds a service while connections stay up (bonded peers get Service Changed). chrs_out gets one handle per
// characteristic, in def order. The OS copies def. Any task, it does not wait on the radio.
lichen_bt_err_t lichen_bt_service_add(lichen_bt_svc_def_t const *def, lichen_bt_svc_t *svc, lichen_bt_chr_t *chrs_out);
// Removes it; its handles are refused from now on. Any task.
lichen_bt_err_t lichen_bt_service_remove(lichen_bt_svc_t svc);
// The value a peer reads from now on (copied, up to the characteristic's max_len). Any task.
lichen_bt_err_t lichen_bt_value_set(lichen_bt_chr_t chr, void const *data, size_t len);
// The current value copy (the app's last set, or a peer's last write): its length, or -1 for a bad handle. Any task.
int             lichen_bt_value_get(lichen_bt_chr_t chr, void *buf, size_t cap);
// Notifies (or indicates) conn, or every subscribed one with LICHEN_BT_CONN_ALL. Never blocks: BUSY when the queue is
// full, NOT_CONNECTED when nobody is subscribed. Any task.
lichen_bt_err_t lichen_bt_notify(lichen_bt_chr_t chr, lichen_bt_conn_t conn, void const *data, size_t len);
// Wants the OS to advertise (connectable, the app's advertise services' UUIDs). The OS advertises while any client
// wants it, fast for about 30 s, then slow, and never past the connection cap. Any task.
lichen_bt_err_t lichen_bt_advertise(bool want);
// Drops one of the app's connections (LICHEN_BT_EV_DISCONNECTED follows). Any task.
lichen_bt_err_t lichen_bt_disconnect(lichen_bt_conn_t conn);

// ---- Central: scan, connect, GATT client ----

// Scans for duration_ms (at most 30 s), only advertisers of uuid when it is not NULL. CAP_RADIO_SCAN. BUSY while
// another app scans. Results are LICHEN_BT_EV_SCAN_RESULT events, then LICHEN_BT_EV_SCAN_DONE. Any task.
lichen_bt_err_t lichen_bt_scan_start(lichen_bt_uuid_t const *uuid, uint32_t duration_ms);
// Ends the app's scan early (LICHEN_BT_EV_SCAN_DONE follows). Any task.
lichen_bt_err_t lichen_bt_scan_stop(void);
// Connects to addr as the central (one central link for the whole badge, 2 peripheral + 1 central in all): FULL when
// the cap is reached. LICHEN_BT_EV_CONNECTED or _DISCONNECTED (status) follows. Any task.
lichen_bt_err_t lichen_bt_connect(lichen_bt_addr_t const *addr, uint32_t timeout_ms, lichen_bt_conn_t *conn);
// Finds a characteristic on the peer; LICHEN_BT_EV_GATTC_FOUND gives its handle. Any task.
lichen_bt_err_t lichen_bt_gattc_find(lichen_bt_conn_t conn, lichen_bt_uuid_t const *svc, lichen_bt_uuid_t const *chr);
// Reads it: LICHEN_BT_EV_GATTC_READ. Any task.
lichen_bt_err_t lichen_bt_gattc_read(lichen_bt_rchr_t rchr);
// with_response false: write without response. Any task.
lichen_bt_err_t lichen_bt_gattc_write(lichen_bt_rchr_t rchr, void const *data, size_t len, bool with_response);
// Turns its notifications (or indications) on or off: LICHEN_BT_EV_GATTC_NOTIFY per value. Any task.
lichen_bt_err_t lichen_bt_gattc_subscribe(lichen_bt_rchr_t rchr, bool on);

// ---- L2CAP channels: streams over a connection (CAP_BT) ----
// For bulk or stream data (files, a game's own protocol): in order, with the transport's credit flow control. The
// PSM is LICHEN_BT_PSM_MIN..LICHEN_BT_PSM_MAX except LICHEN_BT_PSM_PEER; mtu is the largest SDU, up to
// LICHEN_BT_CHANNEL_MTU_MAX.

// Accepts channels peers open on psm (LICHEN_BT_EV_CHANNEL_OPEN for each); on false, stops. EXISTS when another app
// listens there. Any task.
lichen_bt_err_t lichen_bt_channel_listen(uint16_t psm, uint16_t mtu, bool on);
// Opens a channel on one of the app's connections; LICHEN_BT_EV_CHANNEL_OPEN gives the status. Any task.
lichen_bt_err_t lichen_bt_channel_open(lichen_bt_conn_t conn, uint16_t psm, uint16_t mtu, lichen_bt_channel_t *chan);
// Sends one SDU (up to the peer's mtu). Never blocks: BUSY while the peer has no credits or the queue is full,
// LICHEN_BT_EV_READY follows. Any task.
lichen_bt_err_t lichen_bt_channel_send(lichen_bt_channel_t chan, void const *data, size_t len);
// Closes it (LICHEN_BT_EV_CHANNEL_CLOSED follows). Any task.
lichen_bt_err_t lichen_bt_channel_close(lichen_bt_channel_t chan);

// ---- Peer session: badge to badge (Battleship) ----
// One session per badge at a time. Discovery sees badges that are Visible (Settings or the app turns it on, 10 min).
// The inviter becomes the central. The invitee's person accepts on the OS's dialog, then both screens confirm the
// same 6-digit number (numeric comparison; encrypted, never bonded). The channel is ordered, exactly once, up to
// LICHEN_BT_PEER_MSG_MAX per message, carried on an L2CAP channel; it closes after 20 s of silence (the OS keeps it
// alive every 5 s).

// Makes this badge Visible for 10 min (or ends it): it advertises the peer service and accepts invites. Any task.
lichen_bt_err_t lichen_bt_peer_visible(bool on);
// Finds Visible badges (CAP_BT; a scan the OS filters to the peer service): LICHEN_BT_EV_PEER_FOUND events. Any task.
lichen_bt_err_t lichen_bt_peer_discover(bool on);
// Invites peer to this app (the invitee needs the same app id) at the app's protocol version, with up to
// LICHEN_BT_PEER_PARAM_MAX bytes of the app's own (a game's rules). BUSY while a session is under way; FULL when the
// central link is taken. One invite per peer per 10 s is shown there. Any task.
lichen_bt_err_t lichen_bt_peer_invite(lichen_bt_peer_t peer, uint16_t version, void const *param, size_t len);
// Queues one message. BUSY when the window is full (LICHEN_BT_EV_READY follows), NOT_CONNECTED unless open. Any task.
lichen_bt_err_t lichen_bt_peer_send(void const *data, size_t len);
// Ends the session (the other side gets LICHEN_BT_EV_PEER_CLOSED with LICHEN_BT_OK). The OS ends it too when the
// app stops, and the other side learns that its peer's app exited. Any task.
lichen_bt_err_t lichen_bt_peer_end(void);

#ifdef __cplusplus
}
#endif
