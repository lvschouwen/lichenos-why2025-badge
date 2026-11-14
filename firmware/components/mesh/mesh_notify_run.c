// MeshCore's side of the notification service (#50): see mesh_notify_run.h
#include "mesh_notify_run.h"

#include "freertos/FreeRTOS.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_notify.h"
#include "lichen_settings.h"
#include "mesh.h"
#include "meshcore_run.h"

#include <stdio.h>
#include <string.h>

#define DECISIONS  5
#define MAX_POSTS  10 // merged events posted one by one, so the centre's count follows

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static mn_switches_t   sw = MN_DEFAULT_SWITCHES;
static bool            sw_loaded;
static mn_cache_t      cache;
static mn_queue_t      queue;
static mn_failtrack_t  failtrack;
static bool            failures_muted;
static bool            open_set;
static mesh_chat_key_t open_key;
static bool            dirty; // a setting changed: the badge is counted again

static struct {
    uint32_t posted, silent, post_refused, merged;
    uint32_t skipped[MN_R_COUNT];
} stats;
static char decisions[DECISIONS][88];
static uint8_t decision_next;

// MeshCore's context; LICHEN_CTX_NONE before it registered (the settings then keep their defaults, asked again later)
static lichen_ctx_t app(void) {
    return meshcore_ctx();
}

char const *mn_mode_name(mn_mode_t mode) {
    static char const *const NAME[MN_MODE_COUNT] = {"All", "Mentions", "Off"};
    return mode < MN_MODE_COUNT ? NAME[mode] : "?";
}

// ---- settings ----------------------------------------------------------------------------------------------------

static char const *const SW_KEY[] = {"n_dm", "n_fail", "n_ment"};

static void load_switches(void) {
    if (__atomic_load_n(&sw_loaded, __ATOMIC_ACQUIRE)) {
        return;
    }
    lichen_ctx_t a = app();
    if (a == LICHEN_CTX_NONE) {
        return; // not registered yet: the defaults, and ask again
    }
    mn_switches_t s = {
        app_settings_get_u8(a, SW_KEY[MN_SW_DM], 1) != 0,
        app_settings_get_u8(a, SW_KEY[MN_SW_FAIL], 1) != 0,
        app_settings_get_u8(a, SW_KEY[MN_SW_MENTIONS], 1) != 0,
    };
    portENTER_CRITICAL(&mux);
    sw = s;
    portEXIT_CRITICAL(&mux);
    __atomic_store_n(&sw_loaded, true, __ATOMIC_RELEASE);
}

static mn_switches_t switches(void) {
    load_switches();
    portENTER_CRITICAL(&mux);
    mn_switches_t s = sw;
    portEXIT_CRITICAL(&mux);
    return s;
}

bool mesh_notify_get_switch(mn_switch_t which) {
    mn_switches_t s = switches();
    return which == MN_SW_DM ? s.dm : which == MN_SW_FAIL ? s.fail : s.mentions;
}

bool mesh_notify_set_switch(mn_switch_t which, bool on) {
    load_switches();
    lichen_ctx_t a  = app();
    bool         ok = a != LICHEN_CTX_NONE && app_settings_set_u8(a, SW_KEY[which], on ? 1 : 0);
    portENTER_CRITICAL(&mux);
    if (which == MN_SW_DM) {
        sw.dm = on;
    } else if (which == MN_SW_FAIL) {
        sw.fail = on;
    } else {
        sw.mentions = on;
    }
    dirty = true;
    portEXIT_CRITICAL(&mux);
    return ok;
}

// The mode, from the cache or (once per chat) from storage. *read is set when storage was asked.
static mn_mode_t mode_for(mesh_chat_key_t const *key, bool *read) {
    mn_mode_t       mode;
    mesh_chat_key_t chat = *key; // the spinlock sees only internal RAM (#271)
    portENTER_CRITICAL(&mux);
    bool hit = mn_cache_lookup(&cache, &chat, &mode);
    portEXIT_CRITICAL(&mux);
    if (hit) {
        return mode;
    }
    mode           = mn_default_mode(key);
    lichen_ctx_t a = app();
    if (a != LICHEN_CTX_NONE) {
        char nvs[10];
        mn_nvs_key(key, nvs);
        uint8_t v = app_settings_get_u8(a, nvs, (uint8_t)mode);
        mode      = v < MN_MODE_COUNT ? (mn_mode_t)v : mode;
        if (read) {
            *read = true;
        }
    }
    portENTER_CRITICAL(&mux);
    mn_cache_store(&cache, &chat, mode);
    portEXIT_CRITICAL(&mux);
    return mode;
}

mn_mode_t mesh_notify_chan_mode(char const *channel_name) {
    mesh_chat_key_t key;
    mesh_chat_key_channel(&key, channel_name);
    return mode_for(&key, NULL);
}

bool mesh_notify_set_chan_mode(char const *channel_name, mn_mode_t mode) {
    mesh_chat_key_t key;
    mesh_chat_key_channel(&key, channel_name);
    lichen_ctx_t a = app();
    char         nvs[10];
    mn_nvs_key(&key, nvs);
    bool ok = a != LICHEN_CTX_NONE &&
              (mode == mn_default_mode(&key) ? app_settings_erase(a, nvs) : app_settings_set_u8(a, nvs, (uint8_t)mode));
    portENTER_CRITICAL(&mux);
    mn_cache_store(&cache, &key, mode);
    dirty = true;
    portEXIT_CRITICAL(&mux);
    return ok;
}

bool mesh_notify_counts_unread(mesh_chat_key_t const *key, void *ctx) {
    (void)ctx;
    mn_mode_t       mode;
    mesh_chat_key_t chat = *key; // the store's keys are in PSRAM (mesh_store.c); the spinlock sees only the copy (#271)
    portENTER_CRITICAL(&mux);
    bool hit = mn_cache_lookup(&cache, &chat, &mode);
    portEXIT_CRITICAL(&mux);
    return mn_counts_unread(key, hit ? mode : mn_default_mode(key));
}

// ---- events ------------------------------------------------------------------------------------------------------

static bool my_name(char *out, size_t cap) {
    mesh_settings_t s;
    if (!mesh_get_settings(&s)) {
        return false;
    }
    snprintf(out, cap, "%s", s.name);
    return true;
}

static void push(mn_kind_t kind, mesh_chat_key_t const *key, char const *title, char const *body, bool mention) {
    mn_text_t       text; // copied before the spinlock: a channel's name is in MeshCore's tables, in PSRAM (#271)
    mesh_chat_key_t chat = *key;
    mn_text_prepare(&text, title, body);
    portENTER_CRITICAL(&mux);
    mn_queue_push(&queue, kind, &chat, text.title, text.body, mention);
    portEXIT_CRITICAL(&mux);
}

void mesh_notify_dm(mesh_chat_key_t const *key, char const *from_title, char const *text) {
    push(MN_DM, key, from_title, text, false);
}

void mesh_notify_channel(mesh_chat_key_t const *key, char const *channel_name, char const *text) {
    char name[MESH_NAME_MAX + 1];
    push(MN_CHANNEL, key, channel_name, text, my_name(name, sizeof(name)) && mn_is_mention(text, name));
}

void mesh_notify_failed(mesh_chat_key_t const *key, char const *title, char const *text) {
    char body[MESH_TEXT_MAX + 16];
    snprintf(body, sizeof(body), "Not delivered: %s", text);
    push(MN_FAILED, key, title, body, false);
}

void mesh_notify_mute_failures(bool muted) {
    portENTER_CRITICAL(&mux);
    failures_muted = muted;
    portEXIT_CRITICAL(&mux);
}

bool mesh_notify_out_state(uint32_t id, bool failed) {
    portENTER_CRITICAL(&mux);
    bool fresh = mn_failtrack_step(&failtrack, id, failed) && !failures_muted;
    portEXIT_CRITICAL(&mux);
    return fresh;
}

// ---- the open chat -----------------------------------------------------------------------------------------------

static void mark_chat_read(mesh_chat_key_t const *key) {
    char target[MN_TARGET_MAX];
    mn_target(MN_CHANNEL, key, target, sizeof(target));
    lichen_notify_mark_read(meshcore_ctx(), target);
    mn_target(MN_FAILED, key, target, sizeof(target));
    lichen_notify_mark_read(meshcore_ctx(), target);
}

void mesh_notify_set_open_chat(mesh_chat_key_t const *key) {
    mesh_chat_key_t chat = *key; // the chat screen's key is in its PSRAM state (#271)
    portENTER_CRITICAL(&mux);
    open_key = chat;
    open_set = true;
    portEXIT_CRITICAL(&mux);
    mark_chat_read(key);
}

static bool same_key(mesh_chat_key_t const *a, mesh_chat_key_t const *b) {
    return a->kind == b->kind && (a->kind == MESH_CHAT_DM ? memcmp(a->id, b->id, 6) == 0 : strcmp(a->chan, b->chan) == 0);
}

void mesh_notify_clear_open_chat(mesh_chat_key_t const *key) {
    mesh_chat_key_t chat = *key; // as in mesh_notify_set_open_chat
    portENTER_CRITICAL(&mux);
    if (open_set && same_key(&open_key, &chat)) {
        open_set = false;
    }
    portEXIT_CRITICAL(&mux);
}

static bool chat_is_open(mesh_chat_key_t const *key) {
    mesh_chat_key_t chat = *key;
    portENTER_CRITICAL(&mux);
    bool open = open_set && same_key(&open_key, &chat);
    portEXIT_CRITICAL(&mux);
    return open;
}

// ---- the drain -----------------------------------------------------------------------------------------------------

static void remember(mn_note_t const *n, char const *target, mn_out_t out) {
    static char const *const VERDICT[] = {"skip", "silent", "post"};
    char                     line[sizeof(decisions[0])];
    snprintf(line, sizeof(line), "%s %s%s x%u: %s", target, VERDICT[out.verdict], n->mention ? " (mention)" : "",
             (unsigned)n->count, mn_reason_text(out.reason));
    portENTER_CRITICAL(&mux);
    memcpy(decisions[decision_next], line, sizeof(line));
    decision_next = (uint8_t)((decision_next + 1) % DECISIONS);
    if (out.verdict == MN_SKIP) {
        stats.skipped[out.reason]++;
    } else if (out.verdict == MN_SILENT) {
        stats.silent++;
    } else {
        stats.posted++;
    }
    if (n->count > 1) {
        stats.merged += n->count - 1;
    }
    portEXIT_CRITICAL(&mux);
}

static void handle(mn_note_t const *n, mn_switches_t const *s, bool *read) {
    char target[MN_TARGET_MAX];
    mn_target(n->kind, &n->key, target, sizeof(target));
    mn_mode_t mode = n->kind == MN_CHANNEL ? mode_for(&n->key, read) : MN_MODE_ALL;
    mn_out_t  out  = mn_decide(n->kind, mode, s, chat_is_open(&n->key), n->mention, lichen_notify_dnd());
    if (out.verdict != MN_SKIP && !lichen_notify_source_enabled(meshcore_ctx())) {
        out = (mn_out_t){MN_SKIP, MN_R_SOURCE_OFF}; // the centre's own switch for the app, not duplicated here
    }
    remember(n, target, out);
    if (out.verdict == MN_SKIP) {
        return;
    }
    uint32_t flags = LN_COALESCE | (out.verdict == MN_SILENT ? LN_SILENT : 0);
    ln_level_t level = n->kind == MN_FAILED ? LN_WARN : LN_INFO;
    for (unsigned i = 0; i < n->count && i < MAX_POSTS; i++) {
        if (!lichen_notify_to(meshcore_ctx(), level, target, n->title, n->body, flags)) {
            portENTER_CRITICAL(&mux);
            stats.post_refused++;
            portEXIT_CRITICAL(&mux);
            break;
        }
    }
}

// The drain's copy of the queue, off the mesh task's stack (#209: 1.7 KB of a 1.9 KB frame). Static is safe: the mesh
// task is the only caller and there is one at a time (a resume waits for the old task to exit). Internal RAM, not
// PSRAM: mn_queue_take writes it under the spinlock (#271).
static mn_note_t drained[MN_QUEUE_MAX];

bool mesh_notify_drain(void) {
    mn_note_t *notes = drained;
    portENTER_CRITICAL(&mux);
    size_t n = mn_queue_take(&queue, notes, MN_QUEUE_MAX);
    portEXIT_CRITICAL(&mux);
    bool          read = false;
    mn_switches_t s    = switches();
    for (size_t i = 0; i < n; i++) {
        handle(&notes[i], &s, &read);
    }
    portENTER_CRITICAL(&mux);
    bool was_dirty = dirty;
    dirty          = false;
    portEXIT_CRITICAL(&mux);
    return read || was_dirty;
}

// ---- console -------------------------------------------------------------------------------------------------------

static void console_status(void) {
    mn_switches_t s = switches();
    portENTER_CRITICAL(&mux);
    bool            open = open_set;
    mesh_chat_key_t key  = open_key;
    uint32_t        lost = queue.lost;
    typeof(stats)   st   = stats;
    portEXIT_CRITICAL(&mux);
    char opened[MN_TARGET_MAX] = "none";
    if (open) {
        mn_target(MN_CHANNEL, &key, opened, sizeof(opened));
    }
    console_reply("notify: DMs %s, delivery failed %s, mentions %s | OS switch %s | open chat %s", s.dm ? "on" : "off",
                  s.fail ? "on" : "off", s.mentions ? "on" : "off", lichen_notify_source_enabled(meshcore_ctx()) ? "on" : "off",
                  opened);
    console_reply("notify: posted %lu, silent %lu, refused by the service %lu, merged %lu, queue overflow %lu",
                  (unsigned long)st.posted, (unsigned long)st.silent, (unsigned long)st.post_refused,
                  (unsigned long)st.merged, (unsigned long)lost);
    for (int r = 1; r < MN_R_COUNT; r++) {
        if (st.skipped[r]) {
            console_reply("notify: skipped (%s) %lu", mn_reason_text((mn_reason_t)r), (unsigned long)st.skipped[r]);
        }
    }
    for (int i = 0; i < DECISIONS; i++) {
        int idx = (decision_next + i) % DECISIONS;
        if (decisions[idx][0]) {
            console_reply("  last: %s", decisions[idx]);
        }
    }
}

// Same path as a real event (push, drain, decide, post); nothing goes on air
static void console_test(char const *args) {
    static uint8_t const FAKE[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    mesh_chat_key_t      key;
    char                 name[MESH_NAME_MAX + 1];
    if (strcmp(args, "dm") == 0) {
        mesh_chat_key_dm(&key, FAKE);
        mesh_notify_dm(&key, "@Tester", "Tester: test DM from fw mesh notify");
    } else if (strcmp(args, "fail") == 0) {
        mesh_chat_key_dm(&key, FAKE);
        mesh_notify_failed(&key, "@Tester", "test message");
    } else if (strcmp(args, "mention") == 0 && my_name(name, sizeof(name))) {
        char text[96];
        snprintf(text, sizeof(text), "Tester: hello @[%s], test mention", name);
        mesh_chat_key_channel(&key, "Public");
        mesh_notify_channel(&key, "Public", text);
    } else if (strncmp(args, "chan ", 5) == 0 && args[5]) {
        mesh_chat_key_channel(&key, args + 5);
        mesh_notify_channel(&key, args + 5, "Tester: test channel message");
    } else {
        console_reply("mesh notify test dm | chan <name> | fail | mention (MeshCore must run for mention)");
        return;
    }
    console_reply("notify test queued: the mesh task decides within a moment (\"fw mesh notify\", \"fw notify\")");
}

void mesh_notify_console(char const *args) {
    if (!*args) {
        console_status();
    } else if (strncmp(args, "test", 4) == 0 && (args[4] == ' ' || !args[4])) {
        console_test(args[4] ? args + 5 : "");
    } else {
        console_reply("mesh notify | mesh notify test dm|chan <name>|fail|mention");
    }
}
