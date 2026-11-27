#pragma once

#include "lichen_bt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Bluetooth service's books (#73, #599), pure and host-tested (bt_registry_test.c): who owns which GATT service,
// characteristic, scan, central connection and queued event, and the advert built from the services that want to be
// seen. No NimBLE and no statics: lichen_bt.c (#73 B2) keeps one bt_reg_t under its lock and does what the calls
// below hand back. Not linked into the image until then.
//
// Owners: the OS, a built-in app or an SD app. Every handle (owner, service, characteristic) is slot + generation, so
// a handle that outlived its owner, or a slot used again, never matches. The registry copies what it is given (the
// service definition) and keeps no pointer into an owner's memory.
//
// Revoke, before an SD app's ELF is freed (and when any app stops): bt_reg_revoke refuses the owner's handles at once
// and lists what the service must undo on the stack (remove services, stop the scan, drop connections); each undo is
// confirmed (bt_reg_service_gone, bt_reg_scan_done, bt_reg_conn_gone) and each queued event dropped
// (bt_reg_event_done). Only when bt_reg_owner_idle is true may bt_reg_owner_release run and the memory be freed.

#define BT_REG_OWNERS       8u
// The OS's own services (Battery, Device Information, the peer service ...) have their budget, the apps theirs: an app
// can never take the slots the OS needs. OS services added before the host starts are its static GATT table
// (bt_reg_static_services), registered once at start and never removed; everything after is dynamic.
#define BT_REG_OS_SERVICES  4u
#define BT_REG_OS_CHRS      16u
#define BT_REG_APP_SERVICES 8u
#define BT_REG_APP_CHRS     32u
#define BT_REG_SERVICES     (BT_REG_OS_SERVICES + BT_REG_APP_SERVICES)
#define BT_REG_CHRS         (BT_REG_OS_CHRS + BT_REG_APP_CHRS)
#define BT_REG_CONNS        3u // the connection cap (ble_policy.h BLE_CONN_MAX)
#define BT_REG_CHANS        4u // L2CAP channels open at once
#define BT_REG_LISTENS      4u // PSMs listened on
#define BT_REG_UNDO_MAX     (BT_REG_SERVICES + BT_REG_CONNS + BT_REG_CHANS + BT_REG_LISTENS + 2u)

// What an owner holds that is not a stack object: its wish to advertise, its wish to be Visible, the peer session
#define BT_CLAIM_ADVERTISE 0x01u
#define BT_CLAIM_VISIBLE   0x02u
#define BT_CLAIM_PEER      0x04u // one owner at a time: one peer session per badge
#define BT_CLAIM_ALL       0x07u

typedef uint32_t bt_owner_t;
typedef enum { BT_OWNER_OS, BT_OWNER_BUILTIN, BT_OWNER_SD } bt_owner_kind_t;

typedef struct {
    bool            used;
    bool            revoked;
    bt_owner_kind_t kind;
    uint32_t        gen;
    bool            scanning;
    uint8_t         claims; // BT_CLAIM_*
    uint16_t        events; // queued to the owner and not yet run or dropped
} bt_reg_owner_t;

typedef struct {
    bool             used;
    bool             removing; // the stack has not confirmed the removal yet
    bool             advertise;
    bool             is_static; // the OS's, added before the host started
    bool             os;        // counts against the OS's budget
    uint32_t         gen;
    bt_owner_t       owner;
    lichen_bt_uuid_t uuid;
    uint8_t          chr_count; // its characteristics are the chrs whose svc is this slot
} bt_reg_svc_t;

typedef struct {
    bool              used;
    uint32_t          gen;
    uint8_t           svc; // slot
    lichen_bt_uuid_t  uuid;
    uint8_t           props;
    lichen_bt_level_t level;
    uint16_t          max_len;
    uint16_t          att_handle; // the value's ATT handle once the stack registered it, 0 before
} bt_reg_chr_t;

typedef struct {
    bool       used;
    bool       closing;
    bt_owner_t owner;
    uint16_t   handle; // the stack's connection handle
} bt_reg_conn_t;

typedef struct {
    bool       used;
    bool       closing;
    bt_owner_t owner;
    uint16_t   cid; // the stack's channel id
} bt_reg_chan_t;

typedef struct {
    bool       used;
    bt_owner_t owner;
    uint16_t   psm;
} bt_reg_listen_t;

typedef struct {
    bt_reg_owner_t  owners[BT_REG_OWNERS];
    bt_reg_svc_t    svcs[BT_REG_SERVICES];
    bt_reg_chr_t    chrs[BT_REG_CHRS];
    bt_reg_conn_t   conns[BT_REG_CONNS];
    bt_reg_chan_t   chans[BT_REG_CHANS];
    bt_reg_listen_t listens[BT_REG_LISTENS];
    uint32_t        gen;     // the next generation handed out (never 0)
    bool            started; // the host runs: services from now on are dynamic
} bt_reg_t;

typedef enum {
    BT_UNDO_REMOVE_SERVICE,
    BT_UNDO_STOP_SCAN,
    BT_UNDO_DISCONNECT,
    BT_UNDO_CLOSE_CHAN, // handle: the channel id
    BT_UNDO_UNLISTEN,   // handle: the PSM (already free in the books; the stack stops accepting on it)
    BT_UNDO_RELEASE,    // handle: the BT_CLAIM_* bits it held (already dropped: re-plan advertising, end the session)
} bt_undo_kind_t;

typedef struct {
    bt_undo_kind_t  kind;
    lichen_bt_svc_t svc;    // REMOVE_SERVICE
    uint16_t        handle; // DISCONNECT: the stack's connection handle
} bt_undo_t;

// What a characteristic is, for the stack's access callback (bt_reg_chr_by_handle) and the service
typedef struct {
    lichen_bt_chr_t   chr;
    lichen_bt_svc_t   svc;
    bt_owner_t        owner;
    lichen_bt_uuid_t  uuid;
    uint8_t           props;
    lichen_bt_level_t level;
    uint16_t          max_len;
    uint16_t          att_handle;
} bt_chr_info_t;

void   bt_reg_init(bt_reg_t *r);
// The host started: the static table is fixed (bt_reg_static_services lists it), later services are dynamic
void   bt_reg_host_started(bt_reg_t *r);
// The static services, in slot order: the number written
size_t bt_reg_static_services(bt_reg_t const *r, lichen_bt_svc_t *out, size_t cap);

// ---- Owners ----
// 0 when the table is full
bt_owner_t bt_reg_owner_add(bt_reg_t *r, bt_owner_kind_t kind);
// The handle names a live owner that was not revoked
bool       bt_reg_owner_valid(bt_reg_t const *r, bt_owner_t owner);
// Refuses the owner's handles from now on and lists what the service must undo (at most BT_REG_UNDO_MAX): the number
// written. Again on a revoked owner: the undos still unconfirmed, so a retry after a lost confirmation works.
size_t     bt_reg_revoke(bt_reg_t *r, bt_owner_t owner, bt_undo_t *undo, size_t cap);
// Nothing of the owner is live any more: no service (removing or not), scan, connection, channel, listened PSM, claim
// or queued event
bool       bt_reg_owner_idle(bt_reg_t const *r, bt_owner_t owner);
// Frees the slot of a revoked, idle owner; false (nothing done) otherwise. Its memory may be freed only after true.
bool       bt_reg_owner_release(bt_reg_t *r, bt_owner_t owner);

// ---- GATT services ----
// Checks and copies def: chrs_out gets chr_count handles. INVALID for a bad UUID, length, property, or a UUID that is
// reserved (GAP 0x1800, GATT 0x1801 and the Lichen peer service, which only the OS may add); EXISTS for a UUID already
// there; FULL when the owner's budget (OS or apps) ran out; NOT_FOUND for an owner that is not valid.
lichen_bt_err_t bt_reg_service_add(
    bt_reg_t *r, bt_owner_t owner, lichen_bt_svc_def_t const *def, lichen_bt_svc_t *svc, lichen_bt_chr_t *chrs_out
);
// The owner removes its service (a static one: INVALID, it stays for the host's life): refused from now on, *undo the
// removal for the stack (then bt_reg_service_gone)
lichen_bt_err_t bt_reg_service_remove(bt_reg_t *r, bt_owner_t owner, lichen_bt_svc_t svc, bt_undo_t *undo);
// The stack removed it: its slots are free
void            bt_reg_service_gone(bt_reg_t *r, lichen_bt_svc_t svc);
// The stack registered chr's value at att_handle (nonzero)
bool            bt_reg_chr_set_handle(bt_reg_t *r, lichen_bt_chr_t chr, uint16_t att_handle);
// The characteristic behind an ATT handle, for the stack's access callback: only a live, unrevoked one
bool            bt_reg_chr_by_handle(bt_reg_t const *r, uint16_t att_handle, bt_chr_info_t *out);
// The owner's own characteristic (another owner's or a stale handle: false)
bool            bt_reg_chr_info(bt_reg_t const *r, bt_owner_t owner, lichen_bt_chr_t chr, bt_chr_info_t *out);

// ---- Scans, connections, events ----
// One scan at a time for the whole badge: BUSY while another owner scans
lichen_bt_err_t bt_reg_scan_claim(bt_reg_t *r, bt_owner_t owner);
// The scan ended (done, stopped, or undone by a revoke)
void            bt_reg_scan_done(bt_reg_t *r, bt_owner_t owner);
// An owner's central connection came up at the stack's handle: FULL past BT_REG_CONNS
lichen_bt_err_t bt_reg_conn_add(bt_reg_t *r, bt_owner_t owner, uint16_t handle);
// The owner of a connection handle (0 when none, or the owner was revoked)
bt_owner_t      bt_reg_conn_owner(bt_reg_t const *r, uint16_t handle);
void            bt_reg_conn_gone(bt_reg_t *r, uint16_t handle);
// An owner listens on psm: INVALID outside LICHEN_BT_PSM_MIN..MAX or on LICHEN_BT_PSM_PEER (the OS's alone), EXISTS
// when someone listens there, FULL past BT_REG_LISTENS
lichen_bt_err_t bt_reg_listen(bt_reg_t *r, bt_owner_t owner, uint16_t psm);
void            bt_reg_unlisten(bt_reg_t *r, bt_owner_t owner, uint16_t psm);
// Who takes a channel a peer opens on psm (0: nobody, refuse it)
bt_owner_t      bt_reg_listener(bt_reg_t const *r, uint16_t psm);
// An owner's channel came up at the stack's channel id: FULL past BT_REG_CHANS
lichen_bt_err_t bt_reg_chan_add(bt_reg_t *r, bt_owner_t owner, uint16_t cid);
bt_owner_t      bt_reg_chan_owner(bt_reg_t const *r, uint16_t cid);
void            bt_reg_chan_gone(bt_reg_t *r, uint16_t cid);
// An owner claims bits (BT_CLAIM_*): BUSY for BT_CLAIM_PEER when another owner holds it, NOT_FOUND for an invalid one
lichen_bt_err_t bt_reg_claim(bt_reg_t *r, bt_owner_t owner, uint8_t bits);
void            bt_reg_unclaim(bt_reg_t *r, bt_owner_t owner, uint8_t bits);
// Some valid owner holds bit (BT_CLAIM_ADVERTISE: advertise; BT_CLAIM_VISIBLE: stay Visible); the holder of a one-owner
// claim (BT_CLAIM_PEER), 0 when none
bool            bt_reg_claimed(bt_reg_t const *r, uint8_t bit);
bt_owner_t      bt_reg_claim_holder(bt_reg_t const *r, uint8_t bit);
// Before queueing an event to owner: false when it may get none (revoked, gone, or 0xffff queued)
bool            bt_reg_event_queued(bt_reg_t *r, bt_owner_t owner);
// The event ran or was dropped
void            bt_reg_event_done(bt_reg_t *r, bt_owner_t owner);

// ---- The advert ----
#define BT_ADV_MAX       31u
#define BT_ADV_FLAGS_GEN 0x06u // LE General Discoverable, BR/EDR not supported

typedef struct {
    uint8_t adv[BT_ADV_MAX];
    uint8_t adv_len;
    uint8_t rsp[BT_ADV_MAX]; // the scan response
    uint8_t rsp_len;
    uint8_t placed;  // UUIDs that went in
    bool    partial; // some did not fit: the caller advances the rotation for the next round
} bt_adv_t;

// The UUIDs of the live services that want to be seen, the OS's first, then by slot: the number written
size_t bt_reg_advert_uuids(bt_reg_t const *r, lichen_bt_uuid_t *out, size_t cap);
// Packs flags (0: none) and name first into the advertising data (the name shortened at a UTF-8 boundary when it does
// not fit), then the UUIDs into what is left of it and into the scan response, starting at uuids[rotation % n] and
// skipping one that does not fit. A UUID list is marked complete only when it holds every UUID of its size.
// False for a bad UUID length (nothing packed).
bool   bt_adv_pack(
      uint8_t flags, char const *name, lichen_bt_uuid_t const *uuids, size_t n, size_t rotation, bt_adv_t *out
  );
