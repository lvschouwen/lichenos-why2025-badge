// Repeater status history (#168) on the badge: see mesh_rhist_run.h
#include "mesh_rhist_run.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_console.h"
#include "lichen_crashlog.h"
#include "lichen_mem.h"
#include "lichen_settings.h"
#include "mesh.h"
#include "mesh_cscope.h"
#include "mesh_format.h"
#include "meshcore_run.h"

#include <stdio.h>
#include <string.h>

#define TICK_WAIT_MS  20  // the mesh loop never waits long for the table: it tries again next second
#define USER_WAIT_MS  200 // a screen or the console (one may be saving the setting meanwhile)
#define SAVE_RETRY_S  30
#define CONSOLE_SHOWN 24  // samples "fw mesh history <who>" prints

static char const TAG[]     = "meshcore";
static char const CFG_KEY[] = "rh_poll";

// The table is written by the mesh task (tick), the screens and the console, never from an interrupt: a mutex, so the
// copies (3 KB) and the setting's text and write happen with interrupts on. Made with the table, before its pointer
// is published; nothing takes it before the pointer is there.
static StaticSemaphore_t rh_lock_mem;
static SemaphoreHandle_t rh_lock;
static rh_t             *table; // PSRAM, made by the first tick; only that tick writes the pointer
static bool              table_failed;
static int               holds; // screens that ask a repeater, open now (atomic)
// Under rh_lock: the setting does not match the table (a write failed); the tick writes it again until it does
static bool     dirty;
static bool     console_off_pending; // #290: the console scope changed before the table was made
static bool     save_locked(rh_t *t);
static uint32_t retry_s;

static rh_t *get_table(void) {
    return __atomic_load_n(&table, __ATOMIC_ACQUIRE);
}

static bool lock(uint32_t wait_ms) {
    return xSemaphoreTake(rh_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

static void unlock(void) {
    xSemaphoreGive(rh_lock);
}

static void make_table(void) {
    rh_t *t = lichen_alloc(meshcore_ctx(), sizeof(rh_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!t) {
        table_failed = true;
        ESP_LOGW(TAG, "status history: no memory, no repeater is asked");
        return;
    }
    rh_lock = xSemaphoreCreateMutexStatic(&rh_lock_mem);
    rh_init(t);
    char     text[RH_CFG_MAX];
    uint8_t  prefix[RH_SLOTS][RH_PREFIX];
    uint16_t minutes[RH_SLOTS];
    bool     console[RH_SLOTS];
    app_settings_get_str(meshcore_ctx(), CFG_KEY, text, sizeof(text), "");
    size_t n     = rh_cfg_decode(text, prefix, minutes, console, RH_SLOTS);
    bool   crash = lichen_crashlog_after_crash(); // panic, watchdog, brownout: a restart loop must not ask more often
    rh_load(t, prefix, minutes, n, mesh_uptime_s(), crash);
    for (size_t i = 0; i < n; i++) {
        rh_set_console(t, prefix[i], console[i]); // #290: a console line's polls keep the console scope
    }
    // #290: the console scope is gone (a reset) or changed before the table was made: the console's polls go off
    bool   gone = mesh_cscope_refusal(true) || __atomic_exchange_n(&console_off_pending, false, __ATOMIC_ACQ_REL);
    size_t off  = gone ? rh_off_console(t) : 0;
    if (off) {
        save_locked(t); // rh_lock is not taken yet: nothing else sees the table before it is published below
        ESP_LOGW(TAG, "status history: %u console poll(s) switched off: the console scope changed", (unsigned)off);
    }
    if (n) {
        ESP_LOGI(TAG, "status history: %u repeater(s) switched on, first ask in %s", (unsigned)n,
                 crash ? "an interval (after a crash)" : "180 s");
    }
    __atomic_store_n(&table, t, __ATOMIC_RELEASE);
}

// The switched-on repeaters into the setting, erased when none is on (a fresh badge has no key). Caller holds rh_lock:
// the text and its write stay in order with the change. A failed write is retried by the tick.
static bool save_locked(rh_t *t) {
    char text[RH_CFG_MAX];
    rh_cfg_encode(t, text, sizeof(text));
    bool ok = text[0] ? app_settings_set_str(meshcore_ctx(), CFG_KEY, text)
                      : app_settings_erase(meshcore_ctx(), CFG_KEY);
    if (!ok && !dirty) {
        ESP_LOGW(TAG, "status history: the setting could not be written, retrying every %d s", SAVE_RETRY_S);
    } else if (ok && dirty) {
        ESP_LOGI(TAG, "status history: the setting is written now");
    }
    dirty   = !ok;
    retry_s = mesh_uptime_s() + SAVE_RETRY_S;
    return ok;
}

// Ours ended or a person's answer came, and the next due one, all under the lock; *idx the slot to ask, -1 for none
static void step_locked(rr_head_t const *h, bool have, uint32_t now_s, int *idx, uint8_t prefix[RH_PREFIX],
                        bool *console) {
    rh_observe(table, have ? h : NULL, now_s);
    bool held = __atomic_load_n(&holds, __ATOMIC_ACQUIRE) > 0;
    *idx      = rh_due(table, now_s, !have || rr_head_busy(h), rh_other_age_s(table, have ? h : NULL), held);
    if (*idx >= 0) {
        memcpy(prefix, table->slot[*idx].prefix, RH_PREFIX);
        *console = table->slot[*idx].console;
    }
    if (dirty && now_s >= retry_s) {
        save_locked(table);
    }
}

void mesh_rhist_tick(void) {
    static uint32_t checked_s;
    uint32_t        now_s = mesh_uptime_s();
    if (now_s == checked_s) {
        return;
    }
    checked_s = now_s;
    if (!get_table()) {
        if (!table_failed) {
            make_table();
        }
        return;
    }
    rr_head_t h; // internal RAM, as mesh_remote_head wants
    bool      have = mesh_remote_head(&h);
    int       idx  = -1;
    uint8_t   prefix[RH_PREFIX];
    bool      console = false;
    if (!lock(TICK_WAIT_MS)) {
        return;
    }
    step_locked(&h, have, now_s, &idx, prefix, &console);
    unlock();
    if (idx < 0) {
        return;
    }
    char const *why = NULL;
    uint32_t    seq = mesh_remote_start_background(prefix, console ? MESH_FROM_CONSOLE : MESH_FROM_SCREEN,
                                                   &why); // the #51 request; the person's replaces it
    lock(portMAX_DELAY);
    if (rh_find(table, prefix) == idx && table->slot[idx].on) { // not switched off meanwhile
        if (seq) {
            rh_asked(table, idx, seq, now_s);
        } else {
            rh_not_started(table, idx, why ? why : "could not start", now_s);
        }
    }
    unlock();
    if (seq) {
        ESP_LOGI(TAG, "status history: asked %02x%02x%02x%02x%02x%02x (#%lu)", prefix[0], prefix[1], prefix[2],
                 prefix[3], prefix[4], prefix[5], (unsigned long)seq);
    }
}

void mesh_rhist_hold(bool open) {
    __atomic_add_fetch(&holds, open ? 1 : -1, __ATOMIC_ACQ_REL);
}

// The slot (cleared when the repeater has none) and the time, read under the lock so no age comes out negative
static void view_locked(rh_t const *t, int idx, rh_view_t *out) {
    out->now_s    = mesh_uptime_s();
    out->have     = idx >= 0;
    out->asking   = idx >= 0 && t->asking == idx;
    out->on_count = rh_on_count(t);
    if (idx >= 0) {
        out->slot = t->slot[idx];
    } else {
        memset(&out->slot, 0, sizeof(out->slot));
    }
    out->unsaved = dirty;
}

bool mesh_rhist_view(uint8_t const prefix[RH_PREFIX], rh_view_t *out) {
    rh_t *t = get_table();
    if (!t || !lock(USER_WAIT_MS)) {
        return false;
    }
    view_locked(t, rh_find(t, prefix), out);
    unlock();
    return true;
}

bool mesh_rhist_is_on(uint8_t const prefix[RH_PREFIX]) {
    rh_t *t = get_table();
    if (!t || !lock(USER_WAIT_MS)) {
        return false;
    }
    int  idx = rh_find(t, prefix);
    bool on  = idx >= 0 && t->slot[idx].on;
    unlock();
    return on;
}

bool mesh_rhist_needs_confirm(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min) {
    rh_t *t = get_table();
    if (!t || !lock(USER_WAIT_MS)) {
        return on; // unknown: what could raise the rate asks
    }
    bool ask = rh_needs_confirm(t, prefix, on, interval_min);
    unlock();
    return ask;
}

rhs_result_t mesh_rhist_set(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min, mesh_origin_t from) {
    rh_t *t = get_table();
    if (!t || !mesh_running()) {
        return RHS_NOT_RUNNING;
    }
    if (!lock(USER_WAIT_MS)) {
        return RHS_BUSY;
    }
    rh_result_t r = rh_set(t, prefix, on, interval_min, mesh_uptime_s());
    if (r == RH_OK && on) {
        rh_set_console(t, prefix, from == MESH_FROM_CONSOLE); // the last to switch it on or change it decides
    }
    bool saved = r == RH_OK && save_locked(t);
    unlock();
    if (r != RH_OK) {
        return r == RH_FULL ? RHS_FULL : RHS_BAD;
    }
    ESP_LOGI(TAG, "status history: %02x%02x%02x%02x%02x%02x %s, every %u min", prefix[0], prefix[1], prefix[2],
             prefix[3], prefix[4], prefix[5], on ? "on" : "off", (unsigned)interval_min);
    return saved ? RHS_OK : RHS_NOT_SAVED;
}

void mesh_rhist_forget(uint8_t const prefix[RH_PREFIX]) {
    rh_t *t = get_table();
    if (!t) {
        return;
    }
    lock(portMAX_DELAY); // the contact is gone: its asks must stop, so this one waits for the lock
    if (rh_find(t, prefix) >= 0) {
        rh_forget(t, prefix);
        save_locked(t); // a failure is retried by the tick
    }
    unlock();
}

char const *mesh_rhist_result_text(rhs_result_t r) {
    switch (r) {
    case RHS_OK: return "saved";
    case RHS_FULL: return "At most 3 repeaters are asked at a time: switch one off first (its Status history page)";
    case RHS_NOT_RUNNING: return "MeshCore is not running";
    case RHS_NOT_SAVED: return "Changed, but not saved yet: the badge keeps trying to save it";
    case RHS_BUSY: return "MeshCore is busy: try again";
    default: return "That interval is not offered";
    }
}

size_t mesh_rhist_off_console(void) {
    rh_t *t = get_table();
    if (!t) {
        __atomic_store_n(&console_off_pending, true, __ATOMIC_RELEASE); // make_table does it
        return 0;
    }
    lock(portMAX_DELAY); // as mesh_rhist_forget: these polls must stop
    size_t n = rh_off_console(t);
    if (n) {
        save_locked(t); // a failure is retried by the tick
    }
    unlock();
    if (n) {
        ESP_LOGW(TAG, "status history: %u console poll(s) switched off: the console scope changed", (unsigned)n);
    }
    return n;
}

// ---- the console --------------------------------------------------------------------------------------------------------

static void console_samples(rh_view_t const *v) {
    rh_sample_t *smp = lichen_alloc(meshcore_ctx(), RH_SAMPLES * sizeof(rh_sample_t), LICHEN_MEM_BULK); // 3 KB
    if (!smp) {
        console_reply("  no memory for the samples");
        return;
    }
    size_t n    = rh_samples(&v->slot, smp, RH_SAMPLES);
    size_t from = n > CONSOLE_SHOWN ? n - CONSOLE_SHOWN : 0;
    console_reply("  %u sample(s)%s, oldest first:", (unsigned)n, from ? " (the newest 24)" : "");
    for (size_t k = from; k < n; k++) {
        char batt[16], noise[16], up[24], age[32];
        rh_value_text(RH_BATTERY, smp[k].batt_mv ? smp[k].batt_mv : RH_NONE, batt, sizeof(batt));
        rh_value_text(RH_NOISE, smp[k].noise, noise, sizeof(noise));
        rr_format_uptime(smp[k].up_secs, up, sizeof(up));
        mesh_format_age(v->now_s >= smp[k].at_s ? v->now_s - smp[k].at_s : 0, age, sizeof(age));
        console_reply("  %s: battery %s, noise floor %s, up %s, received %lu, sent %lu", age, batt, noise, up,
                      (unsigned long)smp[k].n_recv, (unsigned long)smp[k].n_sent);
    }
    lichen_free(meshcore_ctx(), smp);
}

static void console_slot(rh_view_t const *v, int i, bool samples) {
    rh_slot_t const *s = &v->slot;
    char             next[32], every[16], last[32];
    rh_next_text(s, v->asking, v->now_s, next, sizeof(next));
    rh_interval_text(s->interval_min, every, sizeof(every));
    if (s->answer_s) {
        mesh_format_age(v->now_s >= s->answer_s ? v->now_s - s->answer_s : 0, last, sizeof(last));
    } else {
        snprintf(last, sizeof(last), "none");
    }
    console_reply("history %d %02x%02x%02x%02x%02x%02x: %s, every %s, next ask %s | asks %lu, answers %lu, missed in a row "
                  "%u, wait now %lu min | last answer %s",
                  i, s->prefix[0], s->prefix[1], s->prefix[2], s->prefix[3], s->prefix[4], s->prefix[5],
                  s->on ? "on" : "off", every, next, (unsigned long)s->asks, (unsigned long)s->answers,
                  (unsigned)s->misses, (unsigned long)(rh_wait_s(s) / 60), last);
    if (s->why) {
        console_reply("  last problem: %s", s->why);
    }
    if (samples) {
        console_samples(v);
    }
}

static void console_list(void) {
    rh_t *t = get_table();
    if (!t) {
        console_reply("status history: MeshCore has not made its table yet");
        return;
    }
    rh_view_t *v = lichen_alloc(meshcore_ctx(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
        return;
    }
    int    shown = 0;
    size_t on    = 0;
    bool   unsaved = false;
    for (int i = 0; i < RH_SLOTS; i++) {
        if (!lock(USER_WAIT_MS)) {
            console_reply("MeshCore is busy: try again");
            break;
        }
        bool used = t->slot[i].used;
        view_locked(t, used ? i : -1, v);
        unlock();
        on      = v->on_count;
        unsaved = v->unsaved;
        if (used) {
            console_slot(v, i + 1, false);
            shown++;
        }
    }
    console_reply("%d of %d slots used, %u on%s; fw mesh history <who> [on [10|30|60|180] YES | off | interval <min> "
                  "[YES]]",
                  shown, RH_SLOTS, (unsigned)on, unsaved ? ", NOT SAVED yet (retrying)" : "");
    lichen_free(meshcore_ctx(), v);
}

// "on [min] YES", "interval <min> [YES]": the minutes and whether YES was given; false when the minutes are not a
// number that fits
static bool parse_change(char const *rest, unsigned *minutes, bool *yes) {
    char     a[12] = "", b[8] = "";
    int      n     = sscanf(rest, " %11s %7s", a, b);
    unsigned m     = 0;
    *yes           = (n >= 1 && strcmp(a, "YES") == 0) || (n == 2 && strcmp(b, "YES") == 0);
    if (n >= 1 && strcmp(a, "YES") != 0) {
        if (strspn(a, "0123456789") != strlen(a) || strlen(a) > 5 || sscanf(a, "%u", &m) != 1 || m > UINT16_MAX) {
            return false;
        }
        *minutes = m;
    }
    return true;
}

static void console_change(uint8_t const *prefix, char const *verb) {
    unsigned   minutes = RH_INTERVAL_DEF;
    bool       yes     = false;
    rh_view_t *v       = lichen_alloc(meshcore_ctx(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
        return;
    }
    bool have = mesh_rhist_view(prefix, v) && v->have;
    if (have) {
        minutes = v->slot.interval_min;
    }
    lichen_free(meshcore_ctx(), v);
    bool on;
    if (strcmp(verb, "off") == 0) {
        on = false;
    } else if (strncmp(verb, "interval ", 9) == 0 || (strncmp(verb, "on", 2) == 0 && (verb[2] == ' ' || !verb[2]))) {
        bool interval = verb[0] == 'i';
        if (!parse_change(verb + (interval ? 9 : 2), &minutes, &yes)) {
            console_reply("%s", mesh_rhist_result_text(RHS_BAD));
            return;
        }
        if (interval && !have) {
            console_reply("not switched on: fw mesh history <who> on %u YES", minutes);
            return;
        }
        on = true;
    } else {
        console_reply("fw mesh history <who> [on [10|30|60|180] YES | off | interval <min> [YES]]");
        return;
    }
    // what makes it transmit more often needs YES, as the screen asks in its careful dialog
    if (mesh_rhist_needs_confirm(prefix, on, (uint16_t)minutes) && !yes) {
        console_reply("this asks the repeater for its status on air %s: add YES at the end",
                      have ? "more often" : "at the interval");
        return;
    }
    char const *why = on ? mesh_cscope_refusal(true) : NULL; // #290: its asks would be console floods
    if (why) {
        console_reply("not switched on: %s", why);
        return;
    }
    rhs_result_t r = mesh_rhist_set(prefix, on, (uint16_t)minutes, MESH_FROM_CONSOLE);
    console_reply("%s", r == RHS_OK ? "done" : mesh_rhist_result_text(r));
}

void mesh_rhist_console(uint8_t const *prefix, char const *verb) {
    if (!prefix) {
        console_list();
        return;
    }
    if (verb && *verb) {
        console_change(prefix, verb);
        return;
    }
    rh_view_t *v = lichen_alloc(meshcore_ctx(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
    } else if (!mesh_rhist_view(prefix, v)) {
        console_reply("status history: MeshCore has not made its table yet, or is busy");
    } else if (!v->have) {
        console_reply("no status history for it: fw mesh history <who> on [10|30|60|180] YES switches it on");
    } else {
        console_slot(v, 0, true);
    }
    lichen_free(meshcore_ctx(), v);
}
