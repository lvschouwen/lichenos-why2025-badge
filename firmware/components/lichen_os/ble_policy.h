#pragma once

#include "lichen_bt.h"

#include <stdbool.h>
#include <stdint.h>

// The Bluetooth service's rules (#73, #599), pure and host-tested (ble_policy_test.c): no NimBLE, no ESP headers, no
// statics; every call gets the caller's state and the time (a monotonic ms counter; differences are wrap-safe, and what
// runs out is latched by ble_pair_tick). Not linked into the image until the service (#73 B2) uses it.
// - Pairing (OS-only): a window of BLE_PAIR_WINDOW_MS opened from Settings > Bluetooth; one pairing at a time;
//   BLE_PAIR_FAILS_MAX failures lock pairing out for BLE_PAIR_LOCKOUT_MS (re-opening the window keeps the lockout);
//   a full bond store refuses (the person forgets a device first, nothing is evicted). A success closes the window.
// - Access: a characteristic's level against the link's security, answered as the ATT error that makes a phone pair.
// - The connection cap: BLE_CONN_MAX links, at most BLE_CONN_PERIPHERAL_MAX that peers made to us and
//   BLE_CONN_CENTRAL_MAX that we made.
// - A stranger's link: one a peer made to us that is not bonded with authenticated keys is dropped
//   BLE_UNBONDED_DROP_MS after it came up, or BLE_PAIR_SMP_MS after its pairing started. Links we made (an app's GATT client) and the peer session's links
//   (never bonded, the session has its own timeouts) are not strangers.
// - The passkey: 6 digits drawn without bias, shown "123 456".
// - The peer session's links pair by numeric comparison outside all of this: no window, no lockout, no bond store
//   (they are never bonded); its own invite rate limit and the person's key press guard them (peer_session.h).

#define BLE_PAIR_WINDOW_MS      120000u
#define BLE_PAIR_FAILS_MAX      3u
#define BLE_PAIR_LOCKOUT_MS     60000u
#define BLE_PAIR_SMP_MS         30000u // the SMP timeout (Core spec, vol 3 part H 3.4): a pairing under way ends by then
#define BLE_UNBONDED_DROP_MS    10000u
#define BLE_BONDS_MAX           3u
#define BLE_CONN_MAX            3u
#define BLE_CONN_PERIPHERAL_MAX 2u
#define BLE_CONN_CENTRAL_MAX    1u
#define BLE_PASSKEY_MAX         999999u
#define BLE_PASSKEY_TEXT        8u // "123 456" and the NUL

// ---- Pairing window and attempts ----

typedef struct {
    bool     open;
    bool     busy;  // a pairing is under way (since busy_ms; it fails after BLE_PAIR_SMP_MS)
    uint8_t  fails; // since the last success or lockout
    bool     locked;
    uint32_t opened_ms;
    uint32_t locked_ms;
    uint32_t busy_ms;
} ble_pair_t;

typedef enum {
    BLE_PAIR_GO,         // start it
    BLE_PAIR_CLOSED,     // no window: refuse (and disconnect)
    BLE_PAIR_LOCKED,     // too many failures: refuse until the lockout ends
    BLE_PAIR_BONDS_FULL, // the store has BLE_BONDS_MAX: "Forget a device first"
    BLE_PAIR_BUSY,       // another pairing is under way
} ble_pair_verdict_t;

void               ble_pair_init(ble_pair_t *p);
// Settings opened (or re-opened) the window: BLE_PAIR_WINDOW_MS from now. A lockout stays.
void               ble_pair_open(ble_pair_t *p, uint32_t now_ms);
void               ble_pair_close(ble_pair_t *p);
// Latches what ran out (the window closes, a lockout ends, a pairing under way past BLE_PAIR_SMP_MS fails): call it on the service's timer, at least daily, so a
// window or lockout never reads as current again after the ms counter wraps (49.7 days). ble_pair_begin does it too.
void               ble_pair_tick(ble_pair_t *p, uint32_t now_ms);
bool               ble_pair_is_open(ble_pair_t const *p, uint32_t now_ms);
// The window's time left (the countdown on screen), 0 when closed
uint32_t           ble_pair_left_ms(ble_pair_t const *p, uint32_t now_ms);
// The lockout's time left, 0 when none
uint32_t           ble_pair_locked_ms(ble_pair_t const *p, uint32_t now_ms);
// A peer asks to pair; bonds is how many the store holds. BLE_PAIR_GO marks it busy until ble_pair_end.
ble_pair_verdict_t ble_pair_begin(ble_pair_t *p, uint32_t now_ms, unsigned bonds);
// The pairing that began ended: bonded, and with MITM-protected keys (authenticated). Anything less is a failure: it
// counts towards the lockout, the window stays open, and true says disconnect the link and delete any bond the stack
// stored for it (a Just Works bond is never kept). True too for an end without a begin (stale, or the pairing timed
// out in ble_pair_tick): whatever it bonded is not trusted.
bool               ble_pair_end(ble_pair_t *p, uint32_t now_ms, bool bonded, bool authenticated);

// ---- Access levels ----

typedef struct {
    bool encrypted;
    bool authenticated; // MITM-protected keys (passkey or numeric comparison)
    bool bonded;
} ble_link_sec_t;

typedef enum { BLE_ACCESS_OK, BLE_ACCESS_NEED_ENC, BLE_ACCESS_NEED_AUTH } ble_access_t;

ble_access_t ble_access(lichen_bt_level_t need, ble_link_sec_t sec);
// The ATT error for a refusal: 0x0f Insufficient Encryption, 0x05 Insufficient Authentication, 0 for OK
uint8_t      ble_access_att_error(ble_access_t a);

// ---- The connection cap ----

typedef struct {
    uint8_t central;    // links we made
    uint8_t peripheral; // links peers made to us
} ble_conns_t;

bool ble_conn_may(ble_conns_t const *c, lichen_bt_role_t role);
// Counts a link that came up: false when the cap refuses it (the service drops it; nothing counted)
bool ble_conn_add(ble_conns_t *c, lichen_bt_role_t role);
void ble_conn_remove(ble_conns_t *c, lichen_bt_role_t role);
// Connectable advertising only while one more peer may connect to us
bool ble_conn_may_advertise(ble_conns_t const *c);

// ---- A stranger's link ----

typedef enum { BLE_LINK_PLAIN, BLE_LINK_PEER } ble_link_kind_t; // PEER: the peer session's link

typedef struct {
    lichen_bt_role_t role;
    ble_link_kind_t  kind;
    ble_link_sec_t   sec;
    uint32_t         up_ms;
    bool             pairing; // a pairing on this link is under way
    uint32_t         pairing_ms;
} ble_link_t;

void ble_link_init(ble_link_t *l, lichen_bt_role_t role, uint32_t now_ms);
void ble_link_pairing(ble_link_t *l, uint32_t now_ms, bool under_way);
// The peer service took the link: its first frame on the peer session's PSM parsed (an INVITE). From then on the
// session's own timeouts (answer 30 s, pairing 30 s, silence 20 s) hold it, not the 10 s drop.
void ble_link_set_peer(ble_link_t *l);
bool ble_link_should_drop(ble_link_t const *l, uint32_t now_ms);

// ---- The passkey ----

// r: 32 random bits. False when r falls in the tail that would bias the digits: draw again.
bool ble_passkey_from_random(uint32_t r, uint32_t *passkey);
// "123 456" (leading zeros kept: "004 096"); false (out = "") above BLE_PASSKEY_MAX
bool ble_passkey_format(uint32_t passkey, char out[BLE_PASSKEY_TEXT]);
