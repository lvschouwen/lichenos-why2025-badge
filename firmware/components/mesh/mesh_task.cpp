// MeshCore on the badge (#61, #65): MeshCore's own companion (examples/companion_radio MyMesh + DataStore) runs in one
// task over the LoRa bridge; the screens and the console are its user interface and read copies taken under
// mesh_lock (no app port: nothing to expose on a network). MyMesh's loop() holds the lock, so its callbacks (the Listener below) already run under it.
#include "mesh_link.h"

#include "lora_radio.h"
#include "identity_move.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_chat.h"
#include "mesh_frame.h"
#include "channel_input.h"
#include "mesh_prefs.h"
#include "radio_presets.h"

#include "SPIFFS.h"

#include <SHA256.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/TxtDataHelpers.h>

#include "bootloader_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

extern "C" {
#include "fw_console.h"
#include "lora.h"
}

#include <ctype.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STORE_MESSAGES 100 // kept per chat in mesh_store (PSRAM, allocated with the chat's first message)
#define MESH_STACK     16384 // Ed25519 signature checks are deep, and MyMesh keeps File objects on the stack
#define MESH_CORE      0     // LVGL runs on core 1
#define MESH_POLL_MS   2
#define MESH_SLOW_LOOP_MS 200

static char const TAG[] = "mesh";

// The shim's platform hooks
extern "C" unsigned long millis(void) {
    return (unsigned long)(esp_timer_get_time() / 1000);
}
extern "C" void shim_fill_random(void *buf, size_t len) {
    esp_fill_random(buf, len);
}
extern "C" void delay(unsigned long ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

extern "C" uint32_t mesh_uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

class EspRng : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { esp_fill_random(dest, sz); }
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
void BadgeBoard::reboot() {
    system_restart("MeshCore reboot");
}
bool radio_init() {
    return true;
}
uint32_t radio_get_rng_seed() {
    return esp_random();
}
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
    radio_driver.setParams(freq, bw, sf, cr);
}
void radio_set_tx_power(uint8_t dbm) {
    radio_driver.setTxPower((int8_t)dbm);
}
// A new key pair, with the entropy source on: the P4 has no radio of its own for esp_random() to draw on
mesh::LocalIdentity radio_new_identity() {
    EspRng rng;
    bootloader_random_enable();
    mesh::LocalIdentity id(&rng);
    bootloader_random_disable();
    return id;
}

// DataStore::formatFileSystem() (the app's factory reset) erases NVS on ESP32; on the badge that would take the Wi-Fi
// credentials with it, so DataStore.cpp is built with nvs_flash_erase renamed to this: MeshCore's own files go (the
// shim's format()), the rest of NVS stays
extern "C" esp_err_t mesh_nvs_flash_erase(void) {
    ESP_LOGW(TAG, "factory reset from the app: MeshCore's files erased, NVS (Wi-Fi) kept");
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

// The home tile's numbers (#67): written by the mesh task, read by the LVGL task, under a spinlock of their own so the
// tile never waits on MeshCore
static mesh_tile_t  tile;
static uint32_t     tile_heard_s; // uptime of the newest message, 0 for none
static portMUX_TYPE tile_mux = portMUX_INITIALIZER_UNLOCKED;

// The unread total over all chats, after the store changed (called under the mesh lock)
static void tile_publish_unread(void) {
    uint32_t total = mesh_store_unread_total();
    portENTER_CRITICAL(&tile_mux);
    tile.unread_total = total;
    portEXIT_CRITICAL(&tile_mux);
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
    uint32_t total = mesh_store_unread_total();
    portENTER_CRITICAL(&tile_mux);
    memcpy(tile.latest, m.latest, sizeof(tile.latest));
    tile.unread_total = total;
    tile_heard_s      = mesh_uptime_s();
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
    portEXIT_CRITICAL(&tile_mux);
    out->last_heard_age_s = heard ? mesh_uptime_s() - heard : 0;
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
        char from_name[sizeof(from.name) + 2]; // mesh_store cuts it to MESH_CHANNEL_MAX
        snprintf(from_name, sizeof(from_name), "@%s", from.name);
        // The tile follows the store: a message the store dropped (and logged) is not shown as the latest either
        if (mesh_store_add_dm(from_name, text, (int16_t)(radio_driver.getLastRSSI() * 10.0f), mesh_uptime_s(),
                              from.id.pub_key)) {
            tile_on_message(from_name, text);
        }
    }
    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &channel, char const *text) override {
        if (mesh_store_add_message(channel.name, text, (int16_t)(radio_driver.getLastRSSI() * 10.0f), mesh_uptime_s())) {
            tile_on_message(channel.name, text);
        }
    }
    void onQueueSizeChanged(int msgcount) override { offline_queue_len = msgcount; }
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
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_contacts_snapshot(mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms) {
    size_t n = 0;
    *total   = 0;
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || max == 0) {
        return 0;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    uint32_t         now = rtc_clock.getCurrentTime();
    ContactsIterator it  = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        (*total)++;
        mesh_contact_t entry;
        memcpy(entry.pubkey, c.id.pub_key, sizeof(entry.pubkey));
        mesh_sanitize(c.name, strnlen(c.name, sizeof(c.name)), entry.name, sizeof(entry.name));
        entry.type     = c.type;
        entry.path_len = c.out_path_len;
        entry.age_s    = now > c.lastmod ? now - c.lastmod : 0;
        // Keep out[] sorted by age, newest first, dropping the oldest once full
        size_t pos = n;
        while (pos > 0 && out[pos - 1].age_s > entry.age_s) {
            pos--;
        }
        if (pos >= max) {
            continue;
        }
        size_t move = (n < max ? n : max - 1) - pos;
        memmove(&out[pos + 1], &out[pos], move * sizeof(*out));
        out[pos] = entry;
        n        = n < max ? n + 1 : max;
    }
    mesh_unlock();
    return n;
}

static TaskHandle_t mesh_task_handle;
static bool stop_requested; // mesh_stop_for_update -> the task
static bool task_running;   // set once the task exists
static bool stopped;        // the task -> everyone: it has saved and is gone (or is about to delete itself)

static bool mesh_is_stopped(void) {
    return __atomic_load_n(&stopped, __ATOMIC_ACQUIRE);
}

// MeshCore writes contacts lazily; a firmware update must not lose the last few minutes of them
static bool keep_contact(ContactInfo const &c) {
    return c.type != ADV_TYPE_NONE; // as MyMesh's own filter: not the transient entries
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
            bool ok;
            if (o.kind == OB_DM) {
                ContactInfo *c = node ? node->lookupContactByPubKey(o.prefix, MF_PREFIX) : nullptr;
                snprintf(o.target, sizeof(o.target), "@%s", c ? c->name : "?");
                ok = mesh_store_add_outgoing_dm(o.target, line, o.id, mesh_uptime_s(), o.prefix);
            } else {
                ok = mesh_store_add_outgoing(o.target, line, o.id, mesh_uptime_s());
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
    }
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
        if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
            // The task stops itself, between two loops and with the lock held: nothing is half done, and the stack
            // (16 KB, the saves keep File objects on it) is the one that is big enough
            int64_t save_started = esp_timer_get_time();
            store.savePrefs(*node->getNodePrefs());
            int64_t contacts_started = esp_timer_get_time();
            store.saveContacts(node, keep_contact);
            int64_t contacts_us = esp_timer_get_time() - contacts_started;
            store.saveChannels(node);
            int64_t save_us = esp_timer_get_time() - save_started;
            __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
            mesh_unlock();
            // The contact rewrite is the same one MyMesh does lazily in its loop: its time here says what it costs there
            ESP_LOGW(TAG, "MeshCore stopped for the update: prefs, %d contacts, channels saved in %lld ms (contacts %lld ms)",
                     node->getNumContacts(), (long long)(save_us / 1000), (long long)(contacts_us / 1000));
            vTaskDelete(NULL);
        }
        int64_t took_ms = (esp_timer_get_time() - started) / 1000;
        mesh_unlock();
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

extern "C" bool mesh_stop_for_update(void) {
    if (mesh_is_stopped() || !__atomic_load_n(&task_running, __ATOMIC_ACQUIRE)) {
        return true; // nothing running (never started, or already stopped)
    }
    __atomic_store_n(&stop_requested, true, __ATOMIC_RELEASE);
    // A loop can sit in a file save for a second or two; the saves here the same again
    for (int i = 0; i < 1000 && !mesh_is_stopped(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!mesh_is_stopped()) {
        ESP_LOGE(TAG, "MeshCore did not stop in 10 s");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // the task deletes itself right after giving the lock back
    return true;
}

// Milestone 2 kept the key pair in NVS; MeshCore's DataStore keeps it in /storage/meshcore. Moved once, so the
// contacts that already know the badge keep knowing it.
static void migrate_identity(void) {
    mesh::LocalIdentity existing;
    nvs_handle_t        nvs;
    uint8_t             blob[128];
    size_t              len = sizeof(blob);
    if (store.loadMainIdentity(existing) || nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(nvs, "identity", blob, &len) == ESP_OK && len >= PRV_KEY_SIZE + PUB_KEY_SIZE) {
        mesh::LocalIdentity id;
        id.readFrom(blob, len);
        if (identity_move(store, id)) {
            nvs_erase_key(nvs, "identity");
            nvs_commit(nvs);
            ESP_LOGW(TAG, "MeshCore identity moved from NVS to /storage/meshcore");
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
    radio_from_prefs(prefs, &radio_cache);
    portENTER_CRITICAL(&settings_mux);
    settings_cache = s;
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
    if (mesh_is_stopped()) { // stopped (and saved) for an update while we waited
        mesh_unlock();
        return "MeshCore stopped for update";
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

extern "C" char const *mesh_set_radio(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t path_hash_size,
                                      int8_t tx_dbm) {
    char const *why = radio_check(freq, bw, sf, cr, tx_dbm);
    if (why) {
        return why; // checked before the lock: a refusal never waits on MeshCore
    }
    why = settings_lock();
    if (why) {
        return why;
    }
    why = mesh_prefs_set_radio(node->getNodePrefs(), freq, bw, sf, cr, path_hash_size, tx_dbm);
    if (!why) {
        node->savePrefs();
        settings_from_node(node);
    }
    mesh_unlock();
    if (!why) {
        ESP_LOGW(TAG, "radio set: %.3f MHz, %.2f kHz, SF%u, CR%u, %d dBm, path hash %u B", freq, bw, sf, cr, tx_dbm,
                 path_hash_size);
    }
    return why;
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

// Clears slot `index` as MeshCore's apps do (setChannel with an empty ChannelDetails) and saves. DataStore writes every
// slot in order, empty ones as zeroes, and loads them back by position: so the later channels keep their indices and
// an emptied slot reads back empty (the lists skip it, the next add reuses it). Called with the lock held; by_name
// (or NULL) picks the slot by name, without case, instead.
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
    ESP_LOGW(TAG, "channel %d removed", index);
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

extern "C" bool mesh_send_advert(void) {
    if (mesh_advert_refusal(clock_is_set())) {
        return false; // nodes that heard us before drop an advert stamped before their last one (#8)
    }
    if (settings_lock()) {
        return false;
    }
    bool ok = node->advert();
    mesh_unlock();
    return ok;
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
    uint8_t  sf = now.sf, cr = now.cr, phs = now.path_hash_size;
    int      tx = now.tx_dbm;
    char     what[48];
    if (n == 1 || n == 2) {
        radio_preset_t const *p = v[0] >= 0 && v[0] == (int)v[0] ? radio_preset_at((size_t)v[0]) : NULL;
        if (!p) {
            console_reply("no preset %g (0..%u)", v[0], (unsigned)radio_preset_count() - 1);
            return;
        }
        freq = p->freq_mhz, bw = p->bw_khz, sf = p->sf, cr = p->cr, phs = p->path_hash_size;
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
            console_reply("  %2u %-26s %.3f MHz %g kHz SF%u CR%u, path hash %u B", (unsigned)i, p->title, p->freq_mhz,
                          p->bw_khz, p->sf, p->cr, p->path_hash_size);
        }
        return;
    }
    int8_t      tx8 = tx < -128 ? -128 : tx > 127 ? 127 : (int8_t)tx;
    char const *why = mesh_set_radio(freq, bw, sf, cr, phs, tx8);
    if (why) {
        console_reply("radio not changed: %s", why);
    } else {
        console_reply("radio set (%s): %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B", what, freq, bw, sf, cr,
                      tx8, phs);
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
        *why = mesh_is_stopped() ? "MeshCore stopped for update" : "MeshCore is not running";
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

static void cmd_mesh(char const *args) {
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
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
    if (strcmp(args, "link") == 0) {
        cmd_mesh_link();
        return;
    }
    if (strcmp(args, "chats") == 0) {
        cmd_mesh_chats();
        return;
    }
    if (*args) {
        console_reply("mesh | mesh name <name> | mesh radio <preset index | freq bw sf cr> [txdbm] | mesh link | mesh out | mesh chats | "
                      "mesh send <channel> <text> | mesh dm <12 hex> <text>");
        return;
    }
    mesh_lock();
    uint8_t  key[4];
    char     name[sizeof(node->getNodePrefs()->node_name)];
    int      contacts = node->getNumContacts();
    uint32_t rx = radio_driver.getPacketsRecv(), dropped = lora_radio.dropped;
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
    mesh_settings_t st;
    if (mesh_get_settings(&st)) {
        int preset = radio_preset_match_exact(st.freq, st.bw, st.sf, st.cr, st.path_hash_size);
        console_reply("radio %.3f MHz, %g kHz, SF%u, CR%u, %d dBm, path hash %u B (%s)", st.freq, st.bw, st.sf, st.cr,
                      st.tx_dbm, st.path_hash_size, preset >= 0 ? radio_preset_at((size_t)preset)->title : "custom");
    }
}

static void cmd_contacts(char const *args) {
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
        return;
    }
    (void)args;
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
        if (list[i].path_len == 0xFF) {
            snprintf(hops, sizeof(hops), "flood");
        } else {
            snprintf(hops, sizeof(hops), "%u hops", list[i].path_len);
        }
        console_reply("contact %02x%02x%02x%02x%02x%02x %-32s %-8s %-8s %s", list[i].pubkey[0], list[i].pubkey[1], list[i].pubkey[2],
                      list[i].pubkey[3], list[i].pubkey[4], list[i].pubkey[5], list[i].name,
                      mesh_type_name(list[i].type), hops, age);
    }
    free(list);
    console_reply("%u contact(s)", (unsigned)total);
}

static void cmd_msgs(char const *args) {
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
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
    uint32_t now = mesh_uptime_s();
    for (size_t i = 0; i < n; i++) {
        char age[24];
        mesh_format_age(now - list[i].heard_s, age, sizeof(age));
        console_reply("[%s] %s (%s, rssi %d)", list[i].channel, list[i].text, age, list[i].rssi_x10 / 10);
    }
    free(list);
    console_reply("%u message(s)", (unsigned)n);
}

static void cmd_channels(char const *args) {
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
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
    char const *why = by_index ? mesh_channel_remove((int)index) : mesh_channel_remove_named(arg);
    if (why) {
        console_reply("channel %s not removed: %s", arg, why);
    } else {
        console_reply("channel %s removed", arg);
    }
}

// "fw channel add <name> <base64 key>" or "fw channel add #name": the same as the screen's
static void cmd_channel(char const *args) {
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "rm ", 3) == 0 && args[3]) {
        channel_rm(args + 3);
        return;
    }
    char name[48] = "", key[64] = "";
    if (sscanf(args, "add %47s %63s", name, key) < 1) {
        console_reply("channel add <name> <base64 key> | channel add #name | channel rm <index|name>");
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
    if (mesh_is_stopped()) {
        console_reply("MeshCore stopped for update");
        return;
    }
    (void)args;
    if (char const *why = mesh_advert_refusal(clock_is_set())) {
        console_reply("advert not sent: %s (other nodes would drop it)", why);
        return;
    }
    console_reply(mesh_send_advert() ? "advert queued" : "advert not sent");
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

extern "C" bool mesh_start(void) {
    mesh_lock();
    mesh_store_set_hooks(store_alloc_ring, store_warn);
    bool ok = mesh_store_init(STORE_MESSAGES);
    mesh_unlock();
    void *tables_mem = heap_caps_malloc(sizeof(SimpleMeshTables), MALLOC_CAP_SPIRAM);
    void *mesh_mem   = heap_caps_malloc(sizeof(BadgeMesh), MALLOC_CAP_SPIRAM);
    void *ob_mem     = heap_caps_calloc(1, sizeof(ob_t), MALLOC_CAP_SPIRAM);
    if (!ok || !tables_mem || !mesh_mem || !ob_mem || !SPIFFS.begin(true)) {
        ESP_LOGE(TAG, "no memory or no /storage");
        free(tables_mem);
        free(mesh_mem);
        free(ob_mem);
        return false;
    }
    tables    = new (tables_mem) SimpleMeshTables();
    outbox = (ob_t *)ob_mem;
    ob_init(outbox);
    BadgeMesh *n = new (mesh_mem) BadgeMesh(radio_driver, rng, rtc_clock, *tables, store, outbox);

    // Everything is loaded before node is published: the screens and the console read MeshCore only through node, and
    // must not walk the contact table while begin() fills it (#65 review)
    store.begin();
    migrate_identity();
    bool first_boot = !SPIFFS.exists("/prefs.json") && !SPIFFS.exists("/node_prefs");
    radio_driver.begin();
    n->begin();
    apply_boot_radio(n, first_boot);
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
    __atomic_store_n(&node, n, __ATOMIC_RELEASE);
    mesh_unlock();
    __atomic_store_n(&radio_known, true, __ATOMIC_RELEASE);
    __atomic_store_n(&node_id_known, true, __ATOMIC_RELEASE);

    console_register("mesh", "mesh | mesh name <name> | mesh radio <preset | freq bw sf cr> [txdbm] | mesh link | mesh out | mesh send <channel> <text> | mesh dm <12 hex> <text>", cmd_mesh);
    console_register("contacts", "contacts (nodes heard, newest first)", cmd_contacts);
    console_register("msgs", "msgs [n] (last n messages)", cmd_msgs);
    console_register("channels", "channels (the channels the badge can read)", cmd_channels);
    console_register("channel", "channel add <name> <base64 key> | channel add #name | channel rm <index|name>", cmd_channel);
    console_register("advert", "advert (send a zero-hop advert now)", cmd_advert);

    if (xTaskCreatePinnedToCore(mesh_task, "mesh", MESH_STACK, NULL, 3, &mesh_task_handle, MESH_CORE) != pdPASS) {
        ESP_LOGE(TAG, "mesh task not started");
        return false;
    }
    __atomic_store_n(&task_running, true, __ATOMIC_RELEASE);
    ESP_LOGW(TAG, "MeshCore companion up: %d contacts, BadgeMesh %u bytes in PSRAM", n->getNumContacts(),
             (unsigned)sizeof(BadgeMesh));
    return true;
}
