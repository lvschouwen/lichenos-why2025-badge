// Repeater status history (#168) on the badge: see mesh_rhist_run.h
#include "mesh_rhist_run.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "fw_console.h"
#include "lichen_mem.h"
#include "lichen_settings.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_run.h"

#include <stdio.h>
#include <string.h>

static char const TAG[]   = "meshcore";
static char const CFG_KEY[] = "rh_poll";

// The table is written by the mesh task (tick), the screens and the console; every access is a short copy under this
// spinlock, never with LVGL's lock or a file write inside it
static portMUX_TYPE rh_mux = portMUX_INITIALIZER_UNLOCKED;
static rh_t        *table; // PSRAM, made by the first tick; only that tick writes the pointer
static bool         table_failed;

static rh_t *get_table(void) {
    return __atomic_load_n(&table, __ATOMIC_ACQUIRE);
}

static void make_table(void) {
    rh_t *t = lichen_alloc(meshcore_self(), sizeof(rh_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!t) {
        table_failed = true;
        ESP_LOGW(TAG, "status history: no memory, no repeater is asked");
        return;
    }
    rh_init(t);
    char     text[RH_CFG_MAX];
    uint8_t  prefix[RH_SLOTS][RH_PREFIX];
    uint16_t minutes[RH_SLOTS];
    app_settings_get_str(meshcore_self(), CFG_KEY, text, sizeof(text), "");
    size_t n = rh_cfg_decode(text, prefix, minutes, RH_SLOTS);
    rh_load(t, prefix, minutes, n, mesh_uptime_s());
    if (n) {
        ESP_LOGI(TAG, "status history: %u repeater(s) switched on, first ask in %u s", (unsigned)n, RH_FIRST_S);
    }
    __atomic_store_n(&table, t, __ATOMIC_RELEASE);
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
    portENTER_CRITICAL(&rh_mux);
    rh_observe(table, have ? &h : NULL, now_s);
    int idx = rh_due(table, now_s, !have || rr_head_busy(&h), rh_other_age_s(table, have ? &h : NULL));
    uint8_t prefix[RH_PREFIX];
    if (idx >= 0) {
        memcpy(prefix, table->slot[idx].prefix, RH_PREFIX);
    }
    portEXIT_CRITICAL(&rh_mux);
    if (idx < 0) {
        return;
    }
    char const *why = NULL;
    uint32_t    seq = mesh_remote_start(prefix, RR_STATUS, NULL, &why); // the #51 request; it takes ob_mux only
    portENTER_CRITICAL(&rh_mux);
    if (rh_find(table, prefix) == idx && table->slot[idx].on) { // not switched off meanwhile
        if (seq) {
            rh_asked(table, idx, seq, now_s);
        } else {
            rh_not_started(table, idx, why ? why : "could not start", now_s);
        }
    }
    portEXIT_CRITICAL(&rh_mux);
    if (seq) {
        ESP_LOGI(TAG, "status history: asked %02x%02x%02x%02x%02x%02x (#%lu)", prefix[0], prefix[1], prefix[2],
                 prefix[3], prefix[4], prefix[5], (unsigned long)seq);
    }
}

bool mesh_rhist_view(uint8_t const prefix[RH_PREFIX], rh_view_t *out) {
    rh_t *t = get_table();
    if (!t) {
        return false;
    }
    out->now_s = mesh_uptime_s();
    portENTER_CRITICAL(&rh_mux);
    int idx       = rh_find(t, prefix);
    out->have     = idx >= 0;
    out->asking   = idx >= 0 && t->asking == idx;
    out->on_count = rh_on_count(t);
    if (idx >= 0) {
        out->slot = t->slot[idx];
    }
    portEXIT_CRITICAL(&rh_mux);
    return true;
}

bool mesh_rhist_is_on(uint8_t const prefix[RH_PREFIX]) {
    rh_t *t = get_table();
    if (!t) {
        return false;
    }
    portENTER_CRITICAL(&rh_mux);
    int  idx = rh_find(t, prefix);
    bool on  = idx >= 0 && t->slot[idx].on;
    portEXIT_CRITICAL(&rh_mux);
    return on;
}

// The switched-on repeaters into the setting; erased when none is on (a fresh badge has no key)
static bool save(rh_t *t) {
    char text[RH_CFG_MAX];
    portENTER_CRITICAL(&rh_mux);
    rh_cfg_encode(t, text, sizeof(text));
    portEXIT_CRITICAL(&rh_mux);
    return text[0] ? app_settings_set_str(meshcore_self(), CFG_KEY, text) : app_settings_erase(meshcore_self(), CFG_KEY);
}

rhs_result_t mesh_rhist_set(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min) {
    rh_t *t = get_table();
    if (!t || !mesh_running()) {
        return RHS_NOT_RUNNING;
    }
    portENTER_CRITICAL(&rh_mux);
    rh_result_t r = rh_set(t, prefix, on, interval_min, mesh_uptime_s());
    portEXIT_CRITICAL(&rh_mux);
    if (r != RH_OK) {
        return r == RH_FULL ? RHS_FULL : RHS_BAD;
    }
    ESP_LOGI(TAG, "status history: %02x%02x%02x%02x%02x%02x %s, every %u min", prefix[0], prefix[1], prefix[2],
             prefix[3], prefix[4], prefix[5], on ? "on" : "off", (unsigned)interval_min);
    return save(t) ? RHS_OK : RHS_NOT_SAVED;
}

void mesh_rhist_forget(uint8_t const prefix[RH_PREFIX]) {
    rh_t *t = get_table();
    if (!t) {
        return;
    }
    portENTER_CRITICAL(&rh_mux);
    bool had = rh_find(t, prefix) >= 0;
    rh_forget(t, prefix);
    portEXIT_CRITICAL(&rh_mux);
    if (had) {
        save(t);
    }
}

char const *mesh_rhist_result_text(rhs_result_t r) {
    switch (r) {
    case RHS_OK: return "saved";
    case RHS_FULL: return "At most 3 repeaters are asked at a time: switch one off first (its Status history page)";
    case RHS_NOT_RUNNING: return "MeshCore is not running";
    case RHS_NOT_SAVED: return "Could not be saved: it holds until the badge restarts";
    default: return "That interval is not offered";
    }
}

// ---- the console --------------------------------------------------------------------------------------------------------

static void console_slot(rh_view_t const *v, int i, bool samples) {
    rh_slot_t const *s = &v->slot;
    char             next[32], every[16], last[32];
    rh_next_text(s, v->asking, v->now_s, next, sizeof(next));
    rh_interval_text(s->interval_min, every, sizeof(every));
    if (s->answer_s) {
        mesh_format_age(v->now_s - s->answer_s, last, sizeof(last));
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
    if (!samples) {
        return;
    }
    rh_sample_t *smp = lichen_alloc(meshcore_self(), RH_SAMPLES * sizeof(rh_sample_t), LICHEN_MEM_BULK); // 3 KB
    if (!smp) {
        console_reply("  no memory for the samples");
        return;
    }
    size_t n    = rh_samples(s, smp, RH_SAMPLES);
    size_t from = n > 24 ? n - 24 : 0;
    console_reply("  %u sample(s)%s, oldest first:", (unsigned)n, from ? " (the newest 24)" : "");
    for (size_t k = from; k < n; k++) {
        char batt[16], noise[16], up[24], age[32];
        rh_value_text(RH_BATTERY, smp[k].batt_mv ? smp[k].batt_mv : RH_NONE, batt, sizeof(batt));
        rh_value_text(RH_NOISE, smp[k].noise, noise, sizeof(noise));
        rr_format_uptime(smp[k].up_secs, up, sizeof(up));
        mesh_format_age(v->now_s - smp[k].at_s, age, sizeof(age));
        console_reply("  %s: battery %s, noise floor %s, up %s, received %lu, sent %lu", age, batt, noise, up,
                      (unsigned long)smp[k].n_recv, (unsigned long)smp[k].n_sent);
    }
    lichen_free(meshcore_self(), smp);
}

static void console_list(void) {
    rh_t *t = get_table();
    if (!t) {
        console_reply("status history: MeshCore has not made its table yet");
        return;
    }
    rh_view_t *v = lichen_alloc(meshcore_self(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
        return;
    }
    int shown = 0;
    for (int i = 0; i < RH_SLOTS; i++) {
        v->now_s = mesh_uptime_s();
        portENTER_CRITICAL(&rh_mux);
        v->have     = t->slot[i].used;
        v->slot     = t->slot[i];
        v->asking   = t->asking == i;
        v->on_count = rh_on_count(t);
        portEXIT_CRITICAL(&rh_mux);
        if (v->have) {
            console_slot(v, i + 1, false);
            shown++;
        }
    }
    console_reply("%d of %d slots used, %u on; fw mesh history <who> [on [10|30|60|180] YES | off | interval <min>]",
                  shown, RH_SLOTS, (unsigned)(shown ? v->on_count : 0));
    lichen_free(meshcore_self(), v);
}

static void console_change(uint8_t const *prefix, char const *verb) {
    unsigned    minutes = RH_INTERVAL_DEF;
    char        yes[8]  = "";
    rh_view_t  *v       = lichen_alloc(meshcore_self(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
        return;
    }
    bool have = mesh_rhist_view(prefix, v) && v->have;
    if (have) {
        minutes = v->slot.interval_min;
    }
    lichen_free(meshcore_self(), v);
    rhs_result_t r;
    if (strcmp(verb, "off") == 0) {
        r = mesh_rhist_set(prefix, false, (uint16_t)minutes);
    } else if (sscanf(verb, "interval %u", &minutes) == 1) {
        if (!have) {
            console_reply("not switched on: fw mesh history <who> on %u YES", minutes);
            return;
        }
        r = mesh_rhist_set(prefix, true, (uint16_t)minutes);
    } else if (strncmp(verb, "on", 2) == 0 && (verb[2] == ' ' || !verb[2])) {
        // "on YES" or "on 10 YES": the poll transmits, so YES as for the other commands that send
        if (sscanf(verb + 2, " %u %7s", &minutes, yes) < 2 && sscanf(verb + 2, " %7s", yes) != 1) {
            yes[0] = 0;
        }
        if (strcmp(yes, "YES") != 0) {
            console_reply("this asks the repeater for its status on air at the interval: add YES at the end, fw mesh "
                          "history <who> on [10|30|60|180] YES");
            return;
        }
        r = mesh_rhist_set(prefix, true, (uint16_t)minutes);
    } else {
        console_reply("fw mesh history <who> [on [10|30|60|180] YES | off | interval <min>]");
        return;
    }
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
    rh_view_t *v = lichen_alloc(meshcore_self(), sizeof(*v), LICHEN_MEM_BULK);
    if (!v) {
        console_reply("no memory");
    } else if (!mesh_rhist_view(prefix, v)) {
        console_reply("status history: MeshCore has not made its table yet");
    } else if (!v->have) {
        console_reply("no status history for it: fw mesh history <who> on [10|30|60|180] YES switches it on");
    } else {
        console_slot(v, 0, true);
    }
    lichen_free(meshcore_self(), v);
}
