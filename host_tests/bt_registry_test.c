// Host tests for the Bluetooth books (#599): owners and handles, services, revoke before an SD app's memory is freed,
// scans, connections, events and the 31 + 31 B advert
#include "bt_registry.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static lichen_bt_uuid_t u16(uint16_t v) {
    lichen_bt_uuid_t u = {.len = 2};
    u.b[0]             = (uint8_t)v;
    u.b[1]             = (uint8_t)(v >> 8);
    return u;
}

static lichen_bt_uuid_t u128(uint8_t seed) {
    lichen_bt_uuid_t u = {.len = 16};
    for (int i = 0; i < 16; i++) {
        u.b[i] = (uint8_t)(seed + i);
    }
    return u;
}

static lichen_bt_chr_def_t const CHR_RW = {
    .uuid    = {2, {0x19, 0x2a}},
    .props   = LICHEN_BT_PROP_READ | LICHEN_BT_PROP_WRITE,
    .level   = LICHEN_BT_OPEN,
    .max_len = 20
};

static lichen_bt_svc_def_t svc_def(lichen_bt_uuid_t uuid, lichen_bt_chr_def_t const *chrs, uint8_t n) {
    return (lichen_bt_svc_def_t){.uuid = uuid, .chrs = chrs, .chr_count = n, .advertise = true};
}

static void owners(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    CHECK(!bt_reg_owner_valid(&r, 0));
    bt_owner_t o[BT_REG_OWNERS];
    for (unsigned i = 0; i < BT_REG_OWNERS; i++) {
        o[i] = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
        CHECK(o[i] != 0 && bt_reg_owner_valid(&r, o[i]));
    }
    CHECK(bt_reg_owner_add(&r, BT_OWNER_SD) == 0); // full
    CHECK(!bt_reg_owner_release(&r, o[0]));        // not revoked
    bt_undo_t u[BT_REG_UNDO_MAX];
    CHECK(bt_reg_revoke(&r, o[0], u, BT_REG_UNDO_MAX) == 0);
    CHECK(!bt_reg_owner_valid(&r, o[0]));
    CHECK(bt_reg_owner_release(&r, o[0]));
    bt_owner_t again = bt_reg_owner_add(&r, BT_OWNER_SD); // the same slot, a new generation
    CHECK(again != 0 && again != o[0]);
    CHECK(!bt_reg_owner_valid(&r, o[0]) && bt_reg_owner_valid(&r, again));
    CHECK(!bt_reg_owner_valid(&r, 0x0000ff09u)); // slot out of range
}

static void services(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t          os = bt_reg_owner_add(&r, BT_OWNER_OS), app = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    lichen_bt_svc_t     svc;
    lichen_bt_chr_t     chr[LICHEN_BT_SVC_CHRS_MAX];
    lichen_bt_svc_def_t d = svc_def(u16(0x180f), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_OK);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_EXISTS);
    CHECK(bt_reg_service_add(&r, 0, &d, &svc, chr) == LICHEN_BT_NOT_FOUND);
    // reserved: GAP, GATT for anyone, the peer service for apps
    d = svc_def(u16(0x1800), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, os, &d, &svc, chr) == LICHEN_BT_INVALID);
    d = svc_def(u16(0x1801), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, os, &d, &svc, chr) == LICHEN_BT_INVALID);
    d = (lichen_bt_svc_def_t){.uuid = LICHEN_BT_PEER_UUID, .chrs = &CHR_RW, .chr_count = 1};
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    CHECK(bt_reg_service_add(&r, os, &d, &svc, chr) == LICHEN_BT_OK);
    // bad definitions
    lichen_bt_chr_def_t bad = CHR_RW;
    d                       = svc_def(u16(0x2000), &bad, 1);
    bad.max_len             = 0;
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    bad.max_len = LICHEN_BT_VALUE_MAX + 1;
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    bad.max_len = LICHEN_BT_VALUE_MAX;
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_OK); // the boundary itself
    bad       = CHR_RW;
    bad.props = 0x01; // broadcast: not offered
    d         = svc_def(u16(0x2001), &bad, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    bad          = CHR_RW;
    bad.uuid.len = 4;
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    bad       = CHR_RW;
    bad.level = (lichen_bt_level_t)9;
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    d = svc_def(u16(0x2002), &CHR_RW, 0);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    lichen_bt_chr_def_t nine[LICHEN_BT_SVC_CHRS_MAX + 1] = {{.props = 0}};
    for (unsigned i = 0; i <= LICHEN_BT_SVC_CHRS_MAX; i++) {
        nine[i] = CHR_RW;
    }
    d = svc_def(u16(0x2003), nine, LICHEN_BT_SVC_CHRS_MAX + 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_INVALID);
    d = svc_def(u16(0x2003), nine, LICHEN_BT_SVC_CHRS_MAX);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_OK);
    CHECK(bt_reg_service_add(&r, app, NULL, &svc, chr) == LICHEN_BT_INVALID);
}

static void slots_full(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t          app = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    lichen_bt_svc_t     svc;
    lichen_bt_chr_t     chr[LICHEN_BT_SVC_CHRS_MAX];
    lichen_bt_chr_def_t eight[LICHEN_BT_SVC_CHRS_MAX];
    for (unsigned i = 0; i < LICHEN_BT_SVC_CHRS_MAX; i++) {
        eight[i] = CHR_RW;
    }
    // the apps' 32 characteristics: four services of eight use them all
    for (unsigned i = 0; i < BT_REG_APP_CHRS / LICHEN_BT_SVC_CHRS_MAX; i++) {
        lichen_bt_svc_def_t d = svc_def(u16((uint16_t)(0x3000 + i)), eight, LICHEN_BT_SVC_CHRS_MAX);
        CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_OK);
    }
    lichen_bt_svc_def_t d = svc_def(u16(0x3100), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_FULL);
    // the apps' 8 services
    bt_reg_init(&r);
    app = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    for (unsigned i = 0; i < BT_REG_APP_SERVICES; i++) {
        d = svc_def(u16((uint16_t)(0x3200 + i)), &CHR_RW, 1);
        CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_OK);
    }
    d = svc_def(u16(0x3300), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, chr) == LICHEN_BT_FULL);
}

// The OS's budget is its own, and what it adds before the host starts is the static table
static void os_budget_and_static(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t          os = bt_reg_owner_add(&r, BT_OWNER_OS), app = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    lichen_bt_svc_t     svc[BT_REG_SERVICES], s;
    lichen_bt_chr_t     chr[LICHEN_BT_SVC_CHRS_MAX];
    lichen_bt_chr_def_t four[4] = {CHR_RW, CHR_RW, CHR_RW, CHR_RW};
    for (unsigned i = 0; i < BT_REG_OS_SERVICES - 1; i++) { // 3 x 4 = 12 of the OS's 16 characteristics
        lichen_bt_svc_def_t d = svc_def(u16((uint16_t)(0x1810 + i)), four, 4);
        CHECK(bt_reg_service_add(&r, os, &d, &svc[i], chr) == LICHEN_BT_OK);
    }
    bt_reg_host_started(&r);
    lichen_bt_svc_t st[BT_REG_SERVICES];
    CHECK(bt_reg_static_services(&r, st, BT_REG_SERVICES) == 3 && st[0] == svc[0] && st[2] == svc[2]);
    CHECK(bt_reg_static_services(&r, st, 1) == 1);
    bt_undo_t u;
    CHECK(bt_reg_service_remove(&r, os, svc[0], &u) == LICHEN_BT_INVALID); // static: stays
    // after the start: dynamic, and the budget holds: 4 more would be 16, 5 is past it
    lichen_bt_chr_def_t five[5] = {CHR_RW, CHR_RW, CHR_RW, CHR_RW, CHR_RW};
    lichen_bt_svc_def_t d       = svc_def(u16(0x1820), five, 5);
    CHECK(bt_reg_service_add(&r, os, &d, &s, chr) == LICHEN_BT_FULL);
    d = svc_def(u16(0x1820), four, 4);
    CHECK(bt_reg_service_add(&r, os, &d, &s, chr) == LICHEN_BT_OK);
    CHECK(bt_reg_static_services(&r, st, BT_REG_SERVICES) == 3);
    CHECK(bt_reg_service_remove(&r, os, s, &u) == LICHEN_BT_OK); // dynamic: may go
    bt_reg_service_gone(&r, s);
    CHECK(bt_reg_service_add(&r, os, &d, &s, chr) == LICHEN_BT_OK);
    d = svc_def(u16(0x1821), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, os, &d, &s, chr) == LICHEN_BT_FULL); // the OS's 4 services
    // the apps keep all of theirs
    for (unsigned i = 0; i < BT_REG_APP_SERVICES; i++) {
        d = svc_def(u16((uint16_t)(0x3400 + i)), four, 4);
        CHECK(bt_reg_service_add(&r, app, &d, &s, chr) == LICHEN_BT_OK);
    }
    d = svc_def(u16(0x3500), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &s, chr) == LICHEN_BT_FULL);
}

static void handles(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t          a = bt_reg_owner_add(&r, BT_OWNER_BUILTIN), b = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    lichen_bt_chr_def_t two[2] = {CHR_RW, CHR_RW};
    two[1].level               = LICHEN_BT_BONDED;
    two[1].props               = LICHEN_BT_PROP_NOTIFY;
    lichen_bt_svc_def_t d      = svc_def(u16(0x180d), two, 2);
    lichen_bt_svc_t     svc;
    lichen_bt_chr_t     chr[2];
    CHECK(bt_reg_service_add(&r, a, &d, &svc, chr) == LICHEN_BT_OK);
    CHECK(chr[0] && chr[1] && chr[0] != chr[1]);
    bt_chr_info_t info;
    CHECK(!bt_reg_chr_by_handle(&r, 42, &info)); // not registered with the stack yet
    CHECK(!bt_reg_chr_set_handle(&r, chr[0], 0));
    CHECK(bt_reg_chr_set_handle(&r, chr[0], 41) && bt_reg_chr_set_handle(&r, chr[1], 43));
    CHECK(bt_reg_chr_by_handle(&r, 43, &info) && info.chr == chr[1] && info.svc == svc && info.owner == a);
    CHECK(info.level == LICHEN_BT_BONDED && info.props == LICHEN_BT_PROP_NOTIFY && info.max_len == 20);
    CHECK(!bt_reg_chr_by_handle(&r, 0, &info));
    CHECK(bt_reg_chr_info(&r, a, chr[0], &info) && info.att_handle == 41);
    CHECK(!bt_reg_chr_info(&r, b, chr[0], &info)); // another owner's
    // removed by its owner: refused at once, slots free only when the stack confirmed
    bt_undo_t u;
    CHECK(bt_reg_service_remove(&r, b, svc, &u) == LICHEN_BT_NOT_FOUND);
    CHECK(bt_reg_service_remove(&r, a, svc, &u) == LICHEN_BT_OK);
    CHECK(u.kind == BT_UNDO_REMOVE_SERVICE && u.svc == svc);
    CHECK(bt_reg_service_remove(&r, a, svc, &u) == LICHEN_BT_NOT_FOUND); // already going
    CHECK(!bt_reg_chr_by_handle(&r, 43, &info));
    CHECK(bt_reg_service_add(&r, a, &d, &svc, chr) == LICHEN_BT_EXISTS); // the stack may still serve it
    lichen_bt_svc_t old     = svc;
    lichen_bt_chr_t old_chr = chr[0];
    bt_reg_service_gone(&r, old);
    CHECK(bt_reg_service_add(&r, a, &d, &svc, chr) == LICHEN_BT_OK); // same slots, new generations
    CHECK(svc != old && chr[0] != old_chr);
    CHECK(!bt_reg_chr_info(&r, a, old_chr, &info));
    CHECK(!bt_reg_chr_set_handle(&r, old_chr, 41));
    bt_reg_service_gone(&r, old); // stale: nothing happens to the new one
    CHECK(bt_reg_chr_info(&r, a, chr[0], &info));
}

// The SD app's memory: a stand-in for its ELF block. The registry must hold nothing that points into it.
typedef struct {
    lichen_bt_chr_def_t chrs[2];
    lichen_bt_svc_def_t def;
    char                name[16];
} fake_elf_t;

// #599: every service, scan, connection and queued event of an SD app is undone and confirmed before its memory is
// freed; until then release refuses, and after it every path of the registry still works without the memory (ASan
// reports any read of it).
static void revoke_before_free(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t  os  = bt_reg_owner_add(&r, BT_OWNER_OS);
    bt_owner_t  sd  = bt_reg_owner_add(&r, BT_OWNER_SD);
    fake_elf_t *elf = malloc(sizeof(*elf));
    if (!elf) {
        printf("FAIL out of memory\n");
        failures++;
        return;
    }
    elf->chrs[0] = CHR_RW;
    elf->chrs[1] = CHR_RW;
    elf->def     = svc_def(u128(0x40), elf->chrs, 2);
    lichen_bt_svc_t     svc, os_svc;
    lichen_bt_chr_t     chr[2], os_chr;
    lichen_bt_svc_def_t osd = svc_def(u16(0x180f), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, os, &osd, &os_svc, &os_chr) == LICHEN_BT_OK);
    CHECK(bt_reg_service_add(&r, sd, &elf->def, &svc, chr) == LICHEN_BT_OK);
    CHECK(bt_reg_chr_set_handle(&r, chr[0], 20) && bt_reg_chr_set_handle(&r, os_chr, 10));
    CHECK(bt_reg_scan_claim(&r, sd) == LICHEN_BT_OK);
    CHECK(bt_reg_scan_claim(&r, os) == LICHEN_BT_BUSY);
    CHECK(bt_reg_conn_add(&r, sd, 7) == LICHEN_BT_OK);
    CHECK(bt_reg_chan_add(&r, sd, 0x40) == LICHEN_BT_OK);
    CHECK(bt_reg_listen(&r, sd, 0x0081) == LICHEN_BT_OK);
    CHECK(bt_reg_claim(&r, sd, BT_CLAIM_ADVERTISE | BT_CLAIM_PEER) == LICHEN_BT_OK);
    CHECK(bt_reg_claim(&r, os, BT_CLAIM_PEER) == LICHEN_BT_BUSY);
    CHECK(bt_reg_event_queued(&r, sd) && bt_reg_event_queued(&r, sd));

    bt_undo_t u[BT_REG_UNDO_MAX];
    size_t    n = bt_reg_revoke(&r, sd, u, BT_REG_UNDO_MAX);
    CHECK(n == 6);
    CHECK(u[5].kind == BT_UNDO_RELEASE && u[5].handle == (BT_CLAIM_ADVERTISE | BT_CLAIM_PEER));
    CHECK(!bt_reg_claimed(&r, BT_CLAIM_ADVERTISE) && bt_reg_claim_holder(&r, BT_CLAIM_PEER) == 0);
    CHECK(bt_reg_claim(&r, sd, BT_CLAIM_VISIBLE) == LICHEN_BT_NOT_FOUND);
    CHECK(u[0].kind == BT_UNDO_REMOVE_SERVICE && u[0].svc == svc);
    CHECK(u[1].kind == BT_UNDO_STOP_SCAN);
    CHECK(u[2].kind == BT_UNDO_DISCONNECT && u[2].handle == 7);
    CHECK(u[3].kind == BT_UNDO_CLOSE_CHAN && u[3].handle == 0x40);
    CHECK(u[4].kind == BT_UNDO_UNLISTEN && u[4].handle == 0x0081);
    CHECK(bt_reg_listener(&r, 0x0081) == 0 && bt_reg_chan_owner(&r, 0x40) == 0);
    // refused at once, before any undo is confirmed
    bt_chr_info_t info;
    CHECK(!bt_reg_owner_valid(&r, sd));
    CHECK(!bt_reg_chr_by_handle(&r, 20, &info));
    CHECK(bt_reg_conn_owner(&r, 7) == 0);
    CHECK(!bt_reg_event_queued(&r, sd));
    CHECK(bt_reg_service_add(&r, sd, &osd, &svc, chr) == LICHEN_BT_NOT_FOUND);
    CHECK(bt_reg_scan_claim(&r, sd) == LICHEN_BT_NOT_FOUND);
    // a lost confirmation: revoking again lists what is still open
    CHECK(bt_reg_revoke(&r, sd, u, BT_REG_UNDO_MAX) == 4); // the PSM went at once
    // confirm one at a time: release refuses until the last
    CHECK(!bt_reg_owner_idle(&r, sd) && !bt_reg_owner_release(&r, sd));
    bt_reg_service_gone(&r, u[0].svc);
    CHECK(!bt_reg_owner_release(&r, sd));
    bt_reg_scan_done(&r, sd);
    CHECK(!bt_reg_owner_release(&r, sd));
    bt_reg_conn_gone(&r, 7);
    CHECK(!bt_reg_owner_release(&r, sd));
    bt_reg_chan_gone(&r, 0x40);
    CHECK(!bt_reg_owner_release(&r, sd)); // two events still queued
    bt_reg_event_done(&r, sd);
    CHECK(!bt_reg_owner_release(&r, sd));
    bt_reg_event_done(&r, sd);
    CHECK(bt_reg_owner_idle(&r, sd));
    CHECK(bt_reg_owner_release(&r, sd));
    free(elf); // only now

    // every path once more, with the memory gone
    CHECK(bt_reg_chr_by_handle(&r, 10, &info) && info.owner == os);
    CHECK(!bt_reg_chr_by_handle(&r, 20, &info));
    lichen_bt_uuid_t adv[BT_REG_SERVICES];
    CHECK(bt_reg_advert_uuids(&r, adv, BT_REG_SERVICES) == 1);
    CHECK(bt_reg_scan_claim(&r, os) == LICHEN_BT_OK);
    CHECK(bt_reg_conn_owner(&r, 7) == 0);
    CHECK(!bt_reg_owner_release(&r, sd)); // released already
    CHECK(bt_reg_owner_idle(&r, sd));
    bt_reg_event_done(&r, sd); // stale: nothing
    bt_reg_scan_done(&r, sd);
    CHECK(r.owners[1].used == false);
}

static void scans_conns_events(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t a = bt_reg_owner_add(&r, BT_OWNER_BUILTIN), b = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    CHECK(bt_reg_scan_claim(&r, a) == LICHEN_BT_OK);
    CHECK(bt_reg_scan_claim(&r, a) == LICHEN_BT_BUSY);
    CHECK(bt_reg_scan_claim(&r, b) == LICHEN_BT_BUSY);
    bt_reg_scan_done(&r, b); // not b's scan: a still scans
    CHECK(bt_reg_scan_claim(&r, b) == LICHEN_BT_BUSY);
    bt_reg_scan_done(&r, a);
    CHECK(bt_reg_scan_claim(&r, b) == LICHEN_BT_OK);
    CHECK(bt_reg_conn_add(&r, a, 1) == LICHEN_BT_OK);
    CHECK(bt_reg_conn_add(&r, a, 1) == LICHEN_BT_EXISTS);
    CHECK(bt_reg_conn_add(&r, b, 2) == LICHEN_BT_OK);
    CHECK(bt_reg_conn_add(&r, b, 3) == LICHEN_BT_OK);
    CHECK(bt_reg_conn_add(&r, b, 4) == LICHEN_BT_FULL);
    CHECK(bt_reg_conn_add(&r, 0, 5) == LICHEN_BT_NOT_FOUND);
    CHECK(bt_reg_conn_owner(&r, 2) == b && bt_reg_conn_owner(&r, 1) == a && bt_reg_conn_owner(&r, 9) == 0);
    bt_reg_conn_gone(&r, 2);
    CHECK(bt_reg_conn_owner(&r, 2) == 0);
    CHECK(bt_reg_conn_add(&r, a, 4) == LICHEN_BT_OK);
    // the event counter saturates instead of wrapping (idle could never be reached again)
    r.owners[0].events = UINT16_MAX - 1;
    CHECK(bt_reg_event_queued(&r, a));
    CHECK(!bt_reg_event_queued(&r, a));
    CHECK(r.owners[0].events == UINT16_MAX);
    r.owners[0].events = 0;
    bt_reg_event_done(&r, a); // never below zero
    CHECK(r.owners[0].events == 0);
    // revoke with a small undo buffer writes what fits
    bt_undo_t u[1];
    CHECK(bt_reg_revoke(&r, b, u, 1) == 1);
}

static void claims(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t a = bt_reg_owner_add(&r, BT_OWNER_BUILTIN), b = bt_reg_owner_add(&r, BT_OWNER_SD);
    CHECK(!bt_reg_claimed(&r, BT_CLAIM_ADVERTISE));
    CHECK(bt_reg_claim(&r, a, BT_CLAIM_ADVERTISE) == LICHEN_BT_OK);
    CHECK(bt_reg_claim(&r, b, BT_CLAIM_ADVERTISE | BT_CLAIM_VISIBLE) == LICHEN_BT_OK); // shared wishes
    bt_reg_unclaim(&r, a, BT_CLAIM_ADVERTISE);
    CHECK(bt_reg_claimed(&r, BT_CLAIM_ADVERTISE)); // b still wants it
    bt_reg_unclaim(&r, b, BT_CLAIM_ADVERTISE);
    CHECK(!bt_reg_claimed(&r, BT_CLAIM_ADVERTISE) && bt_reg_claimed(&r, BT_CLAIM_VISIBLE));
    CHECK(bt_reg_claim(&r, a, BT_CLAIM_PEER) == LICHEN_BT_OK);
    CHECK(bt_reg_claim(&r, a, BT_CLAIM_PEER) == LICHEN_BT_OK); // its own again
    CHECK(bt_reg_claim(&r, b, BT_CLAIM_PEER) == LICHEN_BT_BUSY);
    CHECK(bt_reg_claim_holder(&r, BT_CLAIM_PEER) == a);
    CHECK(bt_reg_claim(&r, a, 0x80) == LICHEN_BT_INVALID);
    CHECK(bt_reg_claim(&r, 0, BT_CLAIM_ADVERTISE) == LICHEN_BT_NOT_FOUND);
    // a claim keeps the owner from idle until the revoke drops it
    bt_undo_t u[BT_REG_UNDO_MAX];
    CHECK(!bt_reg_owner_idle(&r, a));
    CHECK(bt_reg_revoke(&r, a, u, BT_REG_UNDO_MAX) == 1 && u[0].kind == BT_UNDO_RELEASE);
    CHECK(bt_reg_owner_idle(&r, a) && bt_reg_owner_release(&r, a));
    CHECK(bt_reg_claim(&r, b, BT_CLAIM_PEER) == LICHEN_BT_OK); // free again
}

static void l2cap(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t os = bt_reg_owner_add(&r, BT_OWNER_OS), a = bt_reg_owner_add(&r, BT_OWNER_BUILTIN);
    bt_owner_t b = bt_reg_owner_add(&r, BT_OWNER_SD);
    CHECK(bt_reg_listen(&r, a, LICHEN_BT_PSM_MIN - 1) == LICHEN_BT_INVALID);
    CHECK(bt_reg_listen(&r, a, LICHEN_BT_PSM_MAX + 1) == LICHEN_BT_INVALID);
    CHECK(bt_reg_listen(&r, a, LICHEN_BT_PSM_PEER) == LICHEN_BT_INVALID); // the OS's
    CHECK(bt_reg_listen(&r, os, LICHEN_BT_PSM_PEER) == LICHEN_BT_OK);
    CHECK(bt_reg_listen(&r, a, LICHEN_BT_PSM_MIN) == LICHEN_BT_OK);
    CHECK(bt_reg_listen(&r, b, LICHEN_BT_PSM_MIN) == LICHEN_BT_EXISTS);
    CHECK(bt_reg_listen(&r, b, LICHEN_BT_PSM_MAX) == LICHEN_BT_OK);
    CHECK(bt_reg_listen(&r, b, 0x0090) == LICHEN_BT_OK);
    CHECK(bt_reg_listen(&r, b, 0x0091) == LICHEN_BT_FULL);
    CHECK(bt_reg_listen(&r, 0, 0x0091) == LICHEN_BT_NOT_FOUND);
    CHECK(bt_reg_listener(&r, LICHEN_BT_PSM_MIN) == a && bt_reg_listener(&r, 0x0099) == 0);
    bt_reg_unlisten(&r, b, LICHEN_BT_PSM_MIN); // not b's: stays
    CHECK(bt_reg_listener(&r, LICHEN_BT_PSM_MIN) == a);
    bt_reg_unlisten(&r, a, LICHEN_BT_PSM_MIN);
    CHECK(bt_reg_listener(&r, LICHEN_BT_PSM_MIN) == 0);
    for (uint16_t c = 0x40; c < 0x40 + BT_REG_CHANS; c++) {
        CHECK(bt_reg_chan_add(&r, a, c) == LICHEN_BT_OK);
    }
    CHECK(bt_reg_chan_add(&r, a, 0x40) == LICHEN_BT_EXISTS);
    CHECK(bt_reg_chan_add(&r, b, 0x50) == LICHEN_BT_FULL);
    CHECK(bt_reg_chan_add(&r, 0, 0x50) == LICHEN_BT_NOT_FOUND);
    CHECK(bt_reg_chan_owner(&r, 0x41) == a && bt_reg_chan_owner(&r, 0x50) == 0);
    bt_reg_chan_gone(&r, 0x41);
    CHECK(bt_reg_chan_owner(&r, 0x41) == 0);
    CHECK(bt_reg_chan_add(&r, b, 0x50) == LICHEN_BT_OK);
}

static void advert_order(void) {
    bt_reg_t r;
    bt_reg_init(&r);
    bt_owner_t          app = bt_reg_owner_add(&r, BT_OWNER_BUILTIN), os = bt_reg_owner_add(&r, BT_OWNER_OS);
    lichen_bt_svc_t     svc;
    lichen_bt_chr_t     chr;
    lichen_bt_svc_def_t d = svc_def(u16(0x1111), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, app, &d, &svc, &chr) == LICHEN_BT_OK);
    d = svc_def(u16(0x2222), &CHR_RW, 1);
    CHECK(bt_reg_service_add(&r, os, &d, &svc, &chr) == LICHEN_BT_OK);
    d           = svc_def(u16(0x3333), &CHR_RW, 1);
    d.advertise = false;
    CHECK(bt_reg_service_add(&r, os, &d, &svc, &chr) == LICHEN_BT_OK);
    lichen_bt_uuid_t out[4];
    CHECK(bt_reg_advert_uuids(&r, out, 4) == 2);
    CHECK(out[0].b[0] == 0x22 && out[1].b[0] == 0x11); // the OS's first
    CHECK(bt_reg_advert_uuids(&r, out, 1) == 1 && out[0].b[0] == 0x22);
}

// Walks AD structures: each must fit, and the total must be the length. The count of structures, -1 when broken.
static int ad_walk(uint8_t const *b, size_t len) {
    size_t i = 0;
    int    n = 0;
    while (i < len) {
        if (b[i] == 0 || i + 1 + b[i] > len) {
            return -1;
        }
        i += 1u + b[i];
        n++;
    }
    return n;
}

// The AD structure of type in b, or NULL; *dlen its data length
static uint8_t const *ad_find(uint8_t const *b, size_t len, uint8_t type, size_t *dlen) {
    for (size_t i = 0; i < len; i += 1u + b[i]) {
        if (b[i + 1] == type) {
            *dlen = b[i] - 1u;
            return b + i + 2;
        }
    }
    return NULL;
}

static void adv_name(void) {
    bt_adv_t a;
    size_t   dl;
    // flags + a 26-byte name fill exactly 31
    char     n26[] = "abcdefghijklmnopqrstuvwxyz";
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, n26, NULL, 0, 0, &a));
    CHECK(a.adv_len == 31 && a.rsp_len == 0 && ad_walk(a.adv, a.adv_len) == 2);
    CHECK(a.adv[0] == 2 && a.adv[1] == 0x01 && a.adv[2] == BT_ADV_FLAGS_GEN);
    uint8_t const *p = ad_find(a.adv, a.adv_len, 0x09, &dl);
    CHECK(p && dl == 26 && memcmp(p, n26, 26) == 0);
    CHECK(!a.partial && a.placed == 0);
    // 27: shortened to 26
    char n27[] = "abcdefghijklmnopqrstuvwxyz0";
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, n27, NULL, 0, 0, &a));
    CHECK(a.adv_len == 31 && ad_find(a.adv, a.adv_len, 0x09, &dl) == NULL);
    p = ad_find(a.adv, a.adv_len, 0x08, &dl);
    CHECK(p && dl == 26);
    // a cut never splits a UTF-8 character: 25 ASCII + "é" (2 bytes) would need 27
    char utf[] = "abcdefghijklmnopqrstuvwxy\xc3\xa9";
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, utf, NULL, 0, 0, &a));
    p = ad_find(a.adv, a.adv_len, 0x08, &dl);
    CHECK(p && dl == 25 && a.adv_len == 30);
    // no flags, no name
    CHECK(bt_adv_pack(0, NULL, NULL, 0, 0, &a) && a.adv_len == 0);
    CHECK(bt_adv_pack(0, "", NULL, 0, 0, &a) && a.adv_len == 0);
    // without flags a 29-byte name fits
    char n29[] = "abcdefghijklmnopqrstuvwxyz012";
    CHECK(bt_adv_pack(0, n29, NULL, 0, 0, &a) && a.adv_len == 31 && ad_find(a.adv, 31, 0x09, &dl) && dl == 29);
}

static void adv_uuids(void) {
    bt_adv_t         a;
    size_t           dl;
    // "Lichen" (3 + 8 = 11 B): 20 left, one 128-bit (18) fits, the next goes to the scan response
    lichen_bt_uuid_t two[2] = {u128(0x10), u128(0x50)};
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", two, 2, 0, &a));
    CHECK(a.adv_len == 29 && a.rsp_len == 18 && a.placed == 2 && !a.partial);
    uint8_t const *p = ad_find(a.adv, a.adv_len, 0x06, &dl); // incomplete: the other one is in the response
    CHECK(p && dl == 16 && p[0] == 0x10);
    p = ad_find(a.rsp, a.rsp_len, 0x06, &dl);
    CHECK(p && p[0] == 0x50);
    // three 128-bit: one is left out; the rotation brings each one in over the rounds
    lichen_bt_uuid_t three[3] = {u128(0x10), u128(0x50), u128(0x90)};
    int              seen[3]  = {0};
    for (size_t rot = 0; rot < 3; rot++) {
        CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", three, 3, rot, &a));
        CHECK(a.placed == 2 && a.partial);
        CHECK(ad_walk(a.adv, a.adv_len) == 3 && ad_walk(a.rsp, a.rsp_len) == 1);
        p = ad_find(a.adv, a.adv_len, 0x06, &dl);
        seen[(p[0] - 0x10) / 0x40]++;
        p = ad_find(a.rsp, a.rsp_len, 0x06, &dl);
        seen[(p[0] - 0x10) / 0x40]++;
    }
    CHECK(seen[0] == 2 && seen[1] == 2 && seen[2] == 2);
    // 16-bit ones share one list: 20 left in the advert = header 2 + 9 UUIDs (18) = exactly 31
    lichen_bt_uuid_t many[24];
    for (int i = 0; i < 24; i++) {
        many[i] = u16((uint16_t)(0x2a00 + i));
    }
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", many, 9, 0, &a));
    CHECK(a.adv_len == 31 && a.rsp_len == 0 && a.placed == 9);
    p = ad_find(a.adv, a.adv_len, 0x03, &dl); // complete: all nine
    CHECK(p && dl == 18 && p[0] == 0x00 && p[1] == 0x2a && p[16] == 0x08);
    // the tenth goes to the response; both lists are now incomplete
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", many, 10, 0, &a));
    CHECK(a.adv_len == 31 && a.rsp_len == 4 && a.placed == 10 && !a.partial);
    CHECK(ad_find(a.adv, a.adv_len, 0x02, &dl) && ad_find(a.rsp, a.rsp_len, 0x02, &dl));
    // 9 + 14 fit (the response holds 2 + 28); the 24th does not
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", many, 23, 0, &a) && a.placed == 23 && a.rsp_len == 30);
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", many, 24, 0, &a) && a.placed == 23 && a.partial);
    CHECK(a.adv_len <= BT_ADV_MAX && a.rsp_len <= BT_ADV_MAX);
    // a 128-bit one that does not fit is skipped, a 16-bit one after it still goes in
    lichen_bt_uuid_t mix[2] = {u128(0x10), u16(0x180f)};
    char             n20[]  = "abcdefghijklmnopqrst"; // 3 + 22 = 25: 6 left
    CHECK(bt_adv_pack(BT_ADV_FLAGS_GEN, n20, mix, 2, 0, &a));
    CHECK(ad_find(a.adv, a.adv_len, 0x03, &dl) && a.adv_len == 29);
    CHECK(ad_find(a.rsp, a.rsp_len, 0x07, &dl) && a.rsp_len == 18); // complete: the only 128-bit one
    // a bad UUID length packs nothing
    lichen_bt_uuid_t bad = {.len = 4};
    CHECK(!bt_adv_pack(BT_ADV_FLAGS_GEN, "Lichen", &bad, 1, 0, &a) && a.adv_len == 0);
}

int main(void) {
    owners();
    services();
    slots_full();
    os_budget_and_static();
    handles();
    revoke_before_free();
    scans_conns_events();
    claims();
    l2cap();
    advert_order();
    adv_name();
    adv_uuids();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("bt_registry_test: all passed\n");
    return 0;
}
