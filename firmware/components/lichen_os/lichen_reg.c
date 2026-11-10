/*
 * The P4's settings registry service (#307/#310): `fw get`, `fw set`, the `fw bl` alias, the proxy entries for the
 * C6's keys and the backlight's REG_SET path.
 *
 * Concurrency: one spinlock around the pure proxy cache (reg_proxy.c); nothing is called under it. The LoRa task
 * writes answers through lora's hooks; the console task asks live (posts REG_GETs, then polls the cache for a bounded
 * time); the LVGL task never waits: there a proxy key reads the cache only (ui_service_lock_check). Every post to the
 * C6 is lora_reg_get/_set, which never blocks: the LoRa task does the send.
 */

#include "backlight.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_os.h"
#include "lichen_sys_thermal.h"
#include "lora_os.h"
#include "reg_cmd.h"
#include "reg_p4.h"
#include "reg_proxy.h"
#include "ui_service.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "reg";

#define LIVE_WAIT_MS 500             // the console's wait for the C6's answers to one `fw get` (plus its pacing)
#define POLL_US      (30 * 1000000)  // background refresh of the cache (screens read it) and the mismatch check
#define REG_LINE_MAX     160

_Static_assert(REG_RADIO_KEYS == sizeof(reg_p4_radio_ids) / sizeof(reg_p4_radio_ids[0]), "radio key count");

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static reg_proxy_t  px; // under lock
static atomic_bool  started;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// The LVGL task (or a task inside the UI lock) never waits for the C6
static bool may_wait(void) {
    return ui_service_lock_check() != UI_LOCK_OK;
}

static rp_support_t support(uint16_t id) {
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    rp_support_t s = reg_proxy_support(&px, id, t);
    portEXIT_CRITICAL(&lock);
    return s;
}

static bool pending(uint16_t id) {
    portENTER_CRITICAL(&lock);
    bool p = reg_proxy_pending(&px, id);
    portEXIT_CRITICAL(&lock);
    return p;
}

// One request to the C6; false when it could not be posted (no LoRa, the C6 being recovered)
static bool post(uint16_t id, bool set, uint32_t value) {
    uint8_t  seq;
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    bool ok = reg_proxy_begin(&px, id, t, &seq);
    portEXIT_CRITICAL(&lock);
    if (ok && !(set ? lora_reg_set(id, value, seq) : lora_reg_get(id, seq))) {
        portENTER_CRITICAL(&lock);
        reg_proxy_cancel(&px, id, seq);
        portEXIT_CRITICAL(&lock);
        ok = false;
    }
    return ok;
}

// Until none of ids waits any more, or wait_ms passed
static void wait_answers(uint16_t const *ids, bool const *asked, size_t n, uint32_t wait_ms) {
    for (uint32_t waited = 0; waited < wait_ms; waited += 10) {
        bool any = false;
        for (size_t i = 0; i < n && !any; i++) {
            any = asked[i] && pending(ids[i]);
        }
        if (!any) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// c6.reg_caps before anything else: an old C6 is asked again only after 5 s, so a run of console commands on it does
// not wait a second each
static void ensure_caps(void) {
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    reg_proxy_slot_t const *c   = reg_proxy_slot(&px, REG_ID_C6_REG_CAPS);
    bool                    ask = !reg_proxy_caps_known(&px) && !(c->pending && t - c->sent_ms < 5000);
    portEXIT_CRITICAL(&lock);
    uint16_t const id    = REG_ID_C6_REG_CAPS;
    bool           asked = ask && post(id, false, 0);
    if (asked || pending(id)) {
        asked = true;
        wait_answers(&id, &asked, 1, REG_PROXY_CAPS_TIMEOUT_MS);
    }
}

// Asks the C6 for the proxy keys among es and waits briefly; no_link[i] when key i could not be asked
static void ask_live(reg_entry_t const *const *es, size_t n, bool *no_link) {
    uint16_t ids[REG_PROXY_MAX] = {0};
    bool     asked[REG_PROXY_MAX] = {false};
    size_t   k = 0;
    bool     any_proxy = false;
    for (size_t i = 0; i < n; i++) {
        any_proxy = any_proxy || reg_p4_is_proxy(es[i]);
    }
    if (any_proxy) { // a query of P4 keys only (bl.idle) never waits for the C6
        ensure_caps();
    }
    for (size_t i = 0; i < n; i++) {
        no_link[i] = false;
        if (!reg_p4_is_proxy(es[i]) || k == REG_PROXY_MAX || support(es[i]->id) != RP_OK) {
            continue;
        }
        ids[k]     = es[i]->id;
        asked[k]   = es[i]->id == REG_ID_C6_REG_CAPS ? pending(ids[k]) : post(ids[k], false, 0);
        no_link[i] = !asked[k] && !pending(ids[k]);
        k++;
    }
    wait_answers(ids, asked, k, LIVE_WAIT_MS + lora_reg_burst_ms(k)); // #314: they go out a few at a time
}

// ---- saved and requested ------------------------------------------------------------------------------------------

static bool saved_of(reg_entry_t const *e, reg_value_t *v) {
    uint8_t d, k;
    if (e->id != REG_ID_BL_DISPLAY && e->id != REG_ID_BL_KEYBOARD) {
        return false;
    }
    backlight_get(&d, &k);
    v->u = e->id == REG_ID_BL_DISPLAY ? d : k;
    return true;
}

static bool lora_requested(uint16_t id, reg_value_t *v) {
    lora_requested_t r;
    lora_get_requested(&r);
    switch (id) {
        case REG_ID_LORA_FREQ: v->u = r.freq_hz; return true;
        case REG_ID_LORA_BW: v->u = r.bw_hz; return true;
        case REG_ID_LORA_SF: v->u = r.sf; return true;
        case REG_ID_LORA_CR: v->u = r.cr; return true;
        case REG_ID_LORA_SYNC: v->u = r.sync_word; return true;
        case REG_ID_LORA_PREAMBLE: v->u = r.preamble; return true;
        case REG_ID_LORA_POWER: v->i = r.tx_power_dbm; return true;
        default: return false;
    }
}

// What the P4 asked for: the backlight driver's last levels (a dim included), the LoRa task's copy of the radio config
static bool requested_of(uint16_t id, reg_value_t *v) {
    uint8_t d, k;
    if (id == REG_ID_BL_DISPLAY || id == REG_ID_BL_KEYBOARD) {
        backlight_get_requested(&d, &k);
        v->u = id == REG_ID_BL_DISPLAY ? d : k;
        return true;
    }
    return lora_requested(id, v);
}

static void build_view(reg_entry_t const *e, bool live, bool no_link, reg_view_t *v) {
    memset(v, 0, sizeof(*v));
    v->has_saved     = saved_of(e, &v->saved);
    v->has_requested = requested_of(e->id, &v->requested);
    if (!reg_p4_is_proxy(e)) {
        v->has_applied = reg_get(e, &v->applied) == REG_OK;
        v->note        = v->has_applied ? RV_NONE : RV_FAILED;
        return;
    }
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    reg_proxy_view(&px, e->id, t, live, no_link, v);
    portEXIT_CRITICAL(&lock);
}

// ---- the table's getters and setter (reg_p4.h) ----------------------------------------------------------------------

reg_status_t reg_p4_proxy_get(reg_entry_t const *e, reg_value_t *out) {
    bool live = may_wait() && atomic_load(&started), no_link = false;
    if (live) {
        ask_live(&e, 1, &no_link);
    }
    reg_view_t v;
    build_view(e, live, no_link, &v);
    if (!v.has_applied || (live && v.note == RV_STALE)) {
        return REG_FAILED;
    }
    *out = v.applied;
    return REG_OK;
}

reg_status_t reg_p4_bl_set(reg_entry_t const *e, reg_value_t const *v) {
    uint8_t d, k;
    backlight_get(&d, &k);
    if (e->id == REG_ID_BL_DISPLAY) {
        d = (uint8_t)v->u;
    } else {
        k = (uint8_t)v->u;
    }
    backlight_set_saved(d, k); // the driver sends it: REG_SET through bl_reg_send, or the UART line
    return REG_OK;
}

reg_status_t reg_p4_bl_mode_get(reg_entry_t const *e, reg_value_t *out) {
    backlight_idle_t     idle;
    backlight_override_t ovr;
    backlight_mode(&idle, &ovr);
    if (e->id == REG_ID_BL_IDLE) {
        out->u = (uint32_t)idle;
        return REG_OK;
    }
    lichen_sys_thermal_info_t th;
    lichen_sys_thermal_info(&th);
    // bl_override_names: none, thermal, flashlight, pulse; the load shed holds the minimum through backlight_set
    out->u = ovr == BACKLIGHT_OVR_FLASHLIGHT ? 2 : ovr == BACKLIGHT_OVR_PULSE ? 3 : th.level != TH_OK ? 1 : 0;
    return REG_OK;
}

// backlight_bind_reg_send: the levels as REG_SET once the C6 said it serves bl.*; any task, never waits
static bool bl_reg_send(uint8_t display, uint8_t keyboard) {
    if (!atomic_load(&started) || support(REG_ID_BL_DISPLAY) != RP_OK) {
        return false;
    }
    return post(REG_ID_BL_DISPLAY, true, display) && post(REG_ID_BL_KEYBOARD, true, keyboard);
}

// The restart blank's first step (backlight_bind_reg_send): the queued and later bl.* REG_SETs never reach the C6
static void bl_reg_close(void) {
    static uint16_t const ids[] = {REG_ID_BL_DISPLAY, REG_ID_BL_KEYBOARD};
    lora_reg_block(ids, sizeof(ids) / sizeof(ids[0]), 50);
}

// ---- lora's hooks (LoRa task) ---------------------------------------------------------------------------------------

static void warn_mismatch(uint16_t id, reg_value_t const *req, uint32_t t) {
    reg_entry_t const *e = reg_find_id(reg_p4_table, reg_p4_count, id);
    portENTER_CRITICAL(&lock);
    bool                    warn = reg_proxy_mismatch(&px, id, req, t);
    reg_proxy_slot_t const *s    = reg_proxy_slot(&px, id);
    reg_value_t             got  = {.u = s ? s->value : 0};
    portEXIT_CRITICAL(&lock);
    char a[REG_STR_MAX + 1], r[REG_STR_MAX + 1];
    if (warn && e && reg_format(e, &got, a, sizeof(a)) && reg_format(e, req, r, sizeof(r))) {
        ESP_LOGW(TAG, "%s: the C6 applies %s, the P4 asked for %s (logged once)", e->name, a, r);
    }
}

static void on_answer(uint16_t id, uint8_t seq, uint8_t status, uint8_t type, uint32_t value) {
    static bool told_type;
    uint32_t    t = now_ms();
    portENTER_CRITICAL(&lock);
    rp_answer_t r = reg_proxy_answer(&px, id, seq, status, type, value, t);
    portEXIT_CRITICAL(&lock);
    if (r == RP_BAD_TYPE && !told_type) {
        told_type = true;
        ESP_LOGW(TAG, "REG_VAL for 0x%04x with type %u dropped (not this build's)", id, type);
    }
    reg_value_t req;
    if (r == RP_ACCEPTED && requested_of(id, &req)) {
        warn_mismatch(id, &req, t);
    }
}

static void on_link(bool up) {
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    reg_proxy_link(&px, t);
    portEXIT_CRITICAL(&lock);
    if (up) {
        post(REG_ID_C6_REG_CAPS, false, 0); // the backlight goes back to REG_SET once it is answered
    }
}

static size_t radio_text(char *buf, size_t cap) {
    reg_entry_t const *es[REG_RADIO_KEYS];
    bool               no_link[REG_RADIO_KEYS] = {false};
    reg_view_t         v[REG_RADIO_KEYS];
    for (size_t i = 0; i < REG_RADIO_KEYS; i++) {
        es[i] = reg_find_id(reg_p4_table, reg_p4_count, reg_p4_radio_ids[i]);
    }
    bool live = may_wait();
    if (live) {
        ask_live(es, REG_RADIO_KEYS, no_link);
    }
    for (size_t i = 0; i < REG_RADIO_KEYS; i++) {
        build_view(es[i], live, no_link[i], &v[i]);
    }
    return reg_radio_text(v, es[REG_RADIO_KEYS - 1], buf, cap);
}

// Background: keeps the screens' cache fresh and the mismatch check fed; esp_timer task, posts only
static void poll(void *arg) {
    (void)arg;
    uint32_t t = now_ms();
    portENTER_CRITICAL(&lock);
    bool due   = reg_proxy_caps_due(&px, t);
    bool known = reg_proxy_caps_known(&px);
    portEXIT_CRITICAL(&lock);
    if (due) {
        post(REG_ID_C6_REG_CAPS, false, 0);
    }
    for (size_t i = 0; known && i < reg_p4_count; i++) {
        reg_entry_t const *e = &reg_p4_table[i];
        if (reg_p4_is_proxy(e) && e->id != REG_ID_C6_REG_CAPS && support(e->id) == RP_OK) {
            post(e->id, false, 0);
        }
    }
}

// ---- console --------------------------------------------------------------------------------------------------------

static void print_rows(char const *query) {
    reg_entry_t const *es[REG_PROXY_MAX + 8];
    bool               no_link[sizeof(es) / sizeof(es[0])];
    size_t             n = 0;
    for (size_t i = 0; i < reg_p4_count && n < sizeof(es) / sizeof(es[0]); i++) {
        if (reg_name_matches(reg_p4_table[i].name, query)) {
            es[n++] = &reg_p4_table[i];
        }
    }
    if (!n) {
        console_reply("ERR unknown key: %s", query);
        return;
    }
    ask_live(es, n, no_link);
    char line[REG_LINE_MAX];
    reg_cmd_header(line, sizeof(line));
    console_reply("%s", line);
    for (size_t i = 0; i < n; i++) {
        reg_view_t v;
        build_view(es[i], true, no_link[i], &v);
        reg_cmd_row(es[i], &v, line, sizeof(line));
        console_reply("%s", line);
    }
}

static void cmd_get(char const *args) {
    print_rows(args);
}

// A C6 key this C6 can read back: false on an old C6 or one without the domain, whose bl.* levels go over UART1
static bool confirmable(reg_entry_t const *e) {
    rp_support_t s = support(e->id);
    return !reg_p4_is_proxy(e) || (s != RP_OLD_C6 && s != RP_NO_DOMAIN);
}

static reg_cmd_result_t set_and_reply(char const *args, bool quiet) {
    reg_entry_t const *e;
    reg_value_t        applied;
    bool               have;
    reg_cmd_result_t   r = reg_cmd_set(reg_p4_table, reg_p4_count, args, confirmable, &e, &applied, &have);
    if (!(quiet && (r == RC_OK || r == RC_FAILED || r == RC_UNCONFIRMED))) { // the alias prints the rows instead
        char line[REG_LINE_MAX];
        reg_cmd_set_reply(r, e, &applied, have, args, line, sizeof(line));
        console_reply("%s", line);
    }
    return r;
}

static void cmd_set(char const *args) {
    set_and_reply(args, false);
}

// "fw bl" (#310): a thin alias over set/get bl.*, with the line the bench docs quote ("backlight <d> <k>", saved)
static void cmd_bl(char const *args) {
    unsigned d, k;
    char     extra;
    if (strcmp(args, "r16") == 0) {
        backlight_r16_console();
        return;
    }
    if (*args && (sscanf(args, "%u %u %c", &d, &k, &extra) != 2 || d > 100 || k > 100)) {
        console_reply("bl [<display %d-100> <keyboard 0-100>] | bl r16", BACKLIGHT_DISPLAY_MIN);
        return;
    }
    if (*args && !atomic_load(&started)) {
        backlight_set_saved((uint8_t)d, (uint8_t)k); // no registry (its table failed the check): the driver as before
    } else if (*args) {
        char set[40];
        snprintf(set, sizeof(set), "bl.display %u", d < BACKLIGHT_DISPLAY_MIN ? BACKLIGHT_DISPLAY_MIN : d);
        set_and_reply(set, true);
        snprintf(set, sizeof(set), "bl.keyboard %u", k);
        set_and_reply(set, true);
    }
    uint8_t sd, sk;
    backlight_get(&sd, &sk);
    console_reply("backlight %u %u", sd, sk);
    if (atomic_load(&started)) {
        print_rows("bl");
    }
}

static lora_reg_hooks_t const hooks = {.answer = on_answer, .link = on_link, .radio_text = radio_text};

static void start_poll(void) {
    static esp_timer_handle_t     timer;
    esp_timer_create_args_t const args = {.callback = poll, .name = "reg_poll"};
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, POLL_US) != ESP_OK) {
        ESP_LOGW(TAG, "no poll timer: the C6's values refresh only when asked");
    }
}

void lichen_reg_start(void) {
    uint16_t ids[REG_PROXY_MAX] = {0};
    uint8_t  types[REG_PROXY_MAX] = {0};
    size_t   n = 0;
    for (size_t i = 0; i < reg_p4_count; i++) {
        if (reg_p4_is_proxy(&reg_p4_table[i]) && n < REG_PROXY_MAX) {
            ids[n]     = reg_p4_table[i].id;
            types[n++] = reg_p4_table[i].type;
        }
    }
    // "fw bl" (and "bl r16") whether or not the registry starts: it replaced the driver's own command
    console_register("bl", "bl [<display> <keyboard>] (backlight 0-100, screen at least 5; = fw set/get bl.*) | bl r16 "
                           "(is R16 fitted?)",
                     cmd_bl);
    size_t bad = reg_table_check(reg_p4_table, reg_p4_count);
    if (bad != reg_p4_count || !reg_proxy_init(&px, ids, types, n, now_ms())) {
        ESP_LOGE(TAG, "registry table bad at entry %u: not started", (unsigned)bad);
        return;
    }
    atomic_store(&started, true);
    lora_set_reg_hooks(&hooks);
    backlight_bind_reg_send(bl_reg_send, bl_reg_close);
    console_register("get", "get [key|prefix] (settings registry: saved / requested / applied / unit / age; fw get bl.)",
                     cmd_get);
    console_register("set", "set <key> <value> (a registry key; radio keys are guarded: fw mesh radio)", cmd_set);
    start_poll();
    post(REG_ID_C6_REG_CAPS, false, 0); // its answer switches the backlight to REG_SET
}
