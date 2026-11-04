// MeshCore on the badge (#61, #65): MeshCore's own companion (examples/companion_radio MyMesh + DataStore) runs in one
// task over the LoRa bridge; the screens and the console are its user interface and read copies taken under
// mesh_lock (no app port: nothing to expose on a network). MyMesh's loop() holds the lock, so its callbacks (the Listener below) already run under it.
#include "mesh_wipe.h"
#include "mesh_rset.h"
#include "mesh_link.h"

#include "lora_radio.h"
#include "identity_move.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_advert_cfg.h"
#include "mesh_chat.h"
#include "mesh_contact_fmt.h"
#include "mesh_frame.h"
#include "mesh_hist_crypto.h"
#include "mesh_ident.h"
#include "mesh_journal.h"
#include "meshcore_run.h"
#include "mesh_rset_run.h"
#include "channel_input.h"
#include "mesh_pathhash.h"
#include "mesh_prefs.h"
#include "mesh_share.h"
#include "radio_presets.h"

#include "SPIFFS.h"

#include <SHA256.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/TxtDataHelpers.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lichen_badge.h"
#include "lichen_fs.h"
#include "lichen_notify.h"
#include "lichen_radio.h"
#include "lichen_settings.h"
#include "mesh_notify_run.h"
#include "lichen_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h" // only the legacy badge_fw/identity hand-over (migrate_identity)

extern "C" {
#include "console_cmd.h" // CONSOLE_WIFI_CMD_MAX
#include "fw_console.h"
#include "lora_app.h"
}

#include <ctype.h>
#include <strings.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STORE_MESSAGES 100 // kept per chat in mesh_store (PSRAM, allocated with the chat's first message)
#define MESH_STACK     12288 // Ed25519 checks are deep and MyMesh keeps File objects on the stack; peak 3.1 KB (#107)
#define MESH_CORE      1     // with LVGL: its 400-650 ms file bursts stay off core 0, where SDIO, tcpip and LoRa live (#91)
#define MESH_POLL_MS   2
#define MESH_SLOW_LOOP_MS 200

static char const TAG[] = "mesh";

// The shim's platform hooks
extern "C" unsigned long millis(void) {
    return (unsigned long)(esp_timer_get_time() / 1000);
}
extern "C" void shim_fill_random(void *buf, size_t len) {
    lichen_random_fill(buf, len);
}
extern "C" void delay(unsigned long ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

extern "C" uint32_t mesh_uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

extern "C" uint32_t mesh_epoch_now(void) {
    return clock_is_set() ? (uint32_t)time(NULL) : 0;
}

class EspRng : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { lichen_random_fill(dest, sz); }
};

// What target.h leaves to the platform
static LoraRadio lora_radio;
BadgeRadio      &radio_driver = lora_radio;
BadgeBoard       board;
BadgeRtc        rtc_clock;
SensorManager    sensors;

uint16_t BadgeBoard::getBattMilliVolts() {
    return 0; // not measured yet
}
bool radio_init() {
    return true;
}
uint32_t radio_get_rng_seed() {
    return lichen_random_u32();
}
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
    radio_driver.setParams(freq, bw, sf, cr);
}
void radio_set_tx_power(uint8_t dbm) {
    radio_driver.setTxPower((int8_t)dbm);
}
// A new key pair; the OS keeps the entropy source on (lichen_random.h)
mesh::LocalIdentity radio_new_identity() {
    EspRng rng;
    mesh::LocalIdentity id(&rng);
    return id;
}

// DataStore::formatFileSystem() (the app's factory reset) erases NVS on ESP32; on the badge that would take the Wi-Fi
// credentials with it, so DataStore.cpp is built with nvs_flash_erase renamed to this: MeshCore's own files go (the
// shim's format()), the rest of NVS stays
extern "C" esp_err_t mesh_nvs_flash_erase(void) {
    ESP_LOGI(TAG, "factory reset from the app: MeshCore's files erased, NVS (Wi-Fi) kept");
    return ESP_OK;
}

static EspRng            rng;
static SimpleMeshTables *tables;
static DataStore         store(SPIFFS, rtc_clock);
static BadgeMesh        *node;
static ob_t             *outbox;            // PSRAM; shared with the UI under ob_mux (mesh_link.h)
static volatile int      offline_queue_len; // MyMesh's offline queue, from the Listener (#55: stays ~0 now)
static portMUX_TYPE      ob_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" void mesh_ob_enter(void) {
    portENTER_CRITICAL(&ob_mux);
}
extern "C" void mesh_ob_exit(void) {
    portEXIT_CRITICAL(&ob_mux);
}
extern "C" void mesh_advert_limited(uint32_t refused) {
    ESP_LOGW(TAG, "adverts from new nodes rate-limited (%lu dropped since boot): an advert flood?",
             (unsigned long)refused);
}
static SemaphoreHandle_t mesh_lock_handle(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&buf);
    return handle;
}
static void mesh_lock(void) {
    xSemaphoreTake(mesh_lock_handle(), portMAX_DELAY);
}
static bool mesh_lock_within(uint32_t wait_ms) {
    return xSemaphoreTake(mesh_lock_handle(), wait_ms == MESH_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) ==
           pdTRUE;
}
static void mesh_unlock(void) {
    xSemaphoreGive(mesh_lock_handle());
}

// Running or not (#103). A stop keeps node, its tables, the outbox and the store in PSRAM: a start after a stop on
// request only claims the radio again and makes a new task, so nothing is loaded or replayed twice.
static TaskHandle_t mesh_task_handle; // valid only while mesh_running()
static bool stop_requested; // mesh_stop -> the task
static bool stop_final;     // with it: the update stop (the journal settles and wipes its key)
static bool task_running;   // set once the first task exists
static bool stopped;        // the task -> everyone: it has saved and is gone (or is about to delete itself)
static bool final_stop;     // stopped for an update: no start again before the restart
static bool task_exited = true; // the task -> mesh_stop / mesh_start: radio released, about to delete itself

static bool mesh_is_stopped(void) {
    return __atomic_load_n(&stopped, __ATOMIC_ACQUIRE);
}

// Why a stopped MeshCore refuses, for the setters and senders
static char const *stopped_why(void) {
    return __atomic_load_n(&final_stop, __ATOMIC_ACQUIRE) ? "MeshCore stopped for update" : "MeshCore is not running";
}

// The stored identity's check at load (#80, ident_verify): only a passed check transmits
static mesh_ident_t ident_state;

static bool ident_tx_allowed(void) {
    return mesh_ident_tx_allowed(__atomic_load_n(&ident_state, __ATOMIC_ACQUIRE));
}

// mesh_start and mesh_stop one at a time: the update path, a job task and the boot never overlap
static SemaphoreHandle_t run_lock_handle(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&buf);
    return handle;
}

// The home tile's numbers (#67): written by the mesh task, read by the LVGL task, under a spinlock of their own so the
// tile never waits on MeshCore
static mesh_tile_t  tile;
static uint32_t     tile_heard_s; // uptime of the newest message, 0 for none
static uint32_t     tile_advert_s; // uptime + 1 of our last advert, 0 for none (+1: one sent in the first second counts)
static portMUX_TYPE tile_mux = portMUX_INITIALIZER_UNLOCKED;

// The periodic advert (#52), under tile_mux: the settings' copy, when the last went out and when the next is due
// (Unix seconds, 0 for none). Stored in the app's settings (adv_load): the schedule survives restarts, so a badge that
// is on for an hour a day still advertises once per interval and a reboot loop never floods.
static struct {
    uint32_t interval_min = ADVCFG_INTERVAL_DEFAULT;
    bool     flood_manual = false; // the manual Advertise: zero-hop unless set
    uint32_t last_epoch   = 0;
    uint32_t next_epoch   = 0;
    uint32_t last_uptime  = 0; // uptime + 1 of the last advert of ours, for the minimum gap
} adv;

static void advert_tick_locked(void);
static void discover_note_locked(mesh::Packet const *pkt); // #167
static void discover_tick_locked(void);
static void contacts_full_post(void);

// The unread total over all chats, after the store changed (called under the mesh lock)
static void tile_publish_unread(void) {
    uint32_t total = mesh_store_unread_where(mesh_notify_counts_unread, NULL); // the number the badge and tile share (#50)
    portENTER_CRITICAL(&tile_mux);
    tile.unread_total = total;
    portEXIT_CRITICAL(&tile_mux);
    // home's count on the icon (#102); its own lock, not inside ours. 0 while stopped (#103): nothing arrives then
    lichen_badge_set("meshcore", mesh_is_stopped() ? 0 : total);
}

static uint32_t tile_unread(void) {
    portENTER_CRITICAL(&tile_mux);
    uint32_t total = tile.unread_total;
    portEXIT_CRITICAL(&tile_mux);
    return total;
}

static void tile_on_message(char const *channel, char const *text) {
    mesh_tile_t m;
    mesh_tile_on_message(&m, channel, text); // the formatting outside the spinlock
    uint32_t total = mesh_store_unread_where(mesh_notify_counts_unread, NULL);
    portENTER_CRITICAL(&tile_mux);
    memcpy(tile.latest, m.latest, sizeof(tile.latest));
    tile.unread_total = total;
    tile_heard_s      = mesh_uptime_s();
    portEXIT_CRITICAL(&tile_mux);
    lichen_badge_set("meshcore", total);
}

// Boot (under the mesh lock): the newest message from the history journal as the tile's latest line
static void tile_seed_latest(void) {
    mesh_msg_t m;
    if (mesh_store_recent(&m, 1) != 1) {
        return;
    }
    mesh_tile_t t;
    mesh_tile_on_message(&t, m.channel, m.text); // the formatting outside the spinlock
    portENTER_CRITICAL(&tile_mux);
    memcpy(tile.latest, t.latest, sizeof(tile.latest));
    portEXIT_CRITICAL(&tile_mux);
}

static void tile_set_contacts(size_t contacts) {
    portENTER_CRITICAL(&tile_mux);
    tile.contacts = contacts;
    portEXIT_CRITICAL(&tile_mux);
}

extern "C" void mesh_tile_snapshot(mesh_tile_t *out) {
    portENTER_CRITICAL(&tile_mux);
    *out            = tile;
    uint32_t heard  = tile_heard_s;
    uint32_t advert = tile_advert_s;
    portEXIT_CRITICAL(&tile_mux);
    uint32_t now          = mesh_uptime_s();
    out->last_heard_age_s = heard ? now - heard : 0;
    out->advert_sent      = advert != 0;
    out->advert_age_s     = advert ? now + 1 - advert : 0;
    portENTER_CRITICAL(&tile_mux);
    uint32_t next = adv.next_epoch;
    portEXIT_CRITICAL(&tile_mux);
    uint32_t in_s = advcfg_next_in_s(next, mesh_epoch_now());
    out->advert_scheduled = in_s != UINT32_MAX;
    out->advert_next_s    = in_s == UINT32_MAX ? 0 : in_s;
}

// The chat rings go to PSRAM explicitly: through plain malloc a ring under SPIRAM_MALLOC_ALWAYSINTERNAL would take
// internal RAM, which the display needs (L6 of #59)
static void *store_alloc_ring(size_t count, size_t size) {
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
}

static void store_warn(char const *what) {
    ESP_LOGW(TAG, "message dropped: %s", what);
}

// Messages for the screens, from MyMesh (called inside loop(), under the lock)
class ScreenFeed : public MyMesh::Listener {
public:
    void onMessageRecv(mesh::Packet *pkt, ContactInfo const &from, uint8_t txt_type, uint32_t sender_timestamp,
                       char const *text) override {
        if (txt_type == MF_TXT_CLI_DATA) {
            return; // a repeater's CLI reply belongs to the CLI screen (mesh_remote.h), not to a chat (#51)
        }
        char from_name[sizeof(from.name) + 2]; // mesh_store cuts it to MESH_CHANNEL_MAX
        snprintf(from_name, sizeof(from_name), "@%s", from.name);
        // The tile follows the store: a message the store dropped (and logged) is not shown as the latest either
        if (mesh_store_add_dm(from_name, text, (int16_t)(radio_driver.getLastRSSI() * 10.0f), mesh_uptime_s(),
                              from.id.pub_key)) {
            tile_on_message(from_name, text);
            mesh_chat_key_t key;
            mesh_chat_key_dm(&key, from.id.pub_key);
            mesh_journal_incoming_locked(&key);
            mesh_notify_dm(&key, from_name, text); // queued; posted by the task after the unlock (#50)
        }
    }
    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &channel, char const *text) override {
        if (mesh_store_add_message(channel.name, text, (int16_t)(radio_driver.getLastRSSI() * 10.0f), mesh_uptime_s())) {
            tile_on_message(channel.name, text);
            mesh_chat_key_t key;
            mesh_chat_key_channel(&key, channel.name);
            mesh_journal_incoming_locked(&key);
            mesh_notify_channel(&key, channel.name, text);
        }
    }
    void onQueueSizeChanged(int msgcount) override { offline_queue_len = msgcount; }
    void onControlDataRecv(mesh::Packet const *pkt) override { discover_note_locked(pkt); }
};
static ScreenFeed screen_feed;

extern "C" size_t mesh_messages_snapshot(mesh_msg_t *out, size_t max, uint32_t *generation, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n    = mesh_store_recent(out, max);
    *generation = mesh_store_view_generation();
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_chat_snapshot(mesh_chat_key_t const *key, mesh_msg_t *out, size_t max, uint32_t *generation,
                                     bool mark_read, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n    = mesh_store_chat(key, out, max, mark_read);
    *generation = mesh_store_view_generation();
    if (mark_read) {
        tile_publish_unread();
    }
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_convs_snapshot(mesh_conv_t *out, size_t max, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n = mesh_store_convs(out, max);
    for (size_t i = 0; i < n; i++) { // a removed contact's DM history stays, marked (#101)
        out[i].contact_gone = out[i].key.kind == MESH_CHAT_DM && node && !node->lookupContactByPubKey(out[i].key.id, 6);
    }
    mesh_unlock();
    return n;
}

// Keeps out[0..*n) in the view's order, dropping the last once max are in
static void contact_insert(mesh_contact_view_t view, mesh_contact_t const *entry, mesh_contact_t *out, size_t *n,
                           size_t max) {
    size_t pos = *n;
    while (pos > 0 && mesh_contact_view_before(view, entry->flags, entry->age_s, out[pos - 1].flags, out[pos - 1].age_s)) {
        pos--;
    }
    if (pos >= max) {
        return;
    }
    size_t move = (*n < max ? *n : max - 1) - pos;
    memmove(&out[pos + 1], &out[pos], move * sizeof(*out));
    out[pos] = *entry;
    *n       = *n < max ? *n + 1 : max;
}

extern "C" size_t mesh_contacts_view(mesh_contact_view_t view, mesh_contact_t *out, size_t max, size_t *total,
                                     uint32_t wait_ms) {
    size_t n = 0;
    *total   = 0;
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    uint32_t         now = rtc_clock.getCurrentTime();
    ContactsIterator it  = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        if (!mesh_contact_view_keeps(view, c.type)) {
            continue;
        }
        (*total)++;
        if (max == 0) {
            continue; // only counting
        }
        mesh_contact_t entry;
        memcpy(entry.pubkey, c.id.pub_key, sizeof(entry.pubkey));
        mesh_sanitize(c.name, strnlen(c.name, sizeof(c.name)), entry.name, sizeof(entry.name));
        entry.type     = c.type;
        entry.flags    = c.flags;
        entry.path_len = c.out_path_len;
        entry.age_s    = now > c.lastmod ? now - c.lastmod : 0;
        contact_insert(view, &entry, out, &n, max);
    }
    mesh_unlock();
    return n;
}

extern "C" bool mesh_contacts_stats(mesh_contact_stats_t *out, uint32_t wait_ms) {
    mesh_contact_stats_t st = {};
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        *out = st;
        return true;
    }
    if (!mesh_lock_within(wait_ms)) {
        return false;
    }
    uint32_t         now = rtc_clock.getCurrentTime();
    ContactsIterator it  = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        uint32_t age = now > c.lastmod ? now - c.lastmod : 0;
        st.total++;
        st.repeaters += mesh_contact_view_keeps(MESH_VIEW_REPEATERS, c.type) ? 1 : 0;
        st.newest_age_s = !st.heard_any || age < st.newest_age_s ? age : st.newest_age_s;
        st.heard_any    = true;
    }
    mesh_unlock();
    *out = st;
    return true;
}

extern "C" size_t mesh_contacts_snapshot(mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms) {
    if (max == 0) {
        *total = 0;
        return 0; // as before: nothing asked, nothing counted
    }
    return mesh_contacts_view(MESH_VIEW_ALL, out, max, total, wait_ms);
}

// ---- one contact: the details screen (#49) ---------------------------------------------------------------------------

extern "C" size_t mesh_contact_detail(uint8_t const prefix[6], mesh_contact_detail_t *out, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY; // not up (yet), or stopped for an update: not a sign the contact is gone
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    ContactInfo const *c = node->lookupContactByPubKey(prefix, MF_PREFIX);
    if (c) {
        uint32_t now = rtc_clock.getCurrentTime();
        memcpy(out->pub, c->id.pub_key, sizeof(out->pub));
        mesh_sanitize(c->name, strnlen(c->name, sizeof(c->name)), out->name, sizeof(out->name));
        out->type     = c->type;
        out->flags    = c->flags;
        out->path_len = c->out_path_len;
        memcpy(out->path, c->out_path, sizeof(out->path));
        out->age_s  = now > c->lastmod ? now - c->lastmod : 0;
        out->lat_e6 = c->gps_lat;
        out->lon_e6 = c->gps_lon;
    }
    mesh_unlock();
    return c ? 1 : 0;
}

extern "C" size_t mesh_contact_do(uint8_t const prefix[6], mesh_contact_op_t op, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY; // as mesh_contact_detail
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    BadgeMesh::ContactResult r = node->contactAction(prefix, op);
    mesh_unlock();
    return r == BadgeMesh::CONTACT_QUEUED ? 1 : r == BadgeMesh::CONTACT_GONE ? 0 : MESH_BUSY;
}

extern "C" size_t mesh_contact_set_path(uint8_t const prefix[6], uint8_t path_len,
                                        uint8_t const path[MESH_CONTACT_PATH_MAX], uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    BadgeMesh::ContactResult r = node->contactSetPath(prefix, path_len, path);
    mesh_unlock();
    return r == BadgeMesh::CONTACT_QUEUED ? 1 : r == BadgeMesh::CONTACT_GONE ? 0 : MESH_BUSY;
}

// MeshCore writes contacts lazily; a firmware update must not lose the last few minutes of them
static bool keep_contact(ContactInfo const &c) {
    return c.type != ADV_TYPE_NONE; // as MyMesh's own filter: not the transient entries
}

// MeshCore's own reboot (an admin's "reboot" CLI command, the companion protocol) runs on the mesh task inside
// node->loop(), with the mesh lock held. The OS's restart save (system_restart -> stop_for_update, #123) would wait for
// this very task to stop, so the saves a stop makes are made here first, and stop_locked then has nothing to wait for.
// The mesh task -> stop_locked: saved for MeshCore's own reboot. Never cleared: system_restart does not return.
static bool rebooting;

void BadgeBoard::reboot() {
    if (mesh_task_handle && xTaskGetCurrentTaskHandle() == mesh_task_handle) {
        store.savePrefs(*node->getNodePrefs());
        store.saveContacts(node, keep_contact);
        store.saveChannels(node);
        mesh_journal_stop_locked(true);
        __atomic_store_n(&rebooting, true, __ATOMIC_RELEASE);
    }
    system_restart("MeshCore reboot");
}

// Our channel messages go into the screens' store as "me: <text>", and their outbox state follows (#46). With the mesh
// lock held (the store is under it); the outbox is read in short spinlock sections, the copies are made outside them.
struct OutSync {
    mesh_msg_id_t id;
    uint8_t       state, repeats, attempt, kind, prefix[MF_PREFIX];
    uint32_t      trip_ms;
    bool          fresh;
    char          target[OB_TARGET_MAX];
    char          text[OB_TEXT_MAX];
};
static OutSync out_sync[OB_CAP];

// An own message has just ended FAILED (#50): its target and text are read again from the outbox, as only fresh
// entries are copied above
static void notify_failed(OutSync const &o) {
    char       target[OB_TARGET_MAX] = "";
    char       text[OB_TEXT_MAX]     = "";
    uint8_t    kind                  = OB_CHANNEL;
    uint8_t    prefix[MF_PREFIX]     = {0};
    mesh_ob_enter();
    for (int i = 0; i < OB_CAP && outbox; i++) {
        ob_entry_t const *e = &outbox->e[i];
        if (e->used && e->id == o.id) {
            memcpy(target, e->target, sizeof(target));
            memcpy(text, e->text, sizeof(text));
            memcpy(prefix, e->prefix, sizeof(prefix));
            kind = e->kind;
        }
    }
    mesh_ob_exit();
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 2];
    if (kind == OB_DM) {
        ContactInfo *c = node ? node->lookupContactByPubKey(prefix, MF_PREFIX) : nullptr;
        snprintf(title, sizeof(title), "@%s", c ? c->name : "?");
        mesh_chat_key_dm(&key, prefix);
    } else {
        snprintf(title, sizeof(title), "%s", target);
        mesh_chat_key_channel(&key, target);
    }
    mesh_notify_failed(&key, title, text);
}

// true when a message was added to the store (which may have evicted a chat, so the unread total can have changed)
static bool sync_outgoing(void) {
    size_t n = 0;
    mesh_ob_enter();
    for (int i = 0; i < OB_CAP && outbox; i++) {
        ob_entry_t *e = &outbox->e[i];
        if (!e->used) {
            continue;
        }
        OutSync &o = out_sync[n++];
        o.id       = e->id;
        o.state    = e->state;
        o.repeats  = e->repeats;
        o.attempt  = e->attempt;
        o.kind     = e->kind;
        o.trip_ms  = e->trip_ms;
        memcpy(o.prefix, e->prefix, MF_PREFIX);
        o.fresh    = !e->in_store;
        if (o.fresh) {
            memcpy(o.target, e->target, sizeof(o.target));
            memcpy(o.text, e->text, sizeof(o.text));
        }
    }
    mesh_ob_exit();
    bool added = false;
    for (size_t i = 0; i < n; i++) {
        OutSync &o = out_sync[i];
        if (o.fresh) {
            char line[MESH_TEXT_MAX + 1];
            snprintf(line, sizeof(line), "me: %s", o.text);
            bool            ok;
            mesh_chat_key_t key;
            if (o.kind == OB_DM) {
                ContactInfo *c = node ? node->lookupContactByPubKey(o.prefix, MF_PREFIX) : nullptr;
                snprintf(o.target, sizeof(o.target), "@%s", c ? c->name : "?");
                ok = mesh_store_add_outgoing_dm(o.target, line, o.id, mesh_uptime_s(), o.prefix);
                mesh_chat_key_dm(&key, o.prefix);
            } else {
                ok = mesh_store_add_outgoing(o.target, line, o.id, mesh_uptime_s());
                mesh_chat_key_channel(&key, o.target);
            }
            if (ok) {
                mesh_journal_outgoing_locked(&key); // held until its state is final
            }
            // in_store only after the store took it: a dropped one is tried again on the next pass (logged once)
            static mesh_msg_id_t failed_logged;
            if (ok) {
                added = true;
                mesh_ob_enter();
                for (int k = 0; k < OB_CAP && outbox; k++) {
                    if (outbox->e[k].used && outbox->e[k].id == o.id) {
                        outbox->e[k].in_store = true;
                    }
                }
                mesh_ob_exit();
            } else if (failed_logged != o.id) {
                failed_logged = o.id;
                ESP_LOGW(TAG, "outgoing message %lu not in the store, will retry", (unsigned long)o.id);
            }
        }
        mesh_store_update_out(o.id, o.state, o.repeats, o.attempt, o.trip_ms);
        if (mesh_notify_out_state(o.id, o.state == MESH_OUT_FAILED)) {
            notify_failed(o);
        }
        mesh_journal_out_state_locked(o.id, o.state, o.repeats, o.attempt, o.trip_ms);
    }
    uint32_t ids[OB_CAP];
    for (size_t i = 0; i < n; i++) {
        ids[i] = out_sync[i].id;
    }
    mesh_journal_out_sweep_locked(ids, n);
    return added;
}

static void mesh_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t asked = esp_timer_get_time();
        mesh_lock();
        int64_t  started = esp_timer_get_time();
        uint32_t rx      = radio_driver.getPacketsRecv();
        fs_shim_take_busy_us(NULL); // what other tasks spent in files under the lock is not this loop's
        node->loop();
        if (sync_outgoing()) {
            tile_publish_unread(); // an added message can evict a DM chat that had unread
        }
        uint32_t file_ops;
        uint32_t file_us = fs_shim_take_busy_us(&file_ops);
        rx               = radio_driver.getPacketsRecv() - rx;
        tile_set_contacts((size_t)node->getNumContacts());
        advert_tick_locked();
        discover_tick_locked();
        mesh_journal_tick_locked(); // a due flush swaps the pending records out; they are written after the unlock
        if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
            // The task stops itself, between two loops and with the lock held: nothing is half done, and the stack
            // (16 KB, the saves keep File objects on it) is the one that is big enough
            bool final = __atomic_load_n(&stop_final, __ATOMIC_ACQUIRE);
            if (!final) {
                // A stop on request (#103): nothing waiting goes out late after the next start; the journal gets the
                // failures through sync_outgoing like any other state change
                mesh_ob_enter();
                size_t failed = ob_fail_pending(outbox);
                mesh_ob_exit();
                int dropped = node->dropOutbound();
                mesh_notify_mute_failures(true); // the user asked for this stop: no "not delivered" notices
                sync_outgoing();
                mesh_notify_mute_failures(false);
                ESP_LOGI(
                    TAG,
                    "stop: %u outgoing message(s) failed, %d queued packet(s) dropped",
                    (unsigned)failed,
                    dropped
                );
            }
            int64_t save_started = esp_timer_get_time();
            store.savePrefs(*node->getNodePrefs());
            int64_t contacts_started = esp_timer_get_time();
            store.saveContacts(node, keep_contact);
            int64_t contacts_us = esp_timer_get_time() - contacts_started;
            store.saveChannels(node);
            int64_t save_us = esp_timer_get_time() - save_started;
            mesh_journal_stop_locked(final);
            __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
            mesh_unlock();
            // Here, not in mesh_stop: a stop that outlasts mesh_stop's wait still gives the radio back (#103)
            lora_radio.end();
            lichen_badge_set("meshcore", 0);
            // The contact rewrite is the same one MyMesh does lazily in its loop: its time here says what it costs there
            ESP_LOGI(
                TAG,
                "MeshCore stopped%s: prefs, %d contacts, channels saved in %lld ms (contacts %lld ms)",
                final ? " for the update" : "",
                node->getNumContacts(),
                (long long)(save_us / 1000),
                (long long)(contacts_us / 1000)
            );
            __atomic_store_n(&task_exited, true, __ATOMIC_RELEASE);
            vTaskDelete(NULL);
        }
        int64_t took_ms = (esp_timer_get_time() - started) / 1000;
        mesh_unlock();
        mesh_journal_flush(); // the card's I/O never under the mesh lock: the screens keep their snapshots meanwhile
        contacts_full_post();
        if (mesh_notify_drain()) { // #50: notices posted after the unlock; a mode read or changed moves the badge
            mesh_lock();
            tile_publish_unread();
            mesh_unlock();
        }
        if (took_ms >= MESH_SLOW_LOOP_MS) {
            // Where it went (#67): file time is DataStore's (the lazy /contacts3 rewrite), the rest is MeshCore's own
            // work on the packets received (Ed25519 checks of adverts, decryption); the screens skip ticks meanwhile
            ESP_LOGW(TAG, "MeshCore loop took %lld ms: files %lu ms (%lu calls), %lu packet(s) in, waited %lld ms for "
                          "the lock",
                     (long long)took_ms, (unsigned long)(file_us / 1000), (unsigned long)file_ops, (unsigned long)rx,
                     (long long)((started - asked) / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_POLL_MS));
    }
}

extern "C" bool mesh_running(void) {
    return __atomic_load_n(&task_running, __ATOMIC_ACQUIRE) && !mesh_is_stopped();
}

// Under the run lock
static bool stop_locked(mesh_stop_reason_t why) {
    bool final = why == MESH_STOP_UPDATE;
    if (__atomic_load_n(&rebooting, __ATOMIC_ACQUIRE)) {
        // MeshCore's own reboot saved on its task, which holds the mesh lock until the restart: nothing to wait for
        __atomic_store_n(&final_stop, true, __ATOMIC_RELEASE);
        return true;
    }
    if (!mesh_running() && !mesh_stop_pending()) {
        // Never started, or stopped on request before: an update still settles the journal (held messages, the key)
        if (final && __atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
            mesh_lock();
            mesh_journal_stop_locked(true);
            mesh_unlock();
        }
        if (final) {
            __atomic_store_n(&final_stop, true, __ATOMIC_RELEASE);
        }
        return true;
    }
    if (final) {
        __atomic_store_n(&final_stop, true, __ATOMIC_RELEASE); // no start slips in after this
    }
    __atomic_store_n(&stop_final, final, __ATOMIC_RELEASE);
    __atomic_store_n(&stop_requested, true, __ATOMIC_RELEASE);
    // A loop can sit in a file save for a second or two; the saves here the same again
    for (int i = 0; i < 1000 && !__atomic_load_n(&task_exited, __ATOMIC_ACQUIRE); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!__atomic_load_n(&task_exited, __ATOMIC_ACQUIRE)) {
        ESP_LOGE(TAG, "MeshCore did not stop in 10 s");
        return false;
    }
    if (final) {
        // The task read stop_final when it stopped: after a stop on request that outlasted its wait it may have stopped
        // as one. Settling again is a no-op when it was already final.
        mesh_lock();
        mesh_journal_stop_locked(true);
        mesh_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // the task releases the radio and deletes itself right after giving the lock back
    return true;
}

extern "C" bool mesh_stop_pending(void) {
    return __atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE) && !__atomic_load_n(&task_exited, __ATOMIC_ACQUIRE);
}

extern "C" bool mesh_stop(mesh_stop_reason_t why) {
    // The update must not wait forever behind a start: it gives up after 15 s, as a stop that did not end in time
    if (xSemaphoreTake(run_lock_handle(), pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "MeshCore busy starting or stopping: not stopped");
        return false;
    }
    bool ok = stop_locked(why);
    xSemaphoreGive(run_lock_handle());
    return ok;
}

// The stored identity's check (#80): once, at the first load. A mismatch, or a file MyMesh could not read (#109),
// blocks every transmit until a restart, at the senders (nothing is queued to fail later) and at the radio (MeshCore's
// own ACKs, paths and adverts). The file is never rewritten or regenerated here: it may be the only copy of the identity.
static void ident_apply(mesh_ident_t state, bool simulated) {
    __atomic_store_n(&ident_state, state, __ATOMIC_RELEASE);
    __atomic_store_n(&lora_radio.tx_blocked, !mesh_ident_tx_allowed(state), __ATOMIC_RELEASE);
    if (mesh_ident_tx_allowed(state)) {
        return;
    }
    bool unreadable = state == MESH_IDENT_UNREADABLE;
    ESP_LOGE(TAG, "MeshCore %s%s; TX blocked, file kept as it is",
             unreadable ? "identity file unreadable (#109): no new identity made"
                        : "identity MISMATCH: the stored public key is not the private key's",
             simulated ? " (simulated)" : "");
    // an error notice: toasted, kept in the history across restarts until read
    lichen_notify("meshcore", LN_ERROR,
                  simulated    ? "MeshCore identity (test)"
                  : unreadable ? "MeshCore identity unreadable"
                               : "MeshCore identity damaged",
                  unreadable ? "The identity file could not be read: MeshCore will not transmit. The file is kept; "
                               "fw mesh shows it."
                             : "The stored keys do not match: MeshCore will not transmit. The file is kept; fw mesh "
                               "shows it.",
                  LN_ONCE_BOOT);
}

extern "C" mesh_ident_t mesh_identity_state(void) {
    return __atomic_load_n(&ident_state, __ATOMIC_ACQUIRE);
}

extern "C" void mesh_identity_no_files(void) {
    mesh_ident_t unchecked = MESH_IDENT_UNCHECKED;
    if (__atomic_compare_exchange_n(&ident_state, &unchecked, MESH_IDENT_UNREADABLE, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&lora_radio.tx_blocked, true, __ATOMIC_RELEASE);
        // the OS raises its own notice for the drive (sys_notices.c): no second one here
        ESP_LOGE(TAG, "MeshCore's files are not reachable (no storage drive?): identity unreadable, no new one made");
    }
}

static void ident_verify(BadgeMesh *n) {
    uint8_t      prv[PRV_KEY_SIZE];
    mesh_ident_t state = MESH_IDENT_MISMATCH; // a key that can't be read is not a sound one either
    if (n->self_id.writeTo(prv, sizeof(prv)) == PRV_KEY_SIZE) {
        state = mesh_ident_after_load(n->identityUnreadable(), prv, n->self_id.pub_key);
    }
    mh_wipe(prv, sizeof(prv));
    ident_apply(state, false);
}

// Milestone 2 kept the key pair in NVS; MeshCore's DataStore keeps it in /storage/meshcore. Moved once, so the
// contacts that already know the badge keep knowing it.
static void migrate_identity(void) {
    nvs_handle_t nvs;
    uint8_t      blob[128];
    size_t       len = sizeof(blob);
    // A file that is there but unreadable (#109) is kept too: never written over, not even with the NVS copy
    if (store.hasMainIdentity() || nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(nvs, "identity", blob, &len) == ESP_OK && len >= PRV_KEY_SIZE + PUB_KEY_SIZE) {
        mesh::LocalIdentity id;
        id.readFrom(blob, len);
        if (identity_move(store, id)) {
            nvs_erase_key(nvs, "identity");
            nvs_commit(nvs);
            ESP_LOGI(TAG, "MeshCore identity moved from NVS to /storage/meshcore");
        } else {
            ESP_LOGE(TAG, "MeshCore identity not saved to /storage/meshcore (full?): kept in NVS, tried again next boot");
        }
    }
    nvs_close(nvs);
}

// First boot of MyMesh (no prefs file yet): the radio settings come from lora.toml instead of MeshCore's defaults
static void apply_boot_radio(MyMesh *n, bool first_boot) {
    NodePrefs *prefs = n->getNodePrefs();
    if (first_boot) {
        uint32_t freq_hz, bw_hz;
        uint8_t  sf, cr;
        lora_get_boot_radio(&freq_hz, &bw_hz, &sf, &cr);
        prefs->freq = freq_hz / 1e6f;
        prefs->bw   = bw_hz / 1e3f;
        prefs->sf   = sf;
        prefs->cr   = cr;
        n->savePrefs();
    }
    radio_driver.setParams(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
}

// ---- settings (#67) ------------------------------------------------------------------------------------------------

static mesh_settings_t settings_cache; // what the setters left, for screens that must not wait on the mesh lock
static bool            settings_known;
// Location and auto-add (#52), refreshed with settings_cache; interval, manual mode and next time are in `adv`
static struct {
    double           lat, lon;
    bool             share;
    advcfg_autoadd_t autoadd;
} acfg_cache;
static portMUX_TYPE    settings_mux = portMUX_INITIALIZER_UNLOCKED;

static void radio_from_prefs(NodePrefs const *prefs, mesh_radio_t *out);
static mesh_radio_t radio_cache; // taken at start, refreshed by mesh_radio_params and the setters

// Under the mesh lock: the copies the screens read
static void settings_from_node(MyMesh *n) {
    NodePrefs const *prefs = n->getNodePrefs();
    mesh_settings_t  s     = {};
    snprintf(s.name, sizeof(s.name), "%s", prefs->node_name);
    s.freq           = prefs->freq;
    s.bw             = prefs->bw;
    s.sf             = prefs->sf;
    s.cr             = prefs->cr;
    s.path_hash_size = (uint8_t)(prefs->path_hash_mode + 1);
    s.tx_dbm         = prefs->tx_power_dbm;
    snprintf(s.scope, sizeof(s.scope), "%s", prefs->default_scope_name);
    radio_from_prefs(prefs, &radio_cache);
    portENTER_CRITICAL(&settings_mux);
    settings_cache = s;
    acfg_cache.lat   = sensors.node_lat; // what an advert carries, and what savePrefs stores
    acfg_cache.lon   = sensors.node_lon;
    acfg_cache.share = prefs->advert_loc_policy != ADVERT_LOC_NONE;
    mesh_prefs_get_autoadd(prefs, &acfg_cache.autoadd);
    settings_known = true;
    portEXIT_CRITICAL(&settings_mux);
}

extern "C" bool mesh_get_settings(mesh_settings_t *out) {
    portENTER_CRITICAL(&settings_mux);
    bool known = settings_known;
    *out       = settings_cache;
    portEXIT_CRITICAL(&settings_mux);
    if (!known) {
        memset(out, 0, sizeof(*out));
    }
    return known;
}

// The lock for a setter, or why there is none (then the lock is not held)
static char const *settings_lock(void) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        return "MeshCore is not running";
    }
    if (!mesh_lock_within(MESH_SET_WAIT_MS)) {
        return "MeshCore is busy, try again";
    }
    if (mesh_is_stopped()) { // stopped (and saved) while we waited
        mesh_unlock();
        return stopped_why();
    }
    return NULL;
}

extern "C" bool mesh_set_name(char const *name) {
    char clean[sizeof(((NodePrefs *)0)->node_name)];
    if (!mesh_name_clean(name, clean, sizeof(clean))) {
        return false;
    }
    char const *why = settings_lock();
    if (why) {
        ESP_LOGW(TAG, "name not set: %s", why);
        return false;
    }
    bool ok = mesh_prefs_set_name(node->getNodePrefs(), clean);
    if (ok) {
        node->savePrefs(); // others see it in the next advert (MyMesh builds each advert from the prefs)
        settings_from_node(node);
    }
    mesh_unlock();
    return ok;
}

extern "C" char const *mesh_set_radio(float freq, float bw, uint8_t sf, uint8_t cr, int8_t tx_dbm) {
    char const *why = radio_check(freq, bw, sf, cr, tx_dbm);
    if (why) {
        return why; // checked before the lock: a refusal never waits on MeshCore
    }
    why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_radio(node->getNodePrefs(), freq, bw, sf, cr, tx_dbm);
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
    }
    mesh_unlock();
    if (!why) {
        ESP_LOGI(
            TAG,
            "radio set: %.3f MHz, %.2f kHz, SF%u, CR%u, %d dBm",
            freq,
            bw,
            sf,
            cr,
            tx_dbm
        );
    }
    return why;
}

extern "C" char const *mesh_set_scope(char const *name) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_scope(node->getNodePrefs(), name);
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
        ESP_LOGI(TAG, "default flood scope: %s", node->getNodePrefs()->default_scope_name[0] ? "set" : "none");
    }
    mesh_unlock();
    return why;
}

extern "C" char const *mesh_set_path_hash(uint8_t size) {
    if (size < 1 || size > 3) {
        return "Path hash size must be 1 to 3 bytes"; // before the lock: a refusal never waits on MeshCore
    }
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_path_hash(node->getNodePrefs(), size);
    int forgot = 0;
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
        forgot = node->forgetStaleRoutes(size); // routes of the old size would be read wrong by the repeaters
        if (forgot) {
            store.saveContacts(node, keep_contact);
        }
    }
    mesh_unlock();
    if (!why) {
        ESP_LOGI(TAG, "path hash size set: %u B (forgot %d stored routes of another size)", size, forgot);
    }
    return why;
}

// The first boot of the build that made the size a setting (#129): see mesh_pathhash_migrate. Before the mesh task runs.
static void migrate_path_hash(MyMesh *n) {
    NodePrefs *prefs = n->getNodePrefs();
    bool       mark;
    uint8_t    to = mesh_pathhash_migrate(meshcore_pathhash_migrated(), (uint8_t)(prefs->path_hash_mode + 1), &mark);
    if (to) {
        mesh_prefs_set_path_hash(prefs, to);
        n->savePrefs();
        ESP_LOGI(TAG, "path hash size moved from 1 B to %u B (one-time default change)", to);
    }
    if (mark && !meshcore_pathhash_migrated_set()) {
        ESP_LOGW(TAG, "path hash migration flag not saved: it runs again next boot");
    }
}

// Once (#130): the routes learned with 1 B hashes before the 2 B default (#129) are forgotten, so the next DM floods and
// learns a route of the current size. Before the pipe and the task run: nothing can transmit meanwhile.
static void forget_stale_routes_once(BadgeMesh *n) {
    if (meshcore_routes_forgotten()) {
        return;
    }
    int forgot = n->forgetStaleRoutes((uint8_t)(n->getNodePrefs()->path_hash_mode + 1));
    if (forgot) {
        store.saveContacts(n, keep_contact);
    }
    ESP_LOGI(TAG, "forgot %d stored routes with other hash sizes", forgot);
    if (!meshcore_routes_forgotten_set()) {
        ESP_LOGW(TAG, "route cleanup flag not saved: it runs again next boot");
    }
}

// MeshCore's hashtag channels: the key is the first 16 bytes of SHA-256 of the name, '#' included, lowercase
static size_t hashtag_secret(char const *name, uint8_t out[CHANNEL_SECRET_MAX]) {
    uint8_t digest[32];
    SHA256  sha;
    sha.update(name, strlen(name));
    sha.finalize(digest, sizeof(digest));
    memcpy(out, digest, 16);
    return 16;
}

extern "C" char const *mesh_channel_add(char const *name_in, char const *b64_key_or_null) {
    ChannelDetails details = {};
    char           name[MESH_CHANNEL_MAX + 1];
    size_t         key_len;
    // The input rules (and the key's bounds, #65 review) are channel_input.c's, host-tested
    char const *why = channel_parse(name_in, b64_key_or_null, name, sizeof(name), details.channel.secret, &key_len);
    if (why) {
        return why;
    }
    if (key_len == 0) {
        key_len = hashtag_secret(name, details.channel.secret);
    }
    StrHelper::strncpy(details.name, name, sizeof(details.name));
    mesh::Utils::sha256(details.channel.hash, sizeof(details.channel.hash), details.channel.secret, (int)key_len);

    why = settings_lock();
    if (why) {
        return why;
    }
    // As the app's CMD_SET_CHANNEL: the first free slot, then saved (addChannel() counts only its own additions and
    // would overwrite channels loaded from flash)
    ChannelDetails slot;
    int            free_slot = -1;
    bool           exists    = false;
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
        if (!slot.name[0]) {
            free_slot = free_slot < 0 ? i : free_slot;
        } else if (channel_same_name(slot.name, details.name)) {
            exists = true;
        }
    }
    bool ok = !exists && free_slot >= 0 && node->setChannel(free_slot, details);
    if (ok) {
        store.saveChannels(node);
    }
    mesh_unlock();
    if (exists) {
        return "That channel is already there";
    }
    return ok ? NULL : "No free channel slot";
}

// The slot of the channel a typed name means (channel_name_match: "badgetest" finds "#badgetest", an exact name wins),
// -1 for none. With the lock held.
static int channel_find_locked(char const *typed) {
    ChannelDetails slot;
    int            best = -1, best_match = CHANNEL_MATCH_NONE;
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
        int match = slot.name[0] ? channel_name_match(slot.name, typed) : CHANNEL_MATCH_NONE;
        if (match > best_match) {
            best = i, best_match = match;
        }
    }
    return best;
}

// Clears slot `index` as MeshCore's apps do (setChannel with an empty ChannelDetails) and saves. DataStore writes every
// slot in order, empty ones as zeroes, and loads them back by position: so the later channels keep their indices and
// an emptied slot reads back empty (the lists skip it, the next add reuses it). Called with the lock held; by_name
// (or NULL) picks the slot by name, without case, instead: the stored name exactly (the screens pass it; a typed one
// goes through channel_find_locked first, so a vanished "x" never takes "#x" with it).
static char const *channel_remove_locked(int index, char const *by_name) {
    ChannelDetails slot;
    if (by_name) {
        index = -1;
        for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
            if (slot.name[0] && channel_same_name(slot.name, by_name)) {
                index = i;
                break;
            }
        }
    }
    if (index < 0 || !node->getChannel(index, slot)) {
        return "No such channel";
    }
    char const *why = channel_removable(index, slot.name);
    if (why) {
        return why;
    }
    ChannelDetails empty = {};
    if (!node->setChannel(index, empty)) {
        return "No such channel";
    }
    store.saveChannels(node);
    ESP_LOGI(TAG, "channel %d removed", index);
    return NULL;
}

extern "C" char const *mesh_channel_remove(int index) {
    if (index == 0) {
        return channel_removable(0, "Public"); // refused before the lock: a refusal never waits on MeshCore
    }
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = channel_remove_locked(index, NULL);
    mesh_unlock();
    return why;
}

extern "C" char const *mesh_channel_remove_named(char const *name) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = channel_remove_locked(-1, name);
    mesh_unlock();
    return why;
}

// ---- channel details, rename (#54) ----------------------------------------------------------------------------------

extern "C" size_t mesh_channel_info(int index, char const *name, mesh_channel_info_t *out, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    ChannelDetails slot;
    int            at = index >= 0 ? index : name ? channel_find_locked(name) : -1;
    bool           ok = at >= 0 && at < MAX_GROUP_CHANNELS && node->getChannel(at, slot) && slot.name[0];
    if (ok) {
        out->index = at;
        mesh_sanitize(slot.name, strnlen(slot.name, sizeof(slot.name)), out->name, sizeof(out->name));
        memcpy(out->secret, slot.channel.secret, sizeof(out->secret));
        out->key_len = (uint8_t)mesh_channel_key_len(out->secret);
        out->hash    = slot.channel.hash[0];
    }
    mesh_unlock();
    memset(&slot, 0, sizeof(slot)); // a channel key: not left on the stack
    return ok ? 1 : 0;
}

extern "C" char const *mesh_channel_rename(char const *name, char const *new_in) {
    char        clean[MESH_CHANNEL_MAX + 1];
    bool        hashtag;
    char const *why = channel_rename_parse(new_in, clean, sizeof(clean), &hashtag);
    if (why) {
        return why;
    }
    why = settings_lock();
    if (why) {
        return why;
    }
    ChannelDetails slot, other;
    int            at = -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, other); i++) {
        if (other.name[0] && channel_same_name(other.name, name)) {
            at = i;
            break;
        }
    }
    if (at < 0 || !node->getChannel(at, slot)) {
        why = "No such channel";
    }
    for (int i = 0; !why && i < MAX_GROUP_CHANNELS && node->getChannel(i, other); i++) {
        if (i != at && other.name[0] && channel_same_name(other.name, clean)) {
            why = "Another channel has that name";
        }
    }
    if (!why && hashtag) {
        uint8_t tag[CHANNEL_SECRET_MAX] = {};
        hashtag_secret(clean, tag);
        if (memcmp(tag, slot.channel.secret, sizeof(slot.channel.secret)) != 0) {
            why = "A #name needs that hashtag's key: add it as a new channel instead";
        }
        memset(tag, 0, sizeof(tag));
    }
    if (!why) {
        StrHelper::strncpy(slot.name, clean, sizeof(slot.name)); // the name only: secret and hash stay as they are
        why = node->setChannel(at, slot) ? NULL : "No such channel";
    }
    if (!why) {
        store.saveChannels(node);
        ESP_LOGI(TAG, "channel %d renamed", at);
    }
    mesh_unlock();
    memset(&slot, 0, sizeof(slot));
    memset(&other, 0, sizeof(other));
    return why;
}

// ---- contact export / import (#54) ---------------------------------------------------------------------------------

#define IMPORT_WAIT_MS 3000 // MyMesh takes a loop-back packet in its next loop

extern "C" char const *mesh_contact_export(uint8_t const *prefix, char *uri, size_t cap) {
    if (!prefix && mesh_advert_refusal(clock_is_set())) {
        return "the clock is not set: our advert would carry a wrong time"; // as adverts (#8)
    }
    uint8_t *pkt = (uint8_t *)heap_caps_malloc(MAX_TRANS_UNIT, MALLOC_CAP_SPIRAM);
    if (!pkt) {
        return "no memory";
    }
    char const *why = settings_lock();
    if (why) {
        heap_caps_free(pkt);
        return why;
    }
    int len = 0;
    if (prefix) {
        ContactInfo *c = node->lookupContactByPubKey(prefix, MF_PREFIX);
        len = c ? node->exportContact(*c, pkt) : 0; // the stored advert, a file read
        why = !c ? "No such contact" : len <= 0 ? "MeshCore kept no advert of this contact" : NULL;
    } else {
        mesh::Packet *p = node->createSelfAdvert(node->getNodeName()); // signed, not queued: never sent
        if (p) {
            p->header |= ROUTE_TYPE_FLOOD;
            len = p->writeTo(pkt);
            node->releasePacket(p);
        }
        why = p ? NULL : "MeshCore is busy, try again";
    }
    mesh_unlock();
    if (!why && !mesh_advert_uri(pkt, (size_t)len, uri, cap)) {
        why = "not an advert MeshCore can read";
    }
    heap_caps_free(pkt);
    return why;
}

extern "C" char const *mesh_contact_import(uint8_t const *pkt, size_t len, char *name_out, size_t name_cap) {
    if (len < MESH_ADVERT_PUB_AT + MESH_ADVERT_PUB_LEN || len > MAX_TRANS_UNIT) {
        return "not an advert";
    }
    uint8_t const *pub = pkt + MESH_ADVERT_PUB_AT;
    char const    *why = settings_lock();
    if (why) {
        return why;
    }
    if (memcmp(pub, node->self_id.pub_key, PUB_KEY_SIZE) == 0) {
        why = "that is this badge's own contact";
    } else if (node->lookupContactByPubKey(pub, PUB_KEY_SIZE)) {
        why = "already a contact (left as it is)";
    } else if (node->getNumContacts() >= MAX_CONTACTS) {
        why = "the contact list is full (MeshCore would replace the oldest contact)";
    } else if (!node->importContact(pkt, (uint8_t)len)) {
        why = "MeshCore did not take it (busy, or not an advert)";
    }
    mesh_unlock();
    if (why) {
        return why;
    }
    // MyMesh checks the signature and adds it in its next loop (its auto-add rules, as for an advert heard on air)
    for (uint32_t waited = 0; waited < IMPORT_WAIT_MS; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!mesh_lock_within(100)) {
            continue;
        }
        ContactInfo const *c = node->lookupContactByPubKey(pub, PUB_KEY_SIZE);
        if (c) {
            mesh_sanitize(c->name, strnlen(c->name, sizeof(c->name)), name_out, name_cap);
        }
        bool gone = mesh_is_stopped();
        mesh_unlock();
        if (c) {
            return NULL;
        }
        if (gone) {
            return stopped_why();
        }
    }
    return "not added: a bad signature, or MeshCore's manual add / hop limit setting kept it out";
}

// ---- our adverts (#52) ---------------------------------------------------------------------------------------------

// The advert keys live in the app's settings (a.meshcore) since #123; meshcore_app.c's start_background adopts them
// from badge_fw before the first start
static lichen_app_t const *adv_app(void) {
    return lichen_app_by_id(MESHCORE_APP_ID);
}

// At start, before the task runs
static void adv_load(void) {
    lichen_app_t const *app      = adv_app();
    uint32_t            interval = app_settings_get_u32(app, ADV_KEY_INTERVAL, ADVCFG_INTERVAL_DEFAULT);
    uint32_t            last     = app_settings_get_u32(app, ADV_KEY_LAST, 0);
    uint8_t             flood    = app_settings_get_u8(app, ADV_KEY_FLOOD, 0);
    if (!advcfg_interval_ok(interval)) {
        interval = ADVCFG_INTERVAL_DEFAULT;
    }
    portENTER_CRITICAL(&tile_mux);
    adv.interval_min = interval;
    adv.flood_manual = flood != 0;
    adv.last_epoch   = last;
    adv.next_epoch   = advcfg_next_due(interval, last, mesh_epoch_now()); // 0 until the clock is set: the tick retries
    portEXIT_CRITICAL(&tile_mux);
}

// THE place where an advert of ours goes to the air, with the mesh lock held (the manual ones and the periodic one).
// NULL when queued, else why not. Radio-off switches (#108) gate here.
static char const *advert_tx_locked(bool flood) {
    if (!ident_tx_allowed()) {
        return "identity damaged: TX blocked"; // a damaged identity transmits nothing (#80)
    }
    if (!lichen_radio_get(RADIO_LORA)) {
        return "LoRa is switched off"; // the one gate for "radio off" (#108): the periodic advert skips to its next time
    }
    if (char const *why = mesh_advert_refusal(clock_is_set())) {
        return why; // nodes that heard us before drop an advert stamped before their last one (#8)
    }
    uint32_t now_s = mesh_uptime_s();
    portENTER_CRITICAL(&tile_mux);
    uint32_t last_s = adv.last_uptime ? adv.last_uptime - 1 : 0;
    portEXIT_CRITICAL(&tile_mux);
    if (!advcfg_gap_ok(last_s, now_s)) {
        return "an advert went out a moment ago";
    }
    if (!node->advertNow(flood)) {
        return "no free packet";
    }
    uint32_t epoch = mesh_epoch_now();
    portENTER_CRITICAL(&tile_mux);
    tile_advert_s    = now_s + 1;
    adv.last_uptime  = now_s + 1;
    adv.last_epoch   = epoch;
    adv.next_epoch   = advcfg_next_due(adv.interval_min, epoch, epoch); // the interval counts from any advert
    portEXIT_CRITICAL(&tile_mux);
    app_settings_set_u32(adv_app(), ADV_KEY_LAST, epoch); // logs when it fails
    return NULL;
}

// Once a second from the mesh loop, under the lock: the periodic flood advert. Not before the first-open questions
// are answered (a fresh badge would advertise its default name), nor on an approximate clock (our advert would carry
// a time that may be wrong).
static void advert_tick_locked(void) {
    static uint32_t checked_s;
    uint32_t        now_s = mesh_uptime_s();
    if (now_s == checked_s || now_s < ADVCFG_FIRST_DELAY_S) {
        return;
    }
    checked_s      = now_s;
    uint32_t epoch = mesh_epoch_now();
    portENTER_CRITICAL(&tile_mux);
    uint32_t interval = adv.interval_min, next = adv.next_epoch, last = adv.last_epoch;
    portEXIT_CRITICAL(&tile_mux);
    if (interval == 0 || epoch == 0) {
        return;
    }
    if (next == 0) { // the clock came up after the start, or the interval was just set
        next = advcfg_next_due(interval, last, epoch);
        portENTER_CRITICAL(&tile_mux);
        adv.next_epoch = next;
        portEXIT_CRITICAL(&tile_mux);
    }
    if (epoch < next) {
        return;
    }
    char const *why = clock_is_approximate() ? "the clock is only approximate"
                      : !meshcore_setup_done() ? "MeshCore is not set up yet"
                                               : advert_tx_locked(true);
    static char const *logged_why;
    if (why) {
        // Soft refusals (clock, setup, LoRa off, the gap) are tried again in a minute, a damaged identity waits a
        // whole interval
        uint32_t retry = advcfg_retry_at(epoch, interval, !ident_tx_allowed());
        portENTER_CRITICAL(&tile_mux);
        adv.next_epoch = retry;
        portEXIT_CRITICAL(&tile_mux);
    }
    if (why != logged_why) { // once per change of reason, not once a minute
        logged_why = why;
        ESP_LOG_LEVEL(why ? ESP_LOG_WARN : ESP_LOG_INFO, TAG, "periodic advert (flood): %s", why ? why : "queued");
    }
}

static bool send_advert_mode(bool flood) {
    char const *why = settings_lock();
    if (!why) {
        why = advert_tx_locked(flood);
        mesh_unlock();
    }
    if (why) {
        ESP_LOGW(TAG, "advert not sent: %s", why);
    }
    return why == NULL;
}

extern "C" bool mesh_send_advert_mode(bool flood) {
    return send_advert_mode(flood);
}

extern "C" bool mesh_send_advert(void) {
    portENTER_CRITICAL(&tile_mux);
    bool flood = adv.flood_manual;
    portEXIT_CRITICAL(&tile_mux);
    return send_advert_mode(flood);
}

extern "C" bool mesh_get_advert_cfg(mesh_advert_cfg_t *out) {
    portENTER_CRITICAL(&settings_mux);
    bool known = settings_known;
    out->lat   = acfg_cache.lat;
    out->lon   = acfg_cache.lon;
    out->share_loc = acfg_cache.share;
    out->autoadd   = acfg_cache.autoadd;
    portEXIT_CRITICAL(&settings_mux);
    portENTER_CRITICAL(&tile_mux);
    out->interval_min = adv.interval_min;
    out->flood_manual = adv.flood_manual;
    uint32_t next     = adv.next_epoch;
    portEXIT_CRITICAL(&tile_mux);
    out->next_in_s = advcfg_next_in_s(next, mesh_epoch_now());
    if (!known) {
        memset(out, 0, sizeof(*out));
        out->next_in_s = UINT32_MAX;
    }
    return known;
}

extern "C" char const *mesh_set_advert_interval(uint32_t minutes) {
    if (!advcfg_interval_ok(minutes)) {
        return "interval is 3 hours to 7 days (180 to 10080 min), or off";
    }
    uint32_t epoch = mesh_epoch_now();
    portENTER_CRITICAL(&tile_mux);
    adv.interval_min = minutes;
    uint32_t next    = advcfg_next_due(minutes, adv.last_epoch, epoch);
    if (next && next < epoch + ADVCFG_FIRST_DELAY_S) {
        next = epoch + ADVCFG_FIRST_DELAY_S; // a long-overdue schedule does not fire the moment it is set
    }
    adv.next_epoch  = next;
    portEXIT_CRITICAL(&tile_mux);
    app_settings_set_u32(adv_app(), ADV_KEY_INTERVAL, minutes);
    return NULL;
}

extern "C" char const *mesh_set_advert_manual_flood(bool flood) {
    portENTER_CRITICAL(&tile_mux);
    adv.flood_manual = flood;
    portEXIT_CRITICAL(&tile_mux);
    app_settings_set_u8(adv_app(), ADV_KEY_FLOOD, flood ? 1 : 0);
    return NULL;
}

// The prefs setters below: the lock, the change, the save (as the name and radio setters)
extern "C" char const *mesh_set_location(double lat, double lon) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_location(node->getNodePrefs(), lat, lon);
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
    }
    mesh_unlock();
    return why;
}

extern "C" char const *mesh_set_location_share(bool share) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_loc_share(node->getNodePrefs(), share);
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
    }
    mesh_unlock();
    return why;
}

extern "C" char const *mesh_set_autoadd(advcfg_autoadd_t const *a) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    mesh_prefs_set_autoadd(node->getNodePrefs(), a); // MyMesh reads the prefs on each advert: live at once
    node->savePrefs();
    settings_from_node(node);
    mesh_unlock();
    return NULL;
}

// The table is full and a new node's advert was not added: one notice per boot would hide a second full table after
// a clean-up, so it coalesces instead (an unread one gets a count). Under the mesh lock, from MyMesh's loop.
// Only a flag here: the notice is posted after the unlock (contacts_full_post), as #50 does for its own.
static bool contacts_full_pending;

extern "C" void mesh_contacts_full(void) {
    __atomic_store_n(&contacts_full_pending, true, __ATOMIC_RELEASE);
}

// The mesh task, after the unlock (the notify service takes its own mutex and may read NVS)
static void contacts_full_post(void) {
    if (__atomic_exchange_n(&contacts_full_pending, false, __ATOMIC_ACQ_REL)) {
        lichen_notify_to("meshcore", LN_WARN, "contacts-full", "Contacts full",
                         "New nodes are not added. Remove contacts, or turn on Overwrite oldest in Auto-add.",
                         LN_COALESCE);
    }
}

extern "C" size_t mesh_channels_snapshot(char names[][MESH_CHANNEL_MAX + 1], size_t max) {
    return mesh_channels_snapshot_wait(names, max, MESH_SET_WAIT_MS);
}

extern "C" size_t mesh_channels_snapshot_wait(char names[][MESH_CHANNEL_MAX + 1], size_t max, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return 0;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t         n = 0;
    ChannelDetails details;
    for (int i = 0; i < MAX_GROUP_CHANNELS && n < max; i++) {
        if (node->getChannel(i, details) && details.name[0]) {
            mesh_sanitize(details.name, strnlen(details.name, sizeof(details.name)), names[n], MESH_CHANNEL_MAX + 1);
            n++;
        }
    }
    mesh_unlock();
    return n;
}

// The console commands while MeshCore does not run (#103): "MeshCore stopped (autostart off)" and the like
static bool console_not_running(void) {
    if (mesh_running()) {
        return false;
    }
    char const *text = mesh_run_console_text(meshcore_run_get());
    console_reply("MeshCore %s", text ? text : "not running");
    mesh_ident_t state = mesh_identity_state();
    if (state != MESH_IDENT_UNCHECKED) {
        console_reply("identity: %s", mesh_ident_text(state)); // #110: also when the drive kept it from starting
    }
    return true;
}

// "fw mesh name <name>": the same setter as the screen
static void cmd_mesh_name(char const *name) {
    if (mesh_set_name(name)) {
        mesh_settings_t s;
        mesh_get_settings(&s);
        console_reply("name \"%s\" saved (others see it in the next advert)", s.name);
    } else {
        console_reply("name not set (empty after trimming, or MeshCore busy)");
    }
}

// "fw mesh radio <preset index> [txdbm]" or "fw mesh radio <freq> <bw> <sf> <cr> [txdbm]"
static void cmd_mesh_radio(char const *args) {
    mesh_settings_t now;
    mesh_get_settings(&now);
    float    v[5];
    int      n = sscanf(args, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]);
    float    freq = now.freq, bw = now.bw;
    uint8_t  sf = now.sf, cr = now.cr;
    int      tx = now.tx_dbm;
    char     what[48];
    if (n == 1 || n == 2) {
        radio_preset_t const *p = v[0] >= 0 && v[0] == (int)v[0] ? radio_preset_at((size_t)v[0]) : NULL;
        if (!p) {
            console_reply("no preset %g (0..%u)", v[0], (unsigned)radio_preset_count() - 1);
            return;
        }
        freq = p->freq_mhz, bw = p->bw_khz, sf = p->sf, cr = p->cr;
        tx   = n == 2 ? (int)lroundf(v[1]) : tx;
        snprintf(what, sizeof(what), "preset %s", p->title);
    } else if (n == 4 || n == 5) {
        // Out of the uint8_t range stays out of range for radio_check
        freq = v[0], bw = v[1];
        sf   = v[2] >= 0 && v[2] <= 255 ? (uint8_t)v[2] : 255;
        cr   = v[3] >= 0 && v[3] <= 255 ? (uint8_t)v[3] : 255;
        tx   = n == 5 ? (int)lroundf(v[4]) : tx;
        snprintf(what, sizeof(what), "custom");
    } else {
        console_reply("mesh radio <preset index> [txdbm] | mesh radio <freq MHz> <bw kHz> <sf> <cr> [txdbm]");
        for (size_t i = 0; i < radio_preset_count(); i++) {
            radio_preset_t const *p = radio_preset_at(i);
            console_reply("  %2u %-26s %.3f MHz %g kHz SF%u CR%u", (unsigned)i, p->title, p->freq_mhz, p->bw_khz,
                          p->sf, p->cr);
        }
        return;
    }
    int8_t      tx8 = tx < -128 ? -128 : tx > 127 ? 127 : (int8_t)tx;
    char const *why = mesh_set_radio(freq, bw, sf, cr, tx8);
    if (why) {
        console_reply("radio not changed: %s", why);
    } else {
        console_reply("radio set (%s): %.3f MHz, %g kHz, SF%u, CR%u, %d dBm", what, freq, bw, sf, cr, tx8);
    }
}

// "fw mesh pathhash [1|2|3]": the size now, or set it (saved, nothing is sent)
static void cmd_mesh_pathhash(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (!*args) {
        mesh_settings_t st;
        mesh_get_settings(&st);
        console_reply("path hash %u B (mesh pathhash <1|2|3> to change)", st.path_hash_size);
        return;
    }
    char *end;
    long  v = strtol(args, &end, 10);
    if (end == args || *end || v < 1 || v > 3) {
        console_reply("mesh pathhash [1|2|3]: bytes per hop in a flood's path");
        return;
    }
    char const *why = mesh_set_path_hash((uint8_t)v);
    if (why) {
        console_reply("path hash not changed: %s", why);
    } else {
        console_reply("path hash set to %ld B (saved)", v);
    }
}

// ---- sending (#45) -------------------------------------------------------------------------------------------------

extern "C" size_t mesh_channel_text_max(void) {
    mesh_settings_t st;
    mesh_get_settings(&st);
    return mf_channel_text_max(strlen(st.name));
}

static bool send_ready(char const **why) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || !outbox || mesh_is_stopped()) {
        *why = stopped_why();
        return false;
    }
    if (!ident_tx_allowed()) {
        *why = "identity damaged: TX blocked"; // refused here, not queued to fail at the radio
        return false;
    }
    if (!lichen_radio_get(RADIO_LORA)) { // the one gate for "radio off" (#108); startSendRaw refuses too, as a net
        *why = lichen_airplane_get() ? "LoRa is off (airplane mode)" : "LoRa is off";
        return false;
    }
    return true;
}

extern "C" mesh_msg_id_t mesh_send_channel(char const *channel_name, char const *text, char const **why) {
    char const *local;
    why = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return 0;
    }
    size_t len = mf_text_cut(text, mesh_channel_text_max());
    if (len == 0 && text[0]) {
        *why = "no room for text";
        return 0;
    }
    mesh_ob_enter();
    mesh_msg_id_t id = ob_add_channel(outbox, channel_name, text, len, (uint32_t)millis(), why);
    mesh_ob_exit();
    return id;
}

extern "C" mesh_msg_id_t mesh_send_dm(uint8_t const prefix[6], char const *text, char const **why) {
    char const *local;
    why = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return 0;
    }
    size_t len = mf_text_cut(text, MF_MAX_TEXT);
    mesh_ob_enter();
    mesh_msg_id_t id = ob_add_dm(outbox, prefix, text, len, (uint32_t)millis(), why);
    mesh_ob_exit();
    return id;
}

extern "C" bool mesh_retry(mesh_msg_id_t id) {
    char const *why;
    if (!send_ready(&why)) {
        return false;
    }
    mesh_ob_enter();
    bool ok = ob_retry(outbox, id, (uint32_t)millis());
    mesh_ob_exit();
    return ok;
}

extern "C" bool mesh_out_state(mesh_msg_id_t id, mesh_out_state_t *st, uint8_t *attempt, uint8_t *repeats) {
    if (!outbox) {
        return false;
    }
    mesh_ob_enter();
    bool ok = ob_get(outbox, id, st, attempt, repeats, NULL);
    mesh_ob_exit();
    return ok;
}

// Copies what the list needs under the lock; the formatting happens after it (snprintf inside a portMUX stalls both cores)
struct OutSnap {
    mesh_out_info_t info;
    uint8_t         prefix[MF_PREFIX];
    char            target[OB_TARGET_MAX];
    uint32_t        seq;
};

extern "C" size_t mesh_out_list(mesh_out_info_t *out, size_t max) {
    OutSnap snap[OB_CAP];
    size_t  n = 0;
    if (!outbox) {
        return 0;
    }
    mesh_ob_enter();
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t const *e = &outbox->e[i];
        if (!e->used) {
            continue;
        }
        OutSnap &s       = snap[n++];
        s.info.id        = e->id;
        s.info.is_dm     = e->kind == OB_DM;
        s.info.state     = (mesh_out_state_t)e->state;
        s.info.attempt   = e->attempt;
        s.info.repeats   = e->repeats;
        s.info.trip_ms   = e->trip_ms;
        s.seq            = e->seq;
        memcpy(s.prefix, e->prefix, MF_PREFIX);
        memcpy(s.target, e->target, sizeof(s.target));
    }
    mesh_ob_exit();
    for (size_t i = 1; i < n; i++) { // newest first
        OutSnap t = snap[i];
        size_t  k = i;
        for (; k > 0 && (int32_t)(t.seq - snap[k - 1].seq) > 0; k--) {
            snap[k] = snap[k - 1];
        }
        snap[k] = t;
    }
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = snap[i].info;
        if (snap[i].info.is_dm) {
            snprintf(out[i].target, sizeof(out[i].target), "%02x%02x%02x%02x%02x%02x", snap[i].prefix[0],
                     snap[i].prefix[1], snap[i].prefix[2], snap[i].prefix[3], snap[i].prefix[4], snap[i].prefix[5]);
        } else {
            snprintf(out[i].target, sizeof(out[i].target), "%s", snap[i].target);
        }
    }
    return n;
}

static char const *const OUT_STATE_NAME[] = {"queued", "sent", "on air", "heard repeated", "delivered", "FAILED"};

// ---- trace path and path discovery (#53) ---------------------------------------------------------------------------
// One request at a time, shared with the pipe under ob_mux like the outbox: copy-in, never the mesh lock (LVGL safe)

static mt_t *trace_req; // PSRAM, made with the outbox

extern "C" uint32_t mesh_trace_start(uint8_t const prefix[6], mt_kind_t kind, char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return 0;
    }
    if (!trace_req) {
        *why = "MeshCore is not running";
        return 0;
    }
    uint32_t tag;
    lichen_random_fill(&tag, sizeof(tag)); // a trace is matched by its tag: random, as MeshCore's apps make it
    mesh_ob_enter();
    uint32_t seq = mt_request(trace_req, kind, prefix, tag, (uint32_t)millis(), why);
    mesh_ob_exit();
    return seq;
}

extern "C" bool mesh_trace_get(mt_t *out) {
    if (!trace_req) {
        return false;
    }
    mesh_ob_enter();
    mt_poll(trace_req, (uint32_t)millis(), out); // a plain copy: the lines are made after the critical section
    mesh_ob_exit();
    return true;
}

// ---- log in, status and CLI for a repeater or room (#51) -------------------------------------------------------------
// One request at a time, shared with the pipe under ob_mux like the trace: copy-in, never the mesh lock (LVGL safe)

static rr_t *remote_req; // PSRAM, made with the outbox

extern "C" uint32_t mesh_remote_start(uint8_t const prefix[6], rr_op_t op, char const *arg, char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return 0;
    }
    if (!remote_req) {
        *why = "MeshCore is not running";
        return 0;
    }
    rr_prep_t prep; // classified and redacted here, outside the critical section (#117)
    rr_prepare(op, arg, &prep);
    mesh_ob_enter();
    uint32_t seq = rr_request_prepared(remote_req, op, prefix, arg, &prep, (uint32_t)millis(), why);
    mesh_ob_exit();
    return seq;
}

extern "C" bool mesh_remote_head(rr_head_t *out) {
    if (!remote_req) {
        return false;
    }
    rr_head_t h; // internal RAM: the critical section copies only this, never into PSRAM
    mesh_ob_enter();
    rr_poll_head(remote_req, (uint32_t)millis(), &h);
    mesh_ob_exit();
    *out = h;
    return true;
}

extern "C" bool mesh_remote_get(rr_t *out) {
    if (!remote_req) {
        return false;
    }
    mesh_ob_enter();
    rr_poll(remote_req, (uint32_t)millis(), out); // a plain copy (without the argument): the text is made outside
    mesh_ob_exit();
    return true;
}

extern "C" bool mesh_remote_logout(uint8_t const prefix[6], uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped() || !remote_req || !mesh_lock_within(wait_ms)) {
        return false;
    }
    bool ok = node->remoteLogout(prefix);
    mesh_unlock();
    if (ok) {
        mesh_ob_enter();
        rr_logout(remote_req, prefix);
        mesh_ob_exit();
    }
    return ok;
}

// A channel name typed on the console as stored (channel_find_locked), into out (may be typed); false (said why,
// after `what`) when there is none or MeshCore is not there
static bool console_channel_name(char const *typed, char *out, size_t cap, char const *what) {
    if (char const *why = settings_lock()) {
        console_reply("%s: %s", what, why);
        return false;
    }
    ChannelDetails found;
    int            slot = channel_find_locked(typed);
    bool           have = slot >= 0 && node->getChannel(slot, found);
    mesh_unlock();
    if (!have) {
        console_reply("%s: no channel %s (\"fw channels\" lists them)", what, typed);
        return false;
    }
    snprintf(out, cap, "%.*s", (int)sizeof(found.name), found.name);
    return true;
}

// "fw mesh send <channel> <text>"
static void cmd_mesh_send(char const *args) {
    char channel[OB_TARGET_MAX] = "";
    while (*args == ' ') {
        args++;
    }
    size_t name_len = strcspn(args, " ");
    int    used     = (int)name_len;
    while (args[used] == ' ') {
        used++;
    }
    if (name_len == 0 || !args[used]) {
        console_reply("mesh send <channel name> <text>");
        return;
    }
    if (name_len >= OB_TARGET_MAX) {
        console_reply("channel name longer than %d bytes", OB_TARGET_MAX - 1);
        return;
    }
    memcpy(channel, args, name_len);
    if (!console_channel_name(channel, channel, sizeof(channel), "not queued")) {
        return; // the outbox names the channel as stored: "badgetest" means "#badgetest" (#101)
    }
    if (!clock_is_set()) {
        console_reply("warning: the clock is not set, other nodes will see a wrong time");
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = mesh_send_channel(channel, args + used, &why);
    if (!id) {
        console_reply("not queued: %s", why ? why : "?");
    } else {
        size_t max = mesh_channel_text_max();
        console_reply("queued #%lu on %s (max %u bytes%s)", (unsigned long)id, channel, (unsigned)max,
                      strlen(args + used) > max ? ", text cut" : "");
    }
}

// "fw mesh dm <12 hex digits of the public key> <text>"
static void cmd_mesh_dm(char const *args) {
    uint8_t prefix[6];
    int     used = 12;
    unsigned b[6];
    bool ok = strspn(args, "0123456789abcdefABCDEF") == 12 && args[12] == ' ';
    while (ok && args[used] == ' ') {
        used++;
    }
    if (!ok || !args[used] ||
        sscanf(args, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) < 6) {
        console_reply("mesh dm <12 hex digits of the public key> <text>");
        return;
    }
    for (int i = 0; i < 6; i++) {
        prefix[i] = (uint8_t)b[i];
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = mesh_send_dm(prefix, args + used, &why);
    if (id) {
        console_reply("queued #%lu", (unsigned long)id);
    } else {
        console_reply("not queued: %s", why ? why : "?");
    }
}

// A contact by the start of its key as typed (2 to 12 hex digits): false (said why) when none or more than one
static bool console_contact(char const *hex, uint8_t out[6]) {
    size_t   len = strspn(hex, "0123456789abcdefABCDEF");
    uint8_t  want[6];
    unsigned b;
    if (len < 2 || len > 12 || len % 2 || hex[len]) {
        console_reply("give 2 to 12 hex digits of the contact's key (fw contacts lists them)");
        return false;
    }
    for (size_t i = 0; i < len / 2; i++) {
        sscanf(hex + 2 * i, "%2x", &b);
        want[i] = (uint8_t)b;
    }
    if (!mesh_lock_within(MESH_SET_WAIT_MS)) {
        console_reply("MeshCore is busy, try again");
        return false;
    }
    int              found = 0;
    ContactsIterator it    = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        if (memcmp(c.id.pub_key, want, len / 2) == 0 && found++ == 0) {
            memcpy(out, c.id.pub_key, 6);
        }
    }
    mesh_unlock();
    if (found > 1) {
        console_reply("%d contacts start with %s: give more digits", found, hex);
    } else if (!found) {
        console_reply("no contact starts with %s", hex);
    }
    return found == 1;
}

// "fw mesh path <hex>" shows the stored path, "... reset" forgets it (the screen's Reset path), "... set <hop>[,<hop>..]"
// stores one by hand (#130). Nothing is sent: the next DM goes along it, or floods when there is none.
static void cmd_mesh_path(char const *args) {
    while (*args == ' ') {
        args++;
    }
    char hex[16];
    size_t hl = strcspn(args, " ");
    if (hl == 0 || hl >= sizeof(hex)) {
        console_reply("mesh path <12 hex digits of the key> [reset | set <hop>[,<hop>...]]");
        return;
    }
    memcpy(hex, args, hl);
    hex[hl] = 0;
    char const *rest = args + hl;
    while (*rest == ' ') {
        rest++;
    }
    uint8_t prefix[6];
    if (!console_contact(hex, prefix)) {
        return;
    }
    size_t r = 1;
    if (strcmp(rest, "reset") == 0) {
        r = mesh_contact_do(prefix, MESH_CO_RESET_PATH, MESH_SET_WAIT_MS);
    } else if (strncmp(rest, "set", 3) == 0 && (rest[3] == ' ' || !rest[3])) {
        char const *hops = rest + 3;
        while (*hops == ' ') {
            hops++;
        }
        mesh_settings_t st;
        mesh_get_settings(&st);
        uint8_t         len, path[MESH_CONTACT_PATH_MAX];
        mesh_path_err_t e = mesh_path_parse(hops, st.path_hash_size, &len, path);
        if (e != MESH_PATH_OK) {
            console_reply("path not set (%u B hops): %s", st.path_hash_size, mesh_path_err_text(e));
            return;
        }
        r = mesh_contact_set_path(prefix, len, path, MESH_SET_WAIT_MS);
    } else if (*rest) {
        console_reply("mesh path <hex> [reset | set <hop>[,<hop>...]]");
        return;
    }
    if (r != 1) {
        console_reply(r == 0 ? "that contact is gone" : "MeshCore is busy, try again");
        return;
    }
    mesh_contact_detail_t d;
    if (mesh_contact_detail(prefix, &d, MESH_SET_WAIT_MS) != 1) {
        console_reply("path of that contact is not readable now");
        return;
    }
    mesh_path_t p = mesh_path_decode(d.path_len);
    if (p.flood) {
        console_reply("path %s: flood", d.name);
        return;
    }
    // mesh_hex groups by hash with spaces: the hops, comma separated
    char list[MESH_CONTACT_PATH_MAX * 2 + MESH_CONTACT_PATH_MAX + 1];
    mesh_hex(d.path, (size_t)p.hops * p.hash_size, p.hash_size, list, sizeof(list));
    for (char *q = list; *q; q++) {
        if (*q == ' ') {
            *q = ',';
        }
    }
    console_reply("path %s: direct, %u hop%s, %u B%s%s", d.name, p.hops, p.hops == 1 ? "" : "s", p.hash_size,
                  p.hops ? ": " : "", list);
}

static char const *const TRACE_STATE_NAME[] = {"none", "queued", "sent to MeshCore", "waiting", "done", "failed",
                                               "no answer"};

// "fw mesh trace|discover <hex>": starts one; "fw mesh trace" alone shows the last one and its result
static void cmd_mesh_trace(char const *arg, mt_kind_t kind) {
    if (*arg) {
        uint8_t prefix[6];
        if (!console_contact(arg, prefix)) {
            return;
        }
        char const *why = NULL;
        uint32_t    seq = mesh_trace_start(prefix, kind, &why);
        if (seq) {
            console_reply("%s #%lu queued: fw mesh trace shows the result", kind == MT_TRACE ? "trace" : "discover",
                          (unsigned long)seq);
        } else {
            console_reply("not started: %s", why ? why : "?");
        }
        return;
    }
    mt_t      *t     = (mt_t *)heap_caps_malloc(sizeof(mt_t), MALLOC_CAP_SPIRAM);
    mt_line_t *lines = (mt_line_t *)heap_caps_malloc(MT_LINES_MAX * sizeof(mt_line_t), MALLOC_CAP_SPIRAM);
    if (t && lines && mesh_trace_get(t)) {
        console_reply("%s #%lu to %02x%02x%02x%02x%02x%02x: %s", t->kind == MT_TRACE ? "trace" : "discover",
                      (unsigned long)t->seq, t->prefix[0], t->prefix[1], t->prefix[2], t->prefix[3], t->prefix[4],
                      t->prefix[5], TRACE_STATE_NAME[t->state]);
        size_t n = t->seq ? mt_format(t, lines, MT_LINES_MAX) : 0;
        for (size_t i = 0; i < n; i++) {
            console_reply("  %s", lines[i].text);
        }
    }
    free(t);
    free(lines);
}

// ---- "fw mesh remote": log in, status and CLI for a repeater or room (#51) -------------------------------------------

// A repeater or room by name (starts with, any case) or by the hex digits of its key; prints why not
static bool console_remote_target(char const *token, uint8_t out[6]) {
    size_t hex = strspn(token, "0123456789abcdefABCDEF");
    if (hex == strlen(token) && hex >= 2 && hex % 2 == 0) {
        return console_contact(token, out);
    }
    if (!mesh_lock_within(MESH_SET_WAIT_MS)) {
        console_reply("MeshCore is busy, try again");
        return false;
    }
    int              found = 0;
    ContactsIterator it    = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        bool remote = c.type == ADV_TYPE_REPEATER || c.type == ADV_TYPE_ROOM;
        if (remote && strncasecmp(c.name, token, strlen(token)) == 0 && found++ == 0) {
            memcpy(out, c.id.pub_key, 6);
        }
    }
    mesh_unlock();
    if (found > 1) {
        console_reply("%d repeaters or rooms start with %s: give more letters or the key", found, token);
    } else if (!found) {
        console_reply("no repeater or room starts with %s", token);
    }
    return found == 1;
}

// A password typed on the console is remembered only once the login succeeded, as on the screen
static char     console_pw[MF_PASSWORD_MAX + 1];
static uint8_t  console_pw_prefix[6];
static uint32_t console_pw_seq;

static void console_pw_settle(void) {
    if (!console_pw_seq) {
        return;
    }
    rr_t *r = (rr_t *)heap_caps_malloc(sizeof(rr_t), MALLOC_CAP_SPIRAM);
    if (!r || !mesh_remote_get(r)) {
        free(r);
        return;
    }
    bool mine = r->seq == console_pw_seq;
    if (mine && rr_busy(r)) {
        free(r);
        return; // still waiting for the answer
    }
    if (mine && r->state == RR_DONE) {
        meshcore_pw_set(console_pw_prefix, console_pw);
    }
    mesh_wipe(console_pw, sizeof(console_pw)); // done, refused, timed out or replaced: not kept
    console_pw_seq = 0;
    free(r);
}

static void console_remote_show(void) {
    console_pw_settle();
    rr_t *r = (rr_t *)heap_caps_malloc(sizeof(rr_t), MALLOC_CAP_SPIRAM);
    if (r && mesh_remote_get(r)) {
        char text[96];
        rr_state_text(r, text, sizeof(text));
        static char const *const OPS[] = {"login", "status", "cli"};
        console_reply("%s #%lu to %02x%02x%02x%02x%02x%02x: %s", r->seq ? OPS[r->op] : "request",
                      (unsigned long)r->seq, r->prefix[0], r->prefix[1], r->prefix[2], r->prefix[3], r->prefix[4],
                      r->prefix[5], text);
        if (r->logged_in) {
            console_reply("logged in to %02x%02x%02x%02x%02x%02x%s", r->login_prefix[0], r->login_prefix[1],
                          r->login_prefix[2], r->login_prefix[3], r->login_prefix[4], r->login_prefix[5],
                          r->admin ? " (admin)" : "");
        }
        if (r->have_status) {
            rr_line_t lines[RR_LINES_MAX];
            size_t    n = rr_status_lines(&r->status, lines, RR_LINES_MAX);
            for (size_t i = 0; i < n; i++) {
                console_reply("  %s", lines[i].text);
            }
        }
        char *log = (char *)heap_caps_malloc(RR_CLI_KEEP * (RR_CMD_MAX + RR_REPLY_MAX + 8), MALLOC_CAP_SPIRAM);
        if (log) {
            rr_cli_format(r, log, RR_CLI_KEEP * (RR_CMD_MAX + RR_REPLY_MAX + 8));
            for (char *line = strtok(log, "\n"); line; line = strtok(NULL, "\n")) {
                console_reply("  %s", line);
            }
            free(log);
        }
    }
    free(r);
}

// A CLI command from the console. A reboot or a radio change needs the word YES after it, as the screen asks with a
// dialog (rs_cmd_confirm: one list for both, #123); erase is refused further down. 0 with *why saying why (the YES is missing, or MeshCore refused).
static uint32_t console_remote_cli(uint8_t const prefix[6], char const *text, char const **why) {
    char big[RR_CMD_MAX + 5]; // room for " YES": a longer command is refused by rr_request, not cut
    char cmd[RR_CMD_MAX + 5];
    snprintf(big, sizeof(big), "%s", text);
    rs_confirm_t c = rs_cmd_confirm(big, cmd, sizeof(cmd));
    mesh_wipe(big, sizeof(big));
    if (c == RS_CONF_MISSING) {
        bool radio = rs_cmd_class(text) == RS_CMD_RADIO;
        *why = radio ? "a wrong radio setting cuts the repeater off the mesh (only a visit to it undoes that): add YES at "
                     "the end to send it, fw mesh remote <who> cli <command> YES"
                     : "this restarts or stops the repeater: add YES at the end to send it, fw mesh remote <who> cli "
                       "<command> YES";
        return 0;
    }
    uint32_t seq = mesh_remote_start(prefix, RR_CLI, cmd, why); // erase is refused in there, YES or not
    mesh_wipe(cmd, sizeof(cmd)); // may hold a new password
    return seq;
}

// fw mesh remote <name|hex> login [password] | status | cli <text> | settings | logout, and fw mesh remote (the last
// result) / fw mesh remote settings (the rows of the last settings read, #117: gets only, never a set).
// A password typed here is remembered like one from the screen; the screens never show it. Nothing is sent unless
// this command says login, status or cli.
static void cmd_mesh_remote(char const *args) {
    console_pw_settle();
    if (!*args) {
        console_remote_show();
        return;
    }
    if (strcmp(args, "settings") == 0) {
        mesh_rset_console_show(); // the rows of the last "settings" read (#117)
        return;
    }
    char token[40];
    int  used = 0;
    if (sscanf(args, "%39s%n", token, &used) != 1 || strlen(token) >= sizeof(token) - 1) {
        console_reply("mesh remote <name or key digits> login [password] | status | cli <text> | settings | logout");
        return;
    }
    char const *verb = args + used + strspn(args + used, " ");
    uint8_t     prefix[6];
    if (!console_remote_target(token, prefix)) {
        return;
    }
    char const *why = NULL;
    uint32_t    seq = 0;
    if (strcmp(verb, "settings") == 0) {
        mesh_rset_console_read(prefix); // gets only, never a set (#117)
        return;
    }
    if (strcmp(verb, "logout") == 0) {
        console_reply(mesh_remote_logout(prefix, MESH_SET_WAIT_MS) ? "logged out" : "MeshCore is busy or the contact is gone");
        return;
    }
    if (strncmp(verb, "login", 5) == 0 && (verb[5] == ' ' || !verb[5])) {
        char        saved[MF_PASSWORD_MAX + 2];
        char const *pw = verb[5] ? verb + 6 : "";
        bool typed = *pw && strlen(pw) <= MF_PASSWORD_MAX;
        if (typed) {
            // kept below, once the login succeeded
        } else if (!*pw && meshcore_pw_get(prefix, saved, sizeof(saved))) {
            pw = saved;
        } else {
            console_reply(*pw ? "the password is at most 15 characters" : "no password remembered: fw mesh remote <who> login <password>");
            return;
        }
        seq = mesh_remote_start(prefix, RR_LOGIN, pw, &why);
        if (seq && typed) {
            snprintf(console_pw, sizeof(console_pw), "%s", pw);
            memcpy(console_pw_prefix, prefix, 6);
            console_pw_seq = seq;
        }
        mesh_wipe(saved, sizeof(saved));
    } else if (strcmp(verb, "status") == 0) {
        seq = mesh_remote_start(prefix, RR_STATUS, NULL, &why);
    } else if (strncmp(verb, "cli ", 4) == 0) {
        seq = console_remote_cli(prefix, verb + 4, &why);
    } else {
        console_reply("mesh remote <name or key digits> login [password] | status | cli <text> | settings | logout");
        return;
    }
    if (seq) {
        console_reply("#%lu queued: fw mesh remote shows the answer", (unsigned long)seq);
    } else {
        console_reply("not started: %s", why ? why : "?");
    }
}

static void cmd_mesh_out(void) {
    mesh_out_info_t list[OB_CAP];
    size_t          n = mesh_out_list(list, OB_CAP);
    for (size_t i = 0; i < n; i++) {
        console_reply("out #%lu %s %s: %s, attempt %u, repeats %u, trip %lu ms", (unsigned long)list[i].id,
                      list[i].is_dm ? "dm" : "channel", list[i].target, OUT_STATE_NAME[list[i].state], list[i].attempt,
                      list[i].repeats, (unsigned long)list[i].trip_ms);
    }
    console_reply("%u outgoing message(s)", (unsigned)n);
}

// "fw mesh link": the companion pipe's counters (#45, #55)
static void cmd_mesh_link(void) {
    mesh_lock();
    LinkStats s    = node->pipe().stats();
    bool      busy = node->pipe().draining();
    mesh_unlock();
    console_reply("link connected, protocol %u | cmds out %lu, replies %lu, pushes %lu | self ok %lu bad %lu", s.app_ver,
                  (unsigned long)s.cmds_out, (unsigned long)s.replies, (unsigned long)s.pushes,
                  (unsigned long)s.self_ok, (unsigned long)s.self_bad);
    console_reply("offline queue %d (drained %lu, msg-waiting pushes %lu, draining %d) | lost replies %lu, fifo drops %lu, "
                  "bad frames %lu, send confirmed %lu",
                  (int)offline_queue_len, (unsigned long)s.drained, (unsigned long)s.msg_waiting, busy,
                  (unsigned long)s.lost_replies, (unsigned long)s.fifo_drops, (unsigned long)s.bad_frames,
                  (unsigned long)s.confirmed);
    console_reply("traces back %lu, discoveries answered %lu, paths stored %lu", (unsigned long)s.traces,
                  (unsigned long)s.discoveries, (unsigned long)s.paths_stored);
    console_reply("repeater/room answers %lu, CLI replies %lu", (unsigned long)s.remote_answers,
                  (unsigned long)s.cli_replies);
    for (int i = 0; i < LINK_PUSH_KINDS; i++) {
        if (s.push_count[i]) {
            console_reply("  push 0x%02x x %lu", 0x80 + i, (unsigned long)s.push_count[i]);
        }
    }
    console_reply("mesh task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(mesh_task_handle));
}

// "fw mesh chats": every chat with its unread count and how full its ring is
static void cmd_mesh_chats(void) {
    mesh_conv_t *list = (mesh_conv_t *)heap_caps_malloc(MESH_CONV_MAX * sizeof(mesh_conv_t), MALLOC_CAP_SPIRAM);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n = mesh_convs_snapshot(list, MESH_CONV_MAX, MESH_WAIT_FOREVER);
    for (size_t i = 0; i < n; i++) {
        char what[16];
        if (list[i].key.kind == MESH_CHAT_DM) {
            snprintf(what, sizeof(what), "dm %02x%02x%02x%02x%02x%02x", list[i].key.id[0], list[i].key.id[1],
                     list[i].key.id[2], list[i].key.id[3], list[i].key.id[4], list[i].key.id[5]);
        } else {
            snprintf(what, sizeof(what), "channel");
        }
        console_reply("chat %-15s %-32s unread %u, %u/%u kept", what, list[i].title, (unsigned)list[i].unread,
                      (unsigned)list[i].count, (unsigned)STORE_MESSAGES);
    }
    free(list);
    console_reply("%u chat(s), %u unread", (unsigned)n, (unsigned)tile_unread());
}

// "fw mesh ident simulate" (#80): the mismatch path on the bench without touching the identity file. A copy of the
// live public key with one bit flipped must fail the check; then TX is blocked and the notice raised, as for a real
// mismatch, until the next restart (which checks the real file again).
static void cmd_mesh_ident_simulate(void) {
    uint8_t prv[PRV_KEY_SIZE], pub[PUB_KEY_SIZE];
    mesh_lock();
    size_t got = node->self_id.writeTo(prv, sizeof(prv));
    memcpy(pub, node->self_id.pub_key, sizeof(pub));
    mesh_unlock();
    pub[PUB_KEY_SIZE / 2] ^= 0x10;
    mesh_ident_t flipped = got == PRV_KEY_SIZE ? mesh_ident_check(prv, pub) : MESH_IDENT_UNCHECKED;
    mh_wipe(prv, sizeof(prv));
    console_reply("check of a copy with one bit flipped: %s", mesh_ident_text(flipped));
    ident_apply(MESH_IDENT_MISMATCH, true);
    console_reply("identity: %s (simulated until restart; the file is untouched)", mesh_ident_text(MESH_IDENT_MISMATCH));
}

// "fw mesh start|stop" (#103): the same job as the tile and the Settings row; "fw mesh" shows the result
static void cmd_mesh_run(bool start) {
    char const *why = meshcore_run_request(start);
    if (why) {
        console_reply("MeshCore not %s: %s", start ? "started" : "stopped", why);
    } else {
        console_reply("MeshCore %s (\"fw mesh\" shows when it is done)", start ? "starting..." : "stopping...");
    }
}

// ---- advert, location, auto-add (#52): "fw mesh advert|loc|autoadd" ---------------------------------------------------

static bool arg_onoff(char const *t, bool *v) {
    if (strcmp(t, "on") == 0 || strcmp(t, "off") == 0) {
        *v = t[1] == 'n';
        return true;
    }
    return false;
}

static void reply_result(char const *what, char const *why) {
    console_reply(why ? "%s not changed: %s" : "%s saved", what, why);
}

static void cmd_mesh_advert(char const *args) {
    mesh_advert_cfg_t c;
    if (strncmp(args, "interval ", 9) == 0) {
        uint32_t    min;
        char const *why = advcfg_parse_interval(args + 9, &min);
        reply_result("advert interval", why ? why : mesh_set_advert_interval(min));
    } else if (strncmp(args, "manual ", 7) == 0 && (strcmp(args + 7, "zero") == 0 || strcmp(args + 7, "flood") == 0)) {
        reply_result("manual advert", mesh_set_advert_manual_flood(args[7] == 'f'));
    } else if (*args) {
        console_reply("mesh advert | mesh advert interval <off|180|6h|2d> (flood, 3 h to 7 d) | mesh advert manual <zero|flood>");
        return;
    }
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    char iv[16], next[12];
    advcfg_interval_text(c.interval_min, iv, sizeof(iv));
    advcfg_next_badge(c.next_in_s, next, sizeof(next));
    console_reply("advert every %s (flood), next %s; the Advertise button sends %s", iv, next[0] ? next : "-",
                  c.flood_manual ? "flood" : "zero-hop");
}

static void cmd_mesh_loc(char const *args) {
    mesh_advert_cfg_t c;
    double            lat, lon;
    char              a_lat[24], a_lon[24];
    if (strncmp(args, "share ", 6) == 0) {
        bool on;
        if (arg_onoff(args + 6, &on)) {
            reply_result("location sharing", mesh_set_location_share(on));
        } else {
            console_reply("mesh loc share on|off");
            return;
        }
    } else if (*args) {
        if (sscanf(args, "%23s %23s", a_lat, a_lon) == 2) {
            char const *why = advcfg_parse_coord(a_lat, true, &lat);
            if (!why) {
                why = advcfg_parse_coord(a_lon, false, &lon);
            }
            reply_result("location", why ? why : mesh_set_location(lat, lon));
        } else {
            console_reply("mesh loc | mesh loc <lat> <lon> (degrees, 52.3702 4.8952) | mesh loc share on|off");
            return;
        }
    }
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    char pos[40];
    advcfg_coord_text(c.lat, c.lon, pos, sizeof(pos));
    console_reply("location %s, %s in adverts", advcfg_location_set(c.lat, c.lon) ? pos : "not set",
                  c.share_loc ? "shared" : "not shared");
}

static void cmd_mesh_autoadd(char const *args) {
    mesh_advert_cfg_t c;
    if (!mesh_get_advert_cfg(&c)) {
        return;
    }
    advcfg_autoadd_t a = c.autoadd;
    bool             change = true, on;
    char const      *why = NULL;
    if (arg_onoff(args, &on)) {
        a.on = on;
        if (on && (a.types & ADVCFG_AA_TYPES) == 0) {
            a.types = ADVCFG_AA_TYPES;
        }
    } else if (strncmp(args, "types ", 6) == 0) {
        why = advcfg_parse_types(args + 6, &a.types);
        a.on = true;
    } else if (strncmp(args, "hops ", 5) == 0) {
        why = advcfg_parse_hops(args + 5, &a.max_hops);
    } else if (strncmp(args, "overwrite ", 10) == 0 && arg_onoff(args + 10, &on)) {
        a.overwrite = on;
    } else if (*args) {
        console_reply("mesh autoadd | on|off | types <all|chat,repeater,room,sensor> | hops <any|direct|n> | overwrite on|off");
        return;
    } else {
        change = false;
    }
    if (change) {
        reply_result("auto-add", why ? why : mesh_set_autoadd(&a));
        mesh_get_advert_cfg(&c);
    }
    char types[48], hops[20];
    advcfg_types_text(c.autoadd.types, types, sizeof(types));
    advcfg_hops_text(c.autoadd.max_hops, hops, sizeof(hops));
    console_reply("auto-add %s (%s), max hops %s, overwrite oldest when full: %s", c.autoadd.on ? "on" : "off", types,
                  hops, c.autoadd.overwrite ? "on" : "off");
}

static void cmd_mesh(char const *args) {
    if (strcmp(args, "start") == 0 || strcmp(args, "stop") == 0) {
        cmd_mesh_run(strcmp(args, "start") == 0);
        return;
    }
    if (strncmp(args, "autostart", 9) == 0 && (args[9] == ' ' || !args[9])) {
        bool on; // #115: a reboot loop without MeshCore needs the start-at-boot setting from the console
        if (args[9] && arg_onoff(args + 10, &on)) {
            console_reply("start at boot %s (%s)", on ? "on" : "off", meshcore_autostart_set(on) ? "saved" : "not saved");
        } else {
            console_reply("start at boot: %s (mesh autostart on|off)", meshcore_autostart() ? "on" : "off");
        }
        return;
    }
    if (strncmp(args, "hist", 4) == 0 && (args[4] == ' ' || !args[4])) {
        mesh_journal_console(args[4] ? args + 5 : ""); // the journal's own status: there whether MeshCore runs or not
        return;
    }
    if (strncmp(args, "notify", 6) == 0 && (args[6] == ' ' || !args[6])) {
        mesh_notify_console(args[6] ? args + 7 : ""); // #50: the settings and counters are there whether MeshCore runs or not
        return;
    }
    if (console_not_running()) {
        return;
    }
    if (strncmp(args, "name ", 5) == 0) {
        cmd_mesh_name(args + 5);
        return;
    }
    if (strncmp(args, "radio", 5) == 0 && (args[5] == ' ' || !args[5])) {
        cmd_mesh_radio(args + 5);
        return;
    }
    if (strncmp(args, "pathhash", 8) == 0 && (args[8] == ' ' || !args[8])) {
        cmd_mesh_pathhash(args + 8);
        return;
    }
    if (strncmp(args, "path ", 5) == 0) {
        cmd_mesh_path(args + 5);
        return;
    }
    if (strncmp(args, "send ", 5) == 0) {
        cmd_mesh_send(args + 5);
        return;
    }
    if (strncmp(args, "dm ", 3) == 0) {
        cmd_mesh_dm(args + 3);
        return;
    }
    if (strcmp(args, "out") == 0) {
        cmd_mesh_out();
        return;
    }
    if (strncmp(args, "trace", 5) == 0 && (args[5] == ' ' || !args[5])) {
        cmd_mesh_trace(args + (args[5] ? 6 : 5), MT_TRACE);
        return;
    }
    if (strncmp(args, "discover ", 9) == 0) {
        cmd_mesh_trace(args + 9, MT_DISCOVER);
        return;
    }
    if (strncmp(args, "remote", 6) == 0 && (args[6] == ' ' || !args[6])) {
        cmd_mesh_remote(args + (args[6] ? 7 : 6));
        return;
    }
    if (strcmp(args, "link") == 0) {
        cmd_mesh_link();
        return;
    }
    if (strcmp(args, "chats") == 0) {
        cmd_mesh_chats();
        return;
    }
    if (strcmp(args, "ident simulate") == 0) {
        cmd_mesh_ident_simulate();
        return;
    }
    if (strncmp(args, "advert", 6) == 0 && (args[6] == ' ' || !args[6])) {
        cmd_mesh_advert(args + (args[6] ? 7 : 6));
        return;
    }
    if (strncmp(args, "loc", 3) == 0 && (args[3] == ' ' || !args[3])) {
        cmd_mesh_loc(args + (args[3] ? 4 : 3));
        return;
    }
    if (strncmp(args, "autoadd", 7) == 0 && (args[7] == ' ' || !args[7])) {
        cmd_mesh_autoadd(args + (args[7] ? 8 : 7));
        return;
    }
    if (*args) {
        console_reply("mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset index | freq bw sf cr> "
                      "[txdbm] | mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                      "mesh dm <12 hex> <text> | mesh remote [<name|hex> login [pw]|status|cli <text>|settings|logout] | mesh remote settings | mesh trace [<hex>] | mesh discover <hex> | mesh advert [interval <off|180|6h|2d>] [manual <zero|flood>] | mesh loc "
                      "[<lat> <lon> | share on|off] | mesh autoadd [on|off | types <all|chat,repeater,room,sensor> | hops "
                      "<any|direct|n> | overwrite on|off]");
        return;
    }
    mesh_lock();
    uint8_t  key[4];
    char     name[sizeof(node->getNodePrefs()->node_name)];
    int      contacts = node->getNumContacts();
    uint32_t rx = radio_driver.getPacketsRecv(), dropped = lora_radio.rx_dropped();
    uint32_t sent = radio_driver.getPacketsSent(), send_failed = lora_radio.send_failed;
    uint32_t timeouts = lora_radio.send_timeouts, refused = lora_radio.send_refused;
    size_t   msgs = mesh_store_message_count();
    memcpy(key, node->self_id.pub_key, sizeof(key));
    snprintf(name, sizeof(name), "%s", node->getNodeName());
    mesh_unlock();
    char      clock[24] = "not set";
    time_t    now       = time(NULL);
    struct tm utc;
    if (clock_is_set() && gmtime_r(&now, &utc)) {
        strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%SZ", &utc);
    }
    console_reply("clock %s", clock);
    console_reply("mesh id %02x%02x%02x%02x \"%s\" | rx %lu, dropped %lu | tx %lu, failed %lu, timed out %lu, "
                  "refused %lu | %d contacts, %u messages",
                  key[0], key[1], key[2], key[3], name, (unsigned long)rx, (unsigned long)dropped,
                  (unsigned long)sent, (unsigned long)send_failed, (unsigned long)timeouts, (unsigned long)refused,
                  contacts, (unsigned)msgs);
    console_reply("identity: %s", mesh_ident_text(__atomic_load_n(&ident_state, __ATOMIC_ACQUIRE)));
    mesh_settings_t st;
    if (mesh_get_settings(&st)) {
        int preset = radio_preset_match(st.freq, st.bw, st.sf, st.cr);
        console_reply("radio %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B (%s)", st.freq, st.bw, st.sf, st.cr,
                      st.tx_dbm, st.path_hash_size, preset >= 0 ? radio_preset_at((size_t)preset)->title : "custom");
    }
}

// ---- "fw contacts export|import" (#54) ------------------------------------------------------------------------------

// PSRAM: the parts of a link pasted so far (the USB console takes 95 bytes a line, the Wi-Fi console a whole link)
static mesh_import_t *import_text;
static_assert(sizeof("contacts import ") - 1 + MESH_ADVERT_URI_MAX - 1 < CONSOLE_WIFI_CMD_MAX,
              "a contact link fits one Wi-Fi console line (#110)");

// "fw contacts export [<12 hex>]": the contact's (or our own) export link
static void contacts_export(char const *arg) {
    uint8_t  prefix[MF_PREFIX];
    unsigned b[MF_PREFIX];
    bool     self = !*arg;
    if (!self && (strspn(arg, "0123456789abcdefABCDEF") != 12 || arg[12] ||
                  sscanf(arg, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) < 6)) {
        console_reply("contacts export [<12 hex digits of the public key>] (none: this badge)");
        return;
    }
    for (int i = 0; i < MF_PREFIX; i++) {
        prefix[i] = (uint8_t)b[i];
    }
    char *uri = (char *)heap_caps_malloc(MESH_ADVERT_URI_MAX, MALLOC_CAP_SPIRAM);
    if (!uri) {
        console_reply("no memory");
        return;
    }
    char const *why = mesh_contact_export(self ? NULL : prefix, uri, MESH_ADVERT_URI_MAX);
    if (why) {
        console_reply("not exported: %s", why);
    } else {
        console_reply("%s", uri);
        console_reply("(%u characters; to import on a badge: \"fw contacts import <link>\" (over USB in parts of up to "
                      "79 characters), then \"fw contacts import end\")", (unsigned)strlen(uri));
    }
    free(uri);
}

// "fw contacts import <part>" ... "fw contacts import end" (or "cancel")
static void contacts_import(char const *arg) {
    if (!import_text) {
        import_text = (mesh_import_t *)heap_caps_calloc(1, sizeof(*import_text), MALLOC_CAP_SPIRAM);
        if (!import_text) {
            console_reply("no memory");
            return;
        }
    }
    if (strcmp(arg, "cancel") == 0) {
        import_text->len = 0;
        console_reply("import cancelled");
        return;
    }
    if (strcmp(arg, "end") != 0) {
        char const *why = *arg ? mesh_import_add(import_text, arg) : "contacts import <part> | end | cancel";
        if (why) {
            console_reply("%s", why);
        } else {
            console_reply("%u characters so far (\"fw contacts import end\" when all parts are in)",
                          (unsigned)import_text->len);
        }
        return;
    }
    uint8_t    *pkt = (uint8_t *)heap_caps_malloc(MESH_ADVERT_MAX, MALLOC_CAP_SPIRAM);
    char const *why = NULL;
    size_t      len = 0;
    if (!pkt) {
        console_reply("no memory");
        return;
    }
    if (import_text->len == 0) {
        why = "nothing pasted (\"fw contacts import meshcore://...\" first)";
    } else {
        len = mesh_advert_uri_parse(import_text->text, pkt, MESH_ADVERT_MAX, &why);
    }
    import_text->len = 0;
    char name[MESH_NAME_MAX + 1] = "";
    if (len) {
        why = mesh_contact_import(pkt, len, name, sizeof(name));
    }
    if (why) {
        console_reply("not imported: %s", why);
    } else {
        console_reply("contact %02x%02x%02x%02x%02x%02x \"%s\" added", pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7],
                      name);
    }
    free(pkt);
}

static void cmd_contacts(char const *args) {
    if (console_not_running()) {
        return;
    }
    if (strncmp(args, "export", 6) == 0 && (args[6] == ' ' || !args[6])) {
        contacts_export(args[6] ? args + 7 : "");
        return;
    }
    if (strncmp(args, "import", 6) == 0 && (args[6] == ' ' || !args[6])) {
        contacts_import(args[6] ? args + 7 : "");
        return;
    }
    if (*args) {
        console_reply("contacts | contacts export [<12 hex>] | contacts import <part> | end | cancel");
        return;
    }
    size_t          total;
    mesh_contact_t *list = (mesh_contact_t *)heap_caps_malloc(MAX_CONTACTS * sizeof(mesh_contact_t), MALLOC_CAP_SPIRAM);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n = mesh_contacts_snapshot(list, MAX_CONTACTS, &total, MESH_WAIT_FOREVER);
    for (size_t i = 0; i < n; i++) {
        char age[24], hops[12];
        mesh_format_age(list[i].age_s, age, sizeof(age));
        mesh_format_hops(list[i].path_len, hops, sizeof(hops)); // the hash size bits are not hops (#85)
        console_reply("contact %02x%02x%02x%02x%02x%02x %-32s %-8s %-8s %s", list[i].pubkey[0], list[i].pubkey[1], list[i].pubkey[2],
                      list[i].pubkey[3], list[i].pubkey[4], list[i].pubkey[5], list[i].name,
                      mesh_type_name(list[i].type), hops, age);
    }
    free(list);
    console_reply("%u contact(s)", (unsigned)total);
}

static void cmd_msgs(char const *args) {
    if (console_not_running()) {
        return;
    }
    long want = *args ? strtol(args, NULL, 10) : 10;
    want      = want < 1 || want > STORE_MESSAGES ? 10 : want;
    uint32_t    generation;
    mesh_msg_t *list = (mesh_msg_t *)heap_caps_malloc((size_t)want * sizeof(mesh_msg_t), MALLOC_CAP_SPIRAM);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n   = mesh_messages_snapshot(list, (size_t)want, &generation, MESH_WAIT_FOREVER);
    uint32_t now = mesh_uptime_s(), epoch = mesh_epoch_now();
    for (size_t i = 0; i < n; i++) {
        char age[24];
        mesh_format_msg_age(list[i].loaded, list[i].heard_s, list[i].epoch, now, epoch, age, sizeof(age));
        console_reply("[%s] %s (%s, rssi %d)", list[i].channel, list[i].text, age, list[i].rssi_x10 / 10);
    }
    free(list);
    console_reply("%u message(s)", (unsigned)n);
}

static void cmd_channels(char const *args) {
    if (console_not_running()) {
        return;
    }
    (void)args;
    ChannelDetails details;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        mesh_lock();
        bool ok = node->getChannel(i, details) && details.name[0];
        mesh_unlock();
        if (ok) {
            console_reply("channel %d: %s (hash %02x)", i, details.name, details.channel.hash[0]);
        }
    }
}

// "fw channel rm <index|name>": the same as the screen's confirm
static void channel_rm(char const *arg) {
    char *end;
    long  index = strtol(arg, &end, 10);
    bool  by_index = end != arg && *end == 0;
    char name[MESH_CHANNEL_MAX + 1];
    if (!by_index && !console_channel_name(arg, name, sizeof(name), "not removed")) {
        return; // "badgetest" means "#badgetest" (#101)
    }
    char const *why = by_index ? mesh_channel_remove((int)index) : mesh_channel_remove_named(name);
    if (why) {
        console_reply("channel %s not removed: %s", arg, why);
    } else {
        console_reply("channel %s removed", arg);
    }
}

// The channel a console argument names: a slot number or a name (channel_find_locked); false (said why) when none
static bool console_channel_info(char const *arg, mesh_channel_info_t *info) {
    char *end;
    long  index = strtol(arg, &end, 10);
    bool  by_index = end != arg && *end == 0 && index >= 0;
    size_t found = mesh_channel_info(by_index ? (int)index : -1, by_index ? NULL : arg, info, MESH_SET_WAIT_MS);
    if (found == MESH_BUSY) {
        console_reply("MeshCore is busy, try again");
    } else if (!found) {
        console_reply("no channel %s (\"fw channels\" lists them)", arg);
    }
    return found == 1;
}

// "fw channel show <index|name>" (#54): the key and the link MeshCore's apps add a channel from. UART only, as the
// update token: a private channel's key does not belong in the log or the Wi-Fi console.
static void channel_show(char const *arg) {
    struct ShowMem {
        mesh_channel_info_t info;
        char                hex[MESH_KEY_HEX_MAX], b64[MESH_KEY_B64_MAX], uri[MESH_CHANNEL_URI_MAX];
    };
    ShowMem *m = (ShowMem *)heap_caps_calloc(1, sizeof(ShowMem), MALLOC_CAP_SPIRAM); // off the console's 4 KB stack
    if (!m) {
        console_reply("no memory");
        return;
    }
    if (console_channel_info(arg, &m->info)) {
        mesh_channel_info_t const &info = m->info;
        mesh_hex_text(info.secret, info.key_len, m->hex, sizeof(m->hex));
        mesh_base64_text(info.secret, info.key_len, m->b64, sizeof(m->b64));
        console_reply("channel %d: %s (hash %02x, %u-byte key; key and link on the serial console only)", info.index,
                      info.name, info.hash, info.key_len);
        console_reply_private("key (hex) %s", m->hex);
        console_reply_private("key (base64) %s", m->b64);
        if (mesh_channel_uri(info.name, info.secret, info.key_len, m->uri, sizeof(m->uri))) {
            console_reply_private("link %s", m->uri);
        } else {
            console_reply_private("no link: MeshCore's apps only take 16-byte keys");
        }
    }
    memset(m, 0, sizeof(*m)); // a channel key: not left in the heap
    free(m);
}

// "fw channel rename <index|name> <new name>" (#54): the name only, the key stays
static void channel_rename(char const *args) {
    char   which[MESH_CHANNEL_MAX + 2];
    size_t n = strcspn(args, " ");
    char const *new_name = args + n + strspn(args + n, " ");
    if (n == 0 || n >= sizeof(which) || !*new_name) {
        console_reply("channel rename <index|name> <new name>");
        return;
    }
    memcpy(which, args, n);
    which[n] = '\0';
    mesh_channel_info_t info;
    if (!console_channel_info(which, &info)) {
        return;
    }
    memset(info.secret, 0, sizeof(info.secret));
    char const *why = mesh_channel_rename(info.name, new_name);
    if (why) {
        console_reply("channel %s not renamed: %s", info.name, why);
    } else {
        console_reply("channel %d renamed from %s", info.index, info.name);
    }
}

// "fw channel add <name> <base64 key>" or "fw channel add #name": the same as the screen's
static void cmd_channel(char const *args) {
    if (console_not_running()) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "rm ", 3) == 0 && args[3]) {
        channel_rm(args + 3);
        return;
    }
    if (strncmp(args, "show ", 5) == 0 && args[5]) {
        channel_show(args + 5);
        return;
    }
    if (strncmp(args, "rename ", 7) == 0) {
        channel_rename(args + 7);
        return;
    }
    char name[48] = "", key[64] = "";
    if (sscanf(args, "add %47s %63s", name, key) < 1) {
        console_reply("channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                      "channel show <index|name> | channel rename <index|name> <new name>");
        return;
    }
    char const *why = mesh_channel_add(name, key[0] ? key : NULL);
    if (why) {
        console_reply("channel %s not added: %s", name, why);
    } else {
        console_reply("channel %s added", name);
    }
}

// "fw advert": a zero-hop advert now (MyMesh::advert(): nodes in direct range hear it), for testing transmit
static void cmd_advert(char const *args) {
    if (console_not_running()) {
        return;
    }
    bool flood;
    mesh_advert_cfg_t cfg;
    mesh_get_advert_cfg(&cfg);
    flood = strcmp(args, "flood") == 0 ? true : strcmp(args, "zero") == 0 ? false : cfg.flood_manual;
    if (*args && strcmp(args, "flood") != 0 && strcmp(args, "zero") != 0) {
        console_reply("advert [zero|flood] (default: the Advertise setting, now %s)", cfg.flood_manual ? "flood" : "zero");
        return;
    }
    if (!ident_tx_allowed()) {
        console_reply("advert not sent: identity %s", mesh_ident_text(MESH_IDENT_MISMATCH));
        return;
    }
    if (!lichen_radio_get(RADIO_LORA)) {
        console_reply("advert not sent: LoRa is off (fw lora on)");
        return;
    }
    if (char const *why = mesh_advert_refusal(clock_is_set())) {
        console_reply("advert not sent: %s (other nodes would drop it)", why);
        return;
    }
    if (!mesh_send_advert_mode(flood)) {
        console_reply("advert not sent");
    } else if (char const *note = mesh_advert_note(clock_is_approximate())) {
        console_reply("advert queued (%s: %s)", note, clock_src_text(CLOCK_SRC_APP)); // allowed, but said (#83)
    } else {
        console_reply("advert queued");
    }
}

static bool         radio_known;

static void radio_from_prefs(NodePrefs const *prefs, mesh_radio_t *out) {
    out->freq_hz = (uint32_t)lround(prefs->freq * 1000.0) * 1000; // as LoraRadio::setParams rounds it
    out->bw_hz   = (uint32_t)lround(prefs->bw * 1000.0);
    out->sf      = prefs->sf;
    out->cr      = prefs->cr;
    out->tx_dbm  = prefs->tx_power_dbm;
}

extern "C" bool mesh_radio_params(mesh_radio_t *out, uint32_t wait_ms) {
    if (!__atomic_load_n(&radio_known, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return false;
    }
    if (mesh_lock_within(wait_ms)) {
        radio_from_prefs(node->getNodePrefs(), &radio_cache);
        mesh_unlock();
    }
    *out = radio_cache; // only written under the lock, by this function and mesh_start; a torn read is cosmetic
    return true;
}

// ---- the air log (#165) ----------------------------------------------------------------------------------------
// The ring is written on the mesh task (inside MyMesh::loop(), mesh lock held) and read by the screen. Its own
// spinlock covers only the push and the copy, never held together with the mesh lock's work or LVGL's lock.

static portMUX_TYPE   airlog_mux = portMUX_INITIALIZER_UNLOCKED;
static airlog_t       airlog_ring;
static bool           airlog_failed;

extern "C" void mesh_airlog_note(uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, float rssi,
                                 float snr) {
    if (!airlog_ring.e) { // the first packet; only this task writes the pointer, readers see NULL or the whole ring
        if (airlog_failed) {
            return;
        }
        auto *mem = (airlog_entry_t *)heap_caps_calloc(AIRLOG_CAP, sizeof(airlog_entry_t), MALLOC_CAP_SPIRAM);
        if (!mem) {
            airlog_failed = true;
            ESP_LOGW(TAG, "air log: no memory, packets are not listed");
            return;
        }
        portENTER_CRITICAL(&airlog_mux);
        airlog_init(&airlog_ring, mem, AIRLOG_CAP);
        portEXIT_CRITICAL(&airlog_mux);
    }
    int16_t rssi_x10 = (int16_t)lroundf(rssi * 10.0f);
    int16_t snr_x10  = (int16_t)lroundf(snr * 10.0f);
    uint32_t at_s    = mesh_uptime_s();
    portENTER_CRITICAL(&airlog_mux);
    airlog_push(&airlog_ring, header, path_len, path, raw_len, rssi_x10, snr_x10, at_s);
    portEXIT_CRITICAL(&airlog_mux);
}

extern "C" size_t mesh_airlog_snapshot(airlog_entry_t *out, size_t max, uint32_t *seq) {
    portENTER_CRITICAL(&airlog_mux);
    size_t n = airlog_ring.e ? airlog_copy(&airlog_ring, out, max) : 0;
    *seq     = airlog_ring.seq;
    portEXIT_CRITICAL(&airlog_mux);
    return n;
}

// ---- Discover nearby (#167) ----------------------------------------------------------------------------------------
// The screen asks (any task, no mesh lock): the request is only marked pending; the mesh loop puts it on air under the
// lock and its listener feeds the answers into the table. The table has its own spinlock, as the air log.

static portMUX_TYPE disc_mux = portMUX_INITIALIZER_UNLOCKED;
static disc_t       disc_table; // zero = DISC_IDLE

extern "C" bool mesh_discover_start(char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return false;
    }
    uint32_t tag;
    lichen_random_fill(&tag, sizeof(tag));
    portENTER_CRITICAL(&disc_mux);
    bool ok = disc_start(&disc_table, tag, mesh_uptime_s(), why);
    portEXIT_CRITICAL(&disc_mux);
    return ok;
}

extern "C" void mesh_discover_get(disc_t *out) {
    portENTER_CRITICAL(&disc_mux);
    disc_poll(&disc_table, mesh_uptime_s());
    *out = disc_table;
    portEXIT_CRITICAL(&disc_mux);
}

// From the mesh loop, under the lock: sends a pending request
static void discover_tick_locked(void) {
    portENTER_CRITICAL(&disc_mux);
    bool     pending = disc_table.state == DISC_PENDING;
    uint32_t tag     = disc_table.tag;
    portEXIT_CRITICAL(&disc_mux);
    if (!pending) {
        return;
    }
    char const *why = NULL;
    if (!ident_tx_allowed()) {
        why = "identity damaged: TX blocked";
    } else if (!lichen_radio_get(RADIO_LORA)) {
        why = "LoRa is switched off";
    } else {
        uint8_t req[DISC_REQ_LEN];
        disc_request(tag, req);
        mesh::Packet *p = node->createControlData(req, sizeof(req));
        if (p) {
            node->sendZeroHop(p);
        } else {
            why = "no free packet";
        }
    }
    portENTER_CRITICAL(&disc_mux);
    if (why) {
        disc_failed(&disc_table, why);
    } else {
        disc_sent(&disc_table, mesh_uptime_s());
    }
    portEXIT_CRITICAL(&disc_mux);
    ESP_LOGI(TAG, "discover nearby: %s", why ? why : "request sent (zero hop)");
}

// An answer (a control packet) heard, under the lock; MyMesh passes every control packet here
static void discover_note_locked(mesh::Packet const *pkt) {
    if (pkt->payload_len < 6 + DISC_PREFIX) {
        return;
    }
    char const *name = NULL;
    if (ContactInfo *c = node->lookupContactByPubKey(&pkt->payload[6], DISC_PREFIX)) {
        name = c->name;
    }
    int16_t snr_x10 = (int16_t)lroundf(pkt->getSNR() * 10.0f);
    portENTER_CRITICAL(&disc_mux);
    disc_on_payload(&disc_table, pkt->payload, pkt->payload_len, snr_x10, name, mesh_uptime_s());
    portEXIT_CRITICAL(&disc_mux);
}

// ---- the signal history (#166) ---------------------------------------------------------------------------------------
// Written on the mesh task (mesh lock held) from the advert and message hooks, read by the contact's Signal page. As
// the air log: its own spinlock covers only the note and the copy, never held with the mesh lock's work or LVGL's lock.

static portMUX_TYPE  signal_mux = portMUX_INITIALIZER_UNLOCKED;
static sig_table_t  *signal_table;
static bool          signal_failed;

extern "C" void mesh_signal_note(uint8_t const *pub_key, bool is_advert, uint8_t path_len, uint8_t const *path,
                                 float rssi, float snr) {
    if (!signal_table) { // the first packet from a contact; only this task writes the pointer
        if (signal_failed) {
            return;
        }
        auto *mem = (sig_table_t *)heap_caps_calloc(1, sizeof(sig_table_t), MALLOC_CAP_SPIRAM);
        if (!mem) {
            signal_failed = true;
            ESP_LOGW(TAG, "signal history: no memory, contact signal graphs stay empty");
            return;
        }
        sig_init(mem);
        __atomic_store_n(&signal_table, mem, __ATOMIC_RELEASE);
    }
    int16_t  rssi_x10 = (int16_t)lroundf(rssi * 10.0f);
    int16_t  snr_x10  = (int16_t)lroundf(snr * 10.0f);
    uint32_t at_s     = mesh_uptime_s();
    portENTER_CRITICAL(&signal_mux);
    sig_note(signal_table, pub_key, is_advert ? SIG_ADVERT : SIG_MESSAGE, rssi_x10, snr_x10, path_len, path, at_s);
    portEXIT_CRITICAL(&signal_mux);
}

extern "C" bool mesh_signal_get(uint8_t const prefix[6], sig_contact_t *out) {
    sig_table_t *t = __atomic_load_n(&signal_table, __ATOMIC_ACQUIRE);
    if (!t) {
        return false;
    }
    portENTER_CRITICAL(&signal_mux);
    bool found = sig_get(t, prefix, out);
    portEXIT_CRITICAL(&signal_mux);
    return found;
}

#define HEALTH_NOISE_MAX_AGE_S 60 // the C6's channel reading older than this is not shown

// Lock order: only the mesh lock, nothing else held; lora_get_channel_rssi takes the LoRa state spinlock for a copy
extern "C" bool mesh_health_snapshot(mesh_health_t *out, uint32_t wait_ms) {
    mesh_health_t h = {};
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped() || !mesh_lock_within(wait_ms)) {
        return false;
    }
    node->fillHealth(&h);
    h.rx_dropped  = lora_radio.rx_dropped();
    h.send_failed = lora_radio.send_failed;
    mesh_unlock();
    h.have_noise = lora_get_channel_rssi(&h.noise_x10, HEALTH_NOISE_MAX_AGE_S);
    *out         = h;
    return true;
}

extern "C" char const *mesh_firmware_version(void) {
    return FIRMWARE_VERSION;
}

static char node_id_text[9]; // written once in mesh_start, before node_id_known is set

static bool node_id_known;

extern "C" bool mesh_node_id(char *hex, size_t cap) {
    if (!__atomic_load_n(&node_id_known, __ATOMIC_ACQUIRE) || cap == 0) {
        return false;
    }
    snprintf(hex, cap, "%s", node_id_text);
    return true;
}

// totalBytes/usedBytes of the drive, through the OS (lichen_fs.h)
static void drive_bytes(size_t *total, size_t *used) {
    if (!lichen_fs_drive_bytes(total, used)) {
        *total = *used = 0;
    }
}

static void cmd_mesh(char const *args);
static void cmd_contacts(char const *args);
static void cmd_msgs(char const *args);
static void cmd_channels(char const *args);
static void cmd_channel(char const *args);
static void cmd_advert(char const *args);

extern "C" void mesh_console_init(void) {
    static bool registered; // boot, from start_background: one task
    if (registered) {
        return;
    }
    registered = true;
    console_register("mesh",
                     "mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset | freq bw sf cr> [txdbm] | "
                     "mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                     "mesh dm <12 hex> <text> | mesh notify [test dm|chan <name>|fail|mention] | mesh advert ... | mesh loc ... | mesh autoadd ...",
                     cmd_mesh);
    console_register("contacts", "contacts (nodes heard, newest first) | contacts export [<12 hex>] | contacts import "
                     "<part> | end | cancel", cmd_contacts);
    console_register("msgs", "msgs [n] (last n messages)", cmd_msgs);
    console_register("channels", "channels (the channels the badge can read)", cmd_channels);
    console_register("channel", "channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                     "channel show <index|name> | channel rename <index|name> <new name>", cmd_channel);
    console_register("advert", "advert [zero|flood] (send an advert now; default as the Advertise setting)", cmd_advert);
}

// The mesh task, new each start; false when it could not be made
static bool task_create(void) {
    // Before the create: the new task must never see the last run's stopped
    bool was_running = __atomic_load_n(&task_running, __ATOMIC_ACQUIRE);
    __atomic_store_n(&stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task_exited, false, __ATOMIC_RELEASE);
    __atomic_store_n(&stopped, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task_running, true, __ATOMIC_RELEASE);
    if (xTaskCreatePinnedToCore(mesh_task, "mesh", MESH_STACK, NULL, 3, &mesh_task_handle, MESH_CORE) != pdPASS) {
        __atomic_store_n(&task_running, was_running, __ATOMIC_RELEASE);
        __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
        __atomic_store_n(&task_exited, true, __ATOMIC_RELEASE);
        ESP_LOGE(TAG, "mesh task not started");
        return false;
    }
    return true;
}

// After a stop on request (#103): everything is still loaded (node, contacts, channels, the store, the journal and
// its key, the console). The radio is claimed again with the prefs' settings, the unread badge comes back and a new
// task runs. Under the run lock.
static bool resume_locked(void) {
    if (!__atomic_load_n(&task_exited, __ATOMIC_ACQUIRE)) {
        ESP_LOGW(TAG, "MeshCore is still stopping: not started");
        return false;
    }
    if (!lora_radio.claim()) {
        return false; // another owner holds the radio: MeshCore would run deaf
    }
    mesh_lock();
    NodePrefs const *prefs = node->getNodePrefs();
    radio_driver.setParams(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
    radio_driver.setTxPower(prefs->tx_power_dbm);
    tile_set_contacts((size_t)node->getNumContacts());
    mesh_unlock();
    if (!task_create()) {
        lora_radio.end();
        return false;
    }
    mesh_lock();
    tile_publish_unread(); // after stopped is cleared: the badge shows the count again
    mesh_unlock();
    ESP_LOGI(TAG, "MeshCore started again: %d contacts", node->getNumContacts());
    return true;
}

static bool start_locked(char const *files_dir);

extern "C" bool mesh_start(char const *files_dir) {
    if (xSemaphoreTake(run_lock_handle(), pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "MeshCore busy stopping: not started");
        return false;
    }
    bool ok;
    if (__atomic_load_n(&final_stop, __ATOMIC_ACQUIRE)) {
        ESP_LOGW(TAG, "MeshCore stopped for update: no start before the restart");
        ok = false;
    } else if (mesh_stop_pending()) {
        ESP_LOGW(TAG, "MeshCore is still stopping: not started"); // a stop that outlasted its wait (#103 review)
        ok = false;
    } else if (mesh_running()) {
        ok = true; // a second start is not a second MeshCore
    } else if (__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        ok = resume_locked();
    } else {
        ok = start_locked(files_dir);
    }
    xSemaphoreGive(run_lock_handle());
    return ok;
}

// The first start: loads everything (#61, #65, #48). Under the run lock; node is published only at the end, so a start
// that fails before that can be tried again from the top.
static bool start_locked(char const *files_dir) {
    if (!files_dir) {
        ESP_LOGE(TAG, "no directory for MeshCore's files");
        return false;
    }
    mesh_console_init();
    fs_shim_set_root(files_dir);
    fs_shim_set_drive_bytes(drive_bytes);
    mesh_lock();
    mesh_store_set_hooks(store_alloc_ring, store_warn);
    mesh_store_set_clock(mesh_epoch_now);
    bool ok = mesh_store_init(STORE_MESSAGES);
    mesh_unlock();
    void *tables_mem = heap_caps_malloc(sizeof(SimpleMeshTables), MALLOC_CAP_SPIRAM);
    void *mesh_mem   = heap_caps_malloc(sizeof(BadgeMesh), MALLOC_CAP_SPIRAM);
    void *ob_mem     = heap_caps_calloc(1, sizeof(ob_t), MALLOC_CAP_SPIRAM);
    void *mt_mem     = heap_caps_calloc(1, sizeof(mt_t), MALLOC_CAP_SPIRAM);
    void *rr_mem     = heap_caps_calloc(1, sizeof(rr_t), MALLOC_CAP_SPIRAM);
    bool  files      = SPIFFS.begin(true);
    if (!files) {
        mesh_identity_no_files();
    }
    if (!ok || !tables_mem || !mesh_mem || !ob_mem || !mt_mem || !rr_mem || !files) {
        ESP_LOGE(TAG, "no memory or no /storage");
        free(tables_mem);
        free(mesh_mem);
        free(ob_mem);
        free(mt_mem);
        free(rr_mem);
        return false;
    }
    tables    = new (tables_mem) SimpleMeshTables();
    outbox = (ob_t *)ob_mem;
    ob_init(outbox);
    BadgeMesh *n = new (mesh_mem) BadgeMesh(radio_driver, rng, rtc_clock, *tables, store, outbox);
    mt_init((mt_t *)mt_mem);
    n->attachTrace((mt_t *)mt_mem);
    trace_req = (mt_t *)mt_mem;
    rr_init((rr_t *)rr_mem);
    n->attachRemote((rr_t *)rr_mem);
    remote_req = (rr_t *)rr_mem;

    // Everything is loaded before node is published: the screens and the console read MeshCore only through node, and
    // must not walk the contact table while begin() fills it (#65 review)
    adv_load();
    store.begin();
    migrate_identity();
    bool first_boot = !SPIFFS.exists("/prefs.json") && !SPIFFS.exists("/node_prefs");
    radio_driver.begin();
    n->begin();
    ident_verify(n); // before anything can transmit: the task does not exist yet
    apply_boot_radio(n, first_boot);
    migrate_path_hash(n);
    forget_stale_routes_once(n);
    // The history from the SD card (#48), encrypted with a key from the identity begin() just loaded (#98), before the
    // mesh task runs: the replayed messages are older than anything MeshCore adds, which it does only in its loop
    uint8_t prv[PRV_KEY_SIZE];
    // With room for the private key only, that is what it writes; anything else: no key (never stack garbage)
    // A damaged identity (#80) may be a damaged private key: no history key from it, so nothing is written under a
    // wrong key and the stored history stays readable once the identity is sound again
    bool have_prv = ident_tx_allowed() && n->self_id.writeTo(prv, sizeof(prv)) == PRV_KEY_SIZE;
    if (!have_prv) {
        ESP_LOGE(TAG, "identity key not readable or damaged: message history off");
    }
    mesh_journal_boot(STORE_MESSAGES, mesh_lock, mesh_unlock, have_prv ? prv : NULL, sizeof(prv));
    mh_wipe(prv, sizeof(prv));
    n->setListener(&screen_feed);
    n->startPipe(); // connected, so MyMesh's replies and pushes reach us (#45)
    uint8_t const *key = n->self_id.pub_key;
    snprintf(node_id_text, sizeof(node_id_text), "%02x%02x%02x%02x", key[0], key[1], key[2], key[3]);
    mesh_lock();
    settings_from_node(n);
    portENTER_CRITICAL(&tile_mux);
    tile.unread_total = 0; // nothing unread at boot
    tile.contacts     = (size_t)n->getNumContacts();
    portEXIT_CRITICAL(&tile_mux);
    lichen_badge_set("meshcore", 0);
    tile_seed_latest(); // the history's newest line; tile_heard_s stays 0, so no "just now" for an old message
    __atomic_store_n(&node, n, __ATOMIC_RELEASE);
    mesh_unlock();
    __atomic_store_n(&radio_known, true, __ATOMIC_RELEASE);
    __atomic_store_n(&node_id_known, true, __ATOMIC_RELEASE);

    if (!task_create()) {
        // As resume_locked: the claim radio_driver.begin() took is released, so another owner (or the next start,
        // which resumes the node published above) can have the radio
        lora_radio.end();
        return false;
    }
    ESP_LOGI(
        TAG,
        "MeshCore companion up: %d contacts, BadgeMesh %u bytes in PSRAM, identity %s",
        n->getNumContacts(),
        (unsigned)sizeof(BadgeMesh),
        mesh_ident_text(__atomic_load_n(&ident_state, __ATOMIC_ACQUIRE))
    );
    return true;
}
