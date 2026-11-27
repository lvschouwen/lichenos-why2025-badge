// Host tests for the Bluetooth rules (#599): pairing window and lockout, access levels, the connection cap, the
// stranger's link and the passkey
#include "ble_policy.h"

#include <stdio.h>

#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void pair_window(void) {
    ble_pair_t p;
    ble_pair_init(&p);
    CHECK(ble_pair_begin(&p, 0, 0) == BLE_PAIR_CLOSED); // never opened: Settings only
    uint32_t t0 = 0xfffff000u;                          // across the counter's wrap
    ble_pair_open(&p, t0);
    CHECK(ble_pair_left_ms(&p, t0) == BLE_PAIR_WINDOW_MS);
    CHECK(ble_pair_is_open(&p, t0 + BLE_PAIR_WINDOW_MS - 1));
    CHECK(ble_pair_left_ms(&p, t0 + BLE_PAIR_WINDOW_MS - 1) == 1);
    CHECK(!ble_pair_is_open(&p, t0 + BLE_PAIR_WINDOW_MS));
    CHECK(ble_pair_left_ms(&p, t0 + BLE_PAIR_WINDOW_MS) == 0);
    CHECK(ble_pair_begin(&p, t0 + BLE_PAIR_WINDOW_MS, 0) == BLE_PAIR_CLOSED);
    // re-opening restarts the 120 s
    ble_pair_open(&p, t0 + 200000);
    CHECK(ble_pair_is_open(&p, t0 + 200000 + BLE_PAIR_WINDOW_MS - 1));
    ble_pair_close(&p);
    CHECK(!ble_pair_is_open(&p, t0 + 200001));
    // a window that ran out is latched closed by the tick: 2^32 ms later it does not read open again
    ble_pair_open(&p, 5000);
    ble_pair_tick(&p, 5000 + BLE_PAIR_WINDOW_MS - 1);
    CHECK(p.open);
    ble_pair_tick(&p, 5000 + BLE_PAIR_WINDOW_MS);
    CHECK(!p.open && !ble_pair_is_open(&p, 5000u + 0u)); // the same counter value after a wrap
}

static void pair_attempts(void) {
    ble_pair_t p;
    ble_pair_init(&p);
    ble_pair_open(&p, 1000);
    CHECK(ble_pair_begin(&p, 1000, 0) == BLE_PAIR_GO);
    CHECK(ble_pair_begin(&p, 1001, 0) == BLE_PAIR_BUSY); // one at a time
    CHECK(ble_pair_end(&p, 1002, false, false));         // failed auth: disconnect
    CHECK(p.fails == 1);
    CHECK(ble_pair_begin(&p, 1003, 0) == BLE_PAIR_GO);
    ble_pair_end(&p, 1004, false, false);
    CHECK(ble_pair_locked_ms(&p, 1004) == 0);
    CHECK(ble_pair_begin(&p, 1005, 0) == BLE_PAIR_GO);
    ble_pair_end(&p, 2000, false, false); // the third: locked out for 60 s
    CHECK(ble_pair_locked_ms(&p, 2000) == BLE_PAIR_LOCKOUT_MS);
    CHECK(ble_pair_begin(&p, 2000 + BLE_PAIR_LOCKOUT_MS - 1, 0) == BLE_PAIR_LOCKED);
    ble_pair_open(&p, 3000); // Settings again: the lockout stays
    CHECK(ble_pair_begin(&p, 3000, 0) == BLE_PAIR_LOCKED);
    CHECK(ble_pair_begin(&p, 2000 + BLE_PAIR_LOCKOUT_MS, 0) == BLE_PAIR_GO); // over, counted from zero again
    CHECK(p.fails == 0);
    ble_pair_end(&p, 2000 + BLE_PAIR_LOCKOUT_MS + 1, false, false);
    CHECK(p.fails == 1 && !p.locked);
    // a stale end without a begin counts nothing
    CHECK(ble_pair_end(&p, 70000, false, false)); // still dropped
    CHECK(ble_pair_end(&p, 70000, true, true)); // a stale success is not trusted either (fails closed)
    CHECK(p.fails == 1);
    // success: failures forgotten and the window closes (one pairing per window)
    CHECK(ble_pair_begin(&p, 70001, 0) == BLE_PAIR_GO);
    CHECK(!ble_pair_end(&p, 70002, true, true));
    CHECK(p.fails == 0 && !ble_pair_is_open(&p, 70003));
}

// A bond without MITM protection (Just Works) is a failed pairing: no bond kept, the link dropped, counted towards the
// lockout, and the window stays open for a proper try
static void pair_unauthenticated(void) {
    ble_pair_t p;
    ble_pair_init(&p);
    ble_pair_open(&p, 0);
    CHECK(ble_pair_begin(&p, 1, 0) == BLE_PAIR_GO);
    CHECK(ble_pair_end(&p, 2, true, false)); // bonded but not authenticated: disconnect, delete the bond
    CHECK(p.fails == 1 && ble_pair_is_open(&p, 3) && !p.busy);
    CHECK(ble_pair_begin(&p, 3, 0) == BLE_PAIR_GO);
    CHECK(!ble_pair_end(&p, 4, true, true));
    CHECK(p.fails == 0 && !ble_pair_is_open(&p, 5));
}

// A pairing that never ends is a failure after the SMP timeout, so BUSY cannot block the window for good
static void pair_stalls(void) {
    ble_pair_t p;
    ble_pair_init(&p);
    ble_pair_open(&p, 0);
    CHECK(ble_pair_begin(&p, 10, 0) == BLE_PAIR_GO);
    CHECK(ble_pair_begin(&p, 10 + BLE_PAIR_SMP_MS - 1, 0) == BLE_PAIR_BUSY);
    ble_pair_tick(&p, 10 + BLE_PAIR_SMP_MS);
    CHECK(!p.busy && p.fails == 1);
    CHECK(ble_pair_end(&p, 10 + BLE_PAIR_SMP_MS + 1, true, true)); // its late end: dropped, not counted again
    CHECK(p.fails == 1 && ble_pair_is_open(&p, 10 + BLE_PAIR_SMP_MS + 1));
    CHECK(ble_pair_begin(&p, 40000, 0) == BLE_PAIR_GO); // begin ticks too
    CHECK(ble_pair_begin(&p, 40000 + BLE_PAIR_SMP_MS, 0) == BLE_PAIR_GO);
    CHECK(p.fails == 2);
}

static void pair_bonds(void) {
    ble_pair_t p;
    ble_pair_init(&p);
    ble_pair_open(&p, 0);
    CHECK(ble_pair_begin(&p, 1, BLE_BONDS_MAX) == BLE_PAIR_BONDS_FULL); // no eviction: Forget first
    CHECK(!p.busy);
    CHECK(ble_pair_begin(&p, 1, BLE_BONDS_MAX - 1) == BLE_PAIR_GO);
}

static void access_levels(void) {
    ble_link_sec_t none = {0}, enc = {.encrypted = true}, auth = {.encrypted = true, .authenticated = true};
    ble_link_sec_t bond        = {.encrypted = true, .authenticated = true, .bonded = true};
    ble_link_sec_t bond_unauth = {.encrypted = true, .bonded = true}; // Just Works bond: not enough
    CHECK(ble_access(LICHEN_BT_OPEN, none) == BLE_ACCESS_OK);
    CHECK(ble_access(LICHEN_BT_ENCRYPTED, none) == BLE_ACCESS_NEED_ENC);
    CHECK(ble_access(LICHEN_BT_ENCRYPTED, enc) == BLE_ACCESS_OK);
    CHECK(ble_access(LICHEN_BT_BONDED, none) == BLE_ACCESS_NEED_AUTH);
    CHECK(ble_access(LICHEN_BT_BONDED, enc) == BLE_ACCESS_NEED_AUTH);
    CHECK(ble_access(LICHEN_BT_BONDED, auth) == BLE_ACCESS_NEED_AUTH);
    CHECK(ble_access(LICHEN_BT_BONDED, bond_unauth) == BLE_ACCESS_NEED_AUTH);
    CHECK(ble_access(LICHEN_BT_BONDED, bond) == BLE_ACCESS_OK);
    CHECK(ble_access((lichen_bt_level_t)7, bond) == BLE_ACCESS_NEED_AUTH);
    CHECK(ble_access_att_error(BLE_ACCESS_OK) == 0);
    CHECK(ble_access_att_error(BLE_ACCESS_NEED_ENC) == 0x0f);
    CHECK(ble_access_att_error(BLE_ACCESS_NEED_AUTH) == 0x05);
}

static void conn_cap(void) {
    ble_conns_t c = {0};
    CHECK(ble_conn_may_advertise(&c));
    CHECK(ble_conn_add(&c, LICHEN_BT_ROLE_PERIPHERAL));
    CHECK(ble_conn_add(&c, LICHEN_BT_ROLE_PERIPHERAL));
    CHECK(!ble_conn_may_advertise(&c)); // 2 peripheral: no more advertising
    CHECK(!ble_conn_add(&c, LICHEN_BT_ROLE_PERIPHERAL));
    CHECK(c.peripheral == 2);
    CHECK(ble_conn_add(&c, LICHEN_BT_ROLE_CENTRAL)); // 3 in all
    CHECK(!ble_conn_add(&c, LICHEN_BT_ROLE_CENTRAL));
    CHECK(c.central == 1);
    ble_conn_remove(&c, LICHEN_BT_ROLE_PERIPHERAL);
    CHECK(ble_conn_may_advertise(&c));
    CHECK(!ble_conn_may(&c, LICHEN_BT_ROLE_CENTRAL)); // one central only
    ble_conn_remove(&c, LICHEN_BT_ROLE_CENTRAL);
    ble_conn_remove(&c, LICHEN_BT_ROLE_CENTRAL); // never below zero
    CHECK(c.central == 0 && c.peripheral == 1);
    CHECK(!ble_conn_may(&c, (lichen_bt_role_t)5));
    // a total past the cap however it came about (a counted link the service forgot): refused
    ble_conns_t odd = {.central = 0, .peripheral = 3};
    CHECK(!ble_conn_may(&odd, LICHEN_BT_ROLE_CENTRAL));
}

static void stranger_link(void) {
    ble_link_t l;
    uint32_t   t0 = 0xffffff00u;
    ble_link_init(&l, LICHEN_BT_ROLE_PERIPHERAL, t0);
    CHECK(!ble_link_should_drop(&l, t0 + BLE_UNBONDED_DROP_MS - 1));
    CHECK(ble_link_should_drop(&l, t0 + BLE_UNBONDED_DROP_MS));
    // encrypted but not bonded is still a stranger
    l.sec.encrypted = true;
    CHECK(ble_link_should_drop(&l, t0 + BLE_UNBONDED_DROP_MS));
    // a pairing under way gets the SMP timeout from its start
    ble_link_pairing(&l, t0 + 5000, true);
    CHECK(!ble_link_should_drop(&l, t0 + 5000 + BLE_PAIR_SMP_MS - 1));
    CHECK(ble_link_should_drop(&l, t0 + 5000 + BLE_PAIR_SMP_MS));
    // it failed: back to the 10 s from the link's start (long gone)
    ble_link_pairing(&l, t0 + 9000, false);
    CHECK(ble_link_should_drop(&l, t0 + 12000));
    l.sec.bonded = true; // a Just Works bond: not authenticated, still a stranger
    CHECK(ble_link_should_drop(&l, t0 + 12000));
    l.sec.authenticated = true;
    CHECK(!ble_link_should_drop(&l, t0 + 1000000));
    // links we made, and the peer session's, are not strangers
    ble_link_init(&l, LICHEN_BT_ROLE_CENTRAL, 0);
    CHECK(!ble_link_should_drop(&l, 1000000));
    ble_link_init(&l, LICHEN_BT_ROLE_PERIPHERAL, 0);
    ble_link_set_peer(&l);
    CHECK(l.kind == BLE_LINK_PEER);
    CHECK(!ble_link_should_drop(&l, 1000000));
}

static void passkey(void) {
    uint32_t pk = 7;
    CHECK(ble_passkey_from_random(0, &pk) && pk == 0);
    CHECK(ble_passkey_from_random(4293999999u, &pk) && pk == 999999);
    pk = 7;
    CHECK(!ble_passkey_from_random(4294000000u, &pk) && pk == 7); // the biased tail: draw again
    CHECK(!ble_passkey_from_random(0xffffffffu, &pk));
    char t[BLE_PASSKEY_TEXT];
    CHECK(ble_passkey_format(123456, t) && strcmp(t, "123 456") == 0);
    CHECK(ble_passkey_format(4096, t) && strcmp(t, "004 096") == 0);
    CHECK(ble_passkey_format(0, t) && strcmp(t, "000 000") == 0);
    CHECK(ble_passkey_format(999999, t) && strcmp(t, "999 999") == 0);
    CHECK(!ble_passkey_format(1000000, t) && t[0] == '\0');
}

int main(void) {
    pair_window();
    pair_attempts();
    pair_unauthenticated();
    pair_stalls();
    pair_bonds();
    access_levels();
    conn_cap();
    stranger_link();
    passkey();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ble_policy_test: all passed\n");
    return 0;
}
