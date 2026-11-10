// Host tests for the P4's settings registry service (#310): the real P4 table (reg_table_check), the console set rules
// on it (a radio key is ERR guarded and reaches no setter, so no CONFIG can go out), unknown keys, the proxy cache
// (seq match, late answers, unknown types, age, stale after a C6 reset), the old-C6 path (no c6.reg_caps answer ->
// "not supported by this C6"), the applied-vs-requested match rules and the once-per-key mismatch.
#include "reg_cmd.h"
#include "reg_p4.h"
#include "reg_proxy.h"

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

// Stubs for lichen_reg.c's getters and setter: what each was asked
static int      bl_sets;        // reg_p4_bl_set calls: the only path from a registry set to the C6
static int      proxy_gets;
static uint32_t proxy_value = 869618000;
static bool     proxy_fails;

reg_status_t reg_p4_proxy_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    proxy_gets++;
    if (proxy_fails) {
        return REG_FAILED;
    }
    out->u = proxy_value;
    return REG_OK;
}

reg_status_t reg_p4_bl_set(reg_entry_t const *e, reg_value_t const *v) {
    (void)e;
    proxy_value = v->u;
    bl_sets++;
    return REG_OK;
}

reg_status_t reg_p4_bl_mode_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 1;
    return REG_OK;
}

// wifi.* (#311): the stub driver reads back NL / 13 dBm; wifi.ps sets are counted
static int      wifi_ps_sets;
static uint32_t wifi_ps_value;

reg_status_t reg_p4_wifi_get(reg_entry_t const *e, reg_value_t *out) {
    switch (e->id) {
        case REG_ID_WIFI_COUNTRY: memcpy(out->s, "NL", 3); break;
        case REG_ID_WIFI_TXPWR: out->i = 52; break;
        case REG_ID_WIFI_BW: out->u = 20; break;
        case REG_ID_WIFI_PS: out->u = wifi_ps_value; break;
        default: out->u = 0x47; break;
    }
    return REG_OK;
}

reg_status_t reg_p4_wifi_ps_set(reg_entry_t const *e, reg_value_t const *v) {
    (void)e;
    wifi_ps_value = v->u;
    wifi_ps_sets++;
    return REG_OK;
}

static bool c6_confirms = true; // the stub C6 has the registry and the bl domain

static bool confirmable(reg_entry_t const *e) {
    (void)e;
    return c6_confirms;
}

static reg_cmd_result_t set(char const *args, char *reply, size_t cap) {
    reg_entry_t const *e;
    reg_value_t        applied;
    bool               have;
    reg_cmd_result_t   r = reg_cmd_set(reg_p4_table, reg_p4_count, args, confirmable, &e, &applied, &have);
    reg_cmd_set_reply(r, e, &applied, have, args, reply, cap);
    return r;
}

static void test_table(void) {
    CHECK(reg_table_check(reg_p4_table, reg_p4_count) == reg_p4_count);
    size_t proxies = 0;
    for (size_t i = 0; i < reg_p4_count; i++) {
        reg_entry_t const *e = &reg_p4_table[i];
        proxies += reg_p4_is_proxy(e);
        if (REG_ID_DOMAIN(e->id) == REG_ID_DOMAIN(REG_ID_LORA_FREQ)) {
            CHECK((e->flags & REG_F_RO) && !e->set); // radio keys: read-only in the registry (#307)
        }
        // A C6 key's proxy, a P4 key not: the domain says which chip serves it (0x10+ and wifi.* 0x03 are the P4's)
        unsigned d = REG_ID_DOMAIN(e->id);
        CHECK(reg_p4_is_proxy(e) == (d < 0x10 && d != REG_ID_DOMAIN(REG_ID_WIFI_COUNTRY)));
    }
    CHECK(proxies <= REG_PROXY_MAX);
    CHECK(reg_find_id(reg_p4_table, reg_p4_count, REG_ID_C6_REG_CAPS) != NULL);
    for (size_t i = 0; i < REG_RADIO_KEYS; i++) {
        CHECK(reg_find_id(reg_p4_table, reg_p4_count, reg_p4_radio_ids[i]) != NULL);
    }
}

static void test_set_guarded(void) {
    char reply[160];
    bl_sets = proxy_gets = 0;
    CHECK(set("lora.freq 869000000", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(strncmp(reply, "ERR guarded", 11) == 0);
    CHECK(strstr(reply, "applied 869618000 Hz") != NULL);
    CHECK(set("lora.power 10", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(set("lora.sf 9", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(bl_sets == 0); // no setter ran: nothing could send a CONFIG
    // Guarded even when the C6 cannot answer (reg_set alone would call that FAILED)
    proxy_fails = true;
    CHECK(set("lora.bw 125000", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(strstr(reply, "applied ?") != NULL);
    proxy_fails = false;
    CHECK(bl_sets == 0);
}

static void test_set_rules(void) {
    char reply[160];
    CHECK(set("no.such 1", reply, sizeof(reply)) == RC_UNKNOWN);
    CHECK(strcmp(reply, "ERR unknown key: no.such 1") == 0);
    CHECK(set("bl.display", reply, sizeof(reply)) == RC_USAGE);
    CHECK(set("bl.display abc", reply, sizeof(reply)) == RC_BAD_VALUE);
    CHECK(set("bl.display 2", reply, sizeof(reply)) == RC_RANGE); // below BACKLIGHT_DISPLAY_MIN
    CHECK(set("bl.keyboard 101", reply, sizeof(reply)) == RC_RANGE);
    CHECK(set("bl.idle dim", reply, sizeof(reply)) == RC_RO);
    CHECK(set("c6.reg_caps 1", reply, sizeof(reply)) == RC_RO);
    bl_sets = 0;
    CHECK(set("bl.display 50", reply, sizeof(reply)) == RC_OK);
    CHECK(strcmp(reply, "bl.display = 50 %") == 0);
    CHECK(bl_sets == 1);
    proxy_fails = true; // set, but the C6 does not answer: said so, not called applied
    CHECK(set("bl.keyboard 20", reply, sizeof(reply)) == RC_FAILED);
    CHECK(strncmp(reply, "ERR failed", 10) == 0);
    // An old C6 (or one without the bl domain): the level went over UART1, said so, not an error (#310 review M4)
    c6_confirms = false;
    bl_sets     = 0;
    CHECK(set("bl.keyboard 20", reply, sizeof(reply)) == RC_UNCONFIRMED);
    CHECK(strcmp(reply, "bl.keyboard = 20 % (sent over UART, not confirmed by this C6)") == 0);
    CHECK(bl_sets == 1);
    CHECK(set("lora.freq 869000000", reply, sizeof(reply)) == RC_GUARDED); // still guarded, never sent
    CHECK(bl_sets == 1);
    c6_confirms = true;
    proxy_fails = false;
}

// #311: the country, TX power and bandwidth are guarded (their own commands), the protocol read-only, power save set
static void test_wifi_keys(void) {
    char reply[160];
    CHECK(reg_find_name(reg_p4_table, reg_p4_count, "bt.state") != NULL);
    CHECK(reg_find_id(reg_p4_table, reg_p4_count, REG_ID_BT_STATE)->type == REG_T_ENUM);
    wifi_ps_sets = 0;
    CHECK(set("wifi.country DE", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(strstr(reply, "applied NL") != NULL);
    CHECK(set("wifi.txpwr 80", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(strstr(reply, "applied 52 qdBm") != NULL);
    CHECK(set("wifi.bw 40", reply, sizeof(reply)) == RC_GUARDED);
    CHECK(set("wifi.protocol 7", reply, sizeof(reply)) == RC_RO);
    CHECK(set("wifi.ps max", reply, sizeof(reply)) == RC_BAD_VALUE); // only none | min, as fw wifi ps
    CHECK(set("wifi.ps 2", reply, sizeof(reply)) == RC_RANGE);
    CHECK(wifi_ps_sets == 0);
    CHECK(set("wifi.ps min", reply, sizeof(reply)) == RC_OK);
    CHECK(strcmp(reply, "wifi.ps = min") == 0);
    CHECK(wifi_ps_sets == 1 && wifi_ps_value == 1);
    CHECK(set("wifi.ps none", reply, sizeof(reply)) == RC_OK);
    CHECK(strcmp(reply, "wifi.ps = none") == 0);
}

// fw airplane's bt.state part (#311): the cache with its age, or why there is none
static void test_cached_text(void) {
    reg_entry_t const *bt = reg_find_id(reg_p4_table, reg_p4_count, REG_ID_BT_STATE);
    char               line[64];
    reg_view_t         v = {.has_applied = true, .has_age = true, .applied = {.u = 0}, .age_ms = 12345};
    reg_cached_text(bt, &v, line, sizeof(line));
    CHECK(strcmp(line, "idle (12.3 s ago)") == 0);
    v.note = RV_STALE;
    reg_cached_text(bt, &v, line, sizeof(line));
    CHECK(strcmp(line, "idle (stale, 12.3 s ago)") == 0);
    v = (reg_view_t){.note = RV_OLD_C6};
    reg_cached_text(bt, &v, line, sizeof(line));
    CHECK(strcmp(line, "? (not supported by this C6)") == 0);
    v = (reg_view_t){0};
    reg_cached_text(bt, &v, line, sizeof(line));
    CHECK(strcmp(line, "? (not read yet)") == 0);
}

static uint16_t const ids[]   = {REG_ID_BL_DISPLAY, REG_ID_LORA_FREQ, REG_ID_LORA_POWER, REG_ID_C6_REG_CAPS};
static uint8_t const  types[] = {REG_T_U8, REG_T_U32, REG_T_I16, REG_T_U32};

static void caps_answer(reg_proxy_t *p, uint32_t caps, uint32_t t) {
    uint8_t seq;
    CHECK(reg_proxy_begin(p, REG_ID_C6_REG_CAPS, t, &seq));
    CHECK(reg_proxy_answer(p, REG_ID_C6_REG_CAPS, seq, REG_OK, REG_T_U32, caps, t) == RP_ACCEPTED);
}

static void test_old_c6(void) {
    reg_proxy_t p;
    reg_view_t  v;
    uint8_t     seq;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    CHECK(reg_proxy_support(&p, REG_ID_BL_DISPLAY, 0) == RP_WAIT_CAPS);
    CHECK(reg_proxy_caps_due(&p, 0));
    CHECK(reg_proxy_begin(&p, REG_ID_C6_REG_CAPS, 100, &seq));
    CHECK(!reg_proxy_caps_due(&p, 200)); // being asked
    CHECK(reg_proxy_support(&p, REG_ID_BL_DISPLAY, 100 + REG_PROXY_CAPS_TIMEOUT_MS - 1) == RP_WAIT_CAPS);
    // No REG_VAL at all: an old C6 ignores REG_GET (BAD_TYPE) -> not supported, the UART fallback stays
    uint32_t late = 100 + REG_PROXY_CAPS_TIMEOUT_MS;
    CHECK(reg_proxy_support(&p, REG_ID_BL_DISPLAY, late) == RP_OLD_C6);
    reg_proxy_view(&p, REG_ID_BL_DISPLAY, late, true, false, &v);
    CHECK(!v.has_applied && v.note == RV_OLD_C6);
    CHECK(strcmp(reg_note_text(RV_OLD_C6), "not supported by this C6") == 0);
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, late, true, false, &v);
    CHECK(v.note == RV_OLD_C6);
    // Re-asked in the background a bounded number of times only (each one counts in the old C6's STATUS.ignored)
    CHECK(reg_proxy_caps_due(&p, late));
    CHECK(reg_proxy_begin(&p, REG_ID_C6_REG_CAPS, late, &seq));
    CHECK(reg_proxy_begin(&p, REG_ID_C6_REG_CAPS, late + 5000, &seq));
    CHECK(!reg_proxy_caps_due(&p, late + 10000));
}

// A post that could not go out (#310 review L5): no c6.reg_caps try is used up, and a request still in flight keeps its
// seq, so its answer is taken
static void test_cancel(void) {
    reg_proxy_t p;
    uint8_t     s1, s2;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    for (int i = 0; i < 2 * REG_PROXY_CAPS_TRIES; i++) {
        CHECK(reg_proxy_caps_due(&p, 0));
        CHECK(reg_proxy_begin(&p, REG_ID_C6_REG_CAPS, 0, &s1));
        reg_proxy_cancel(&p, REG_ID_C6_REG_CAPS, s1); // no link: nothing sent
    }
    CHECK(p.caps_tries == 0 && reg_proxy_caps_due(&p, 0));
    caps_answer(&p, 0x8016, 0);
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 100, &s1)); // in flight
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 200, &s2)); // the console's, could not be posted
    reg_proxy_cancel(&p, REG_ID_LORA_FREQ, s2);
    CHECK(reg_proxy_pending(&p, REG_ID_LORA_FREQ));
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s1, REG_OK, REG_T_U32, 869618000, 300) == RP_ACCEPTED);
    // Cancelling with nothing in flight before leaves the key not waiting
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_POWER, 400, &s1));
    reg_proxy_cancel(&p, REG_ID_LORA_POWER, s1);
    CHECK(!reg_proxy_pending(&p, REG_ID_LORA_POWER));
    // A stale seq does not cancel the newest request
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_POWER, 500, &s2));
    reg_proxy_cancel(&p, REG_ID_LORA_POWER, s1);
    CHECK(reg_proxy_pending(&p, REG_ID_LORA_POWER));
}

static void test_caps_and_domains(void) {
    reg_proxy_t p;
    reg_view_t  v;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    caps_answer(&p, REG_CAP_LORA | REG_CAP_C6, 10); // a C6 without the bl domain
    CHECK(reg_proxy_caps_known(&p) && !reg_proxy_caps_due(&p, 20));
    CHECK(reg_proxy_support(&p, REG_ID_LORA_FREQ, 20) == RP_OK);
    CHECK(reg_proxy_support(&p, REG_ID_BL_DISPLAY, 20) == RP_NO_DOMAIN);
    reg_proxy_view(&p, REG_ID_BL_DISPLAY, 20, true, false, &v);
    CHECK(v.note == RV_NO_DOMAIN);
}

static void test_seq_and_cache(void) {
    reg_proxy_t p;
    reg_view_t  v;
    uint8_t     s1, s2;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    caps_answer(&p, 0x8016, 0);
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 100, &s1));
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 150, &s2));
    CHECK(s1 != s2);
    // The answer to the older request is late: dropped, the key still waits for the newer one
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s1, REG_OK, REG_T_U32, 1, 160) == RP_LATE);
    CHECK(reg_proxy_pending(&p, REG_ID_LORA_FREQ));
    // A type this build does not know, or not the table's: dropped
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s2, REG_OK, 99, 1, 170) == RP_BAD_TYPE);
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s2, REG_OK, REG_T_U8, 1, 170) == RP_BAD_TYPE);
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s2, REG_OK, REG_T_U32, 869618000, 200) == RP_ACCEPTED);
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s2, REG_OK, REG_T_U32, 2, 210) == RP_LATE); // a duplicate
    CHECK(reg_proxy_answer(&p, 0x7777, 1, REG_OK, REG_T_U32, 2, 210) == RP_NOT_OURS);
    reg_proxy_view(&p, REG_ID_LORA_FREQ, 1700, false, false, &v);
    CHECK(v.has_applied && v.applied.u == 869618000 && v.has_age && v.age_ms == 1500 && v.note == RV_NONE);
    // A live ask that got no answer shows the last value as stale, not a hang
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 2000, &s1));
    reg_proxy_view(&p, REG_ID_LORA_FREQ, 2500, true, false, &v);
    CHECK(v.has_applied && v.note == RV_STALE);
    reg_proxy_view(&p, REG_ID_LORA_FREQ, 2500, false, false, &v); // a screen's cache read: no live ask
    CHECK(v.has_applied && v.note == RV_NONE);
    // Signed keys travel sign-extended
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_POWER, 3000, &s1));
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_POWER, s1, REG_OK, REG_T_I16, (uint32_t)-9, 3000) == RP_ACCEPTED);
    reg_proxy_view(&p, REG_ID_LORA_POWER, 3000, false, false, &v);
    CHECK(v.has_applied && v.applied.i == -9);
    // A radio key the C6 has not applied yet (no radio start since its reset): not an error
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_POWER, 4000, &s1));
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_POWER, s1, REG_FAILED, REG_T_I16, 0, 4000) == RP_ACCEPTED);
    reg_proxy_view(&p, REG_ID_LORA_POWER, 4000, true, false, &v);
    CHECK(!v.has_applied && v.note == RV_NOT_APPLIED);
    // An id the C6 does not know
    CHECK(reg_proxy_begin(&p, REG_ID_BL_DISPLAY, 4000, &s1));
    CHECK(reg_proxy_answer(&p, REG_ID_BL_DISPLAY, s1, REG_UNKNOWN, 0, 0, 4000) == RP_ACCEPTED);
    reg_proxy_view(&p, REG_ID_BL_DISPLAY, 4000, true, false, &v);
    CHECK(v.note == RV_UNKNOWN_ID);
}

static void test_link(void) {
    reg_proxy_t p;
    reg_view_t  v;
    uint8_t     s;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    caps_answer(&p, 0x8016, 0);
    CHECK(reg_proxy_begin(&p, REG_ID_BL_DISPLAY, 10, &s));
    CHECK(reg_proxy_answer(&p, REG_ID_BL_DISPLAY, s, REG_OK, REG_T_U8, 30, 10) == RP_ACCEPTED);
    CHECK(reg_proxy_begin(&p, REG_ID_LORA_FREQ, 20, &s)); // in flight when the C6 resets
    reg_proxy_link(&p, 1000);
    // Caps asked anew, the cached value stays visible as stale, nothing waits for an answer that will not come
    CHECK(!reg_proxy_caps_known(&p) && reg_proxy_caps_due(&p, 1000));
    CHECK(reg_proxy_support(&p, REG_ID_BL_DISPLAY, 1000) == RP_WAIT_CAPS);
    CHECK(!reg_proxy_pending(&p, REG_ID_LORA_FREQ));
    CHECK(reg_proxy_answer(&p, REG_ID_LORA_FREQ, s, REG_OK, REG_T_U32, 1, 1100) == RP_LATE);
    reg_proxy_view(&p, REG_ID_BL_DISPLAY, 1100, false, false, &v);
    CHECK(v.has_applied && v.applied.u == 30 && v.note == RV_STALE);
    reg_proxy_view(&p, REG_ID_LORA_FREQ, 1100, false, true, &v);
    CHECK(!v.has_applied && v.note == RV_NO_LINK);
}

// c6.reg_caps is asked once per link (#314): in a live console ask, which does not ask it again, it shows its age and
// no stale note however old it is; after a re-attach it shows stale until answered again
static void test_caps_age(void) {
    reg_proxy_t p;
    reg_view_t  v;
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    caps_answer(&p, 0x8016, 1000);
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, 76000, true, true, &v);
    CHECK(v.has_applied && v.applied.u == 0x8016 && v.has_age && v.age_ms == 75000 && v.note == RV_NONE);
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, 76000, false, false, &v);
    CHECK(v.has_applied && v.note == RV_NONE);
    // A C6 that went away (maybe reflashed): the old caps show stale until answered again, and count as unknown
    reg_proxy_link(&p, 80000);
    CHECK(!reg_proxy_caps_known(&p) && reg_proxy_caps_due(&p, 80000));
    CHECK(reg_proxy_support(&p, REG_ID_LORA_FREQ, 80000) == RP_WAIT_CAPS);
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, 80000, true, true, &v);
    CHECK(v.has_applied && v.applied.u == 0x8016 && v.note == RV_STALE);
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, 80000, false, false, &v);
    CHECK(v.has_applied && v.note == RV_STALE);
    caps_answer(&p, 0x0016, 81000); // asked anew after the re-attach
    reg_proxy_view(&p, REG_ID_C6_REG_CAPS, 81000, true, true, &v);
    CHECK(v.has_applied && v.applied.u == 0x0016 && v.note == RV_NONE && reg_proxy_caps_known(&p));
}

static void test_matches(void) {
    reg_value_t r = {.u = 869618000}, a = {.u = 869617990};
    CHECK(reg_applied_matches(REG_ID_LORA_FREQ, &r, &r) == RM_EQUAL);
    CHECK(reg_applied_matches(REG_ID_LORA_FREQ, &r, &a) == RM_TOLERATED);
    a.u = 869618000 + REG_FREQ_TOLERANCE_HZ + 1;
    CHECK(reg_applied_matches(REG_ID_LORA_FREQ, &r, &a) == RM_DIFFERS);
    r.u = 62500, a.u = 62500;
    CHECK(reg_applied_matches(REG_ID_LORA_BW, &r, &a) == RM_EQUAL);
    r.u = 7800, a.u = 7812;
    CHECK(reg_applied_matches(REG_ID_LORA_BW, &r, &a) == RM_TOLERATED);
    r.u = 41700, a.u = 41667;
    CHECK(reg_applied_matches(REG_ID_LORA_BW, &r, &a) == RM_TOLERATED);
    r.u = 10400, a.u = 10417;
    CHECK(reg_applied_matches(REG_ID_LORA_BW, &r, &a) == RM_TOLERATED);
    r.u = 125000, a.u = 250000;
    CHECK(reg_applied_matches(REG_ID_LORA_BW, &r, &a) == RM_DIFFERS);
    r.u = 5, a.u = 10; // the C6's 10..80 % clamp
    CHECK(reg_applied_matches(REG_ID_BL_DISPLAY, &r, &a) == RM_TOLERATED);
    r.u = 95, a.u = 80;
    CHECK(reg_applied_matches(REG_ID_BL_KEYBOARD, &r, &a) == RM_TOLERATED);
    r.u = 50, a.u = 40;
    CHECK(reg_applied_matches(REG_ID_BL_DISPLAY, &r, &a) == RM_DIFFERS);
    r.u = 0, a.u = 10; // off is off
    CHECK(reg_applied_matches(REG_ID_BL_DISPLAY, &r, &a) == RM_DIFFERS);
    r.u = 7, a.u = 8;
    CHECK(reg_applied_matches(REG_ID_LORA_SF, &r, &a) == RM_DIFFERS);
}

static void answer(reg_proxy_t *p, uint16_t id, uint8_t type, uint32_t value, uint32_t t) {
    uint8_t s;
    CHECK(reg_proxy_begin(p, id, t, &s));
    CHECK(reg_proxy_answer(p, id, s, REG_OK, type, value, t) == RP_ACCEPTED);
}

static void test_mismatch(void) {
    reg_proxy_t       p;
    reg_value_t const req = {.u = 50};
    CHECK(reg_proxy_init(&p, ids, types, 4, 0));
    caps_answer(&p, 0x8016, 0);
    answer(&p, REG_ID_BL_DISPLAY, REG_T_U8, 40, 100);
    CHECK(!reg_proxy_mismatch(&p, REG_ID_BL_DISPLAY, &req, 100)); // just asked: not settled
    answer(&p, REG_ID_BL_DISPLAY, REG_T_U8, 40, 100 + REG_PROXY_SETTLE_MS);
    CHECK(reg_proxy_mismatch(&p, REG_ID_BL_DISPLAY, &req, 100 + REG_PROXY_SETTLE_MS));
    answer(&p, REG_ID_BL_DISPLAY, REG_T_U8, 40, 100 + 3 * REG_PROXY_SETTLE_MS);
    CHECK(!reg_proxy_mismatch(&p, REG_ID_BL_DISPLAY, &req, 100 + 3 * REG_PROXY_SETTLE_MS)); // once per key
    // A tolerated difference (the clamp, the synthesizer step) never warns
    reg_value_t const dim = {.u = 5}, freq = {.u = 869618000};
    answer(&p, REG_ID_LORA_FREQ, REG_T_U32, 869617980, 100);
    CHECK(!reg_proxy_mismatch(&p, REG_ID_LORA_FREQ, &freq, 100));
    answer(&p, REG_ID_LORA_FREQ, REG_T_U32, 869617980, 100 + REG_PROXY_SETTLE_MS);
    CHECK(!reg_proxy_mismatch(&p, REG_ID_LORA_FREQ, &freq, 100 + REG_PROXY_SETTLE_MS));
    // After a C6 reset the settle starts over: the C6 runs its defaults until the P4's levels arrive
    reg_proxy_t q;
    CHECK(reg_proxy_init(&q, ids, types, 4, 0));
    caps_answer(&q, 0x8016, 0);
    answer(&q, REG_ID_BL_DISPLAY, REG_T_U8, 10, 0);
    CHECK(!reg_proxy_mismatch(&q, REG_ID_BL_DISPLAY, &dim, 0));
    reg_proxy_link(&q, 10000);
    caps_answer(&q, 0x8016, 10000);
    answer(&q, REG_ID_BL_DISPLAY, REG_T_U8, 30, 10001);
    CHECK(!reg_proxy_mismatch(&q, REG_ID_BL_DISPLAY, &dim, 10001));
}

static void test_rows(void) {
    char               line[160];
    reg_entry_t const *bl = reg_find_name(reg_p4_table, reg_p4_count, "bl.display");
    reg_view_t         v  = {.has_saved = true, .has_requested = true, .has_applied = true, .has_age = true,
                             .saved = {.u = 50}, .requested = {.u = 50}, .applied = {.u = 50}, .age_ms = 1234};
    reg_cmd_header(line, sizeof(line));
    CHECK(strncmp(line, "key ", 4) == 0 && strstr(line, "applied") != NULL);
    reg_cmd_row(bl, &v, line, sizeof(line));
    CHECK(strcmp(line, "bl.display      50           50           50           %     1.2 s") == 0);
    v.requested.u = 95, v.applied.u = 80;
    reg_cmd_row(bl, &v, line, sizeof(line));
    CHECK(strstr(line, "\xe2\x89\x88" "80") != NULL);
    v = (reg_view_t){.note = RV_OLD_C6};
    reg_cmd_row(bl, &v, line, sizeof(line));
    CHECK(strstr(line, "?") != NULL && strstr(line, "not supported by this C6") != NULL);
    // The radio line for fw lora
    reg_view_t r[REG_RADIO_KEYS];
    uint32_t   vals[REG_RADIO_KEYS] = {869618000, 62500, 7, 5, 0x12, 16, (uint32_t)22, 2};
    for (int i = 0; i < REG_RADIO_KEYS; i++) {
        r[i] = (reg_view_t){.has_applied = true, .has_age = true, .applied = {.u = vals[i]}, .age_ms = 300};
    }
    reg_entry_t const *st = reg_find_id(reg_p4_table, reg_p4_count, REG_ID_LORA_STATE);
    reg_radio_text(r, st, line, sizeof(line));
    CHECK(strcmp(line, "869.618 MHz 62.5 kHz SF7 CR5 sync 0x12 pre 16 22 dBm rx | 0.3 s ago") == 0);
    r[6] = (reg_view_t){.note = RV_OLD_C6};
    reg_radio_text(r, st, line, sizeof(line));
    CHECK(strcmp(line, "? (not supported by this C6)") == 0);
}

int main(void) {
    test_table();
    test_set_guarded();
    test_set_rules();
    test_wifi_keys();
    test_cached_text();
    test_old_c6();
    test_caps_and_domains();
    test_caps_age();
    test_cancel();
    test_seq_and_cache();
    test_link();
    test_matches();
    test_mismatch();
    test_rows();
    if (failures) {
        printf("reg_service_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("reg_service_test: ok\n");
    return 0;
}
