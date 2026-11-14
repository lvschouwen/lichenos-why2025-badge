// MeshCore on the badge (#61, #65): MeshCore's own companion (examples/companion_radio MyMesh + DataStore) runs in one
// task over the LoRa bridge; the screens and the console are its user interface and read copies taken under
// mesh_lock (no app port: nothing to expose on a network). MyMesh's loop() holds the lock, so its callbacks (the Listener below) already run under it.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

#define MESH_STACK     12288 // Ed25519 checks are deep and MyMesh keeps File objects on the stack; peak 3.1 KB (#107)
// The mesh task is LICHEN_PRIO_APP_LOOP: core 1 with LVGL, its 400-650 ms file bursts stay off core 0, where SDIO,
// tcpip and LoRa live (#91)
#define MESH_POLL_MS   2
#define MESH_SLOW_LOOP_MS 200

static char const TAG[] = "mesh";

// fw soak (#419): the loops the "MeshCore loop took" warning is printed for, since boot; the mesh task writes, any reads
static uint32_t slow_loops, slow_max_ms, slow_file_ms;

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
LoraRadio mesh_int::lora_radio;
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

static EspRng            rng;
static SimpleMeshTables *tables;
DataStore         mesh_int::store(SPIFFS, rtc_clock);
BadgeMesh        *mesh_int::node;
ob_t             *mesh_int::outbox;            // PSRAM; shared with the UI under the outbox lock (mesh_link.h)
volatile int      mesh_int::offline_queue_len; // MyMesh's offline queue, from the Listener (#55: stays ~0 now)

// DataStore::formatFileSystem() (the app's factory reset) erases NVS on ESP32; on the badge that would take the Wi-Fi
// credentials with it, so DataStore.cpp is built with nvs_flash_erase renamed to this: MeshCore's own files go (the
// shim's format()), the rest of NVS stays
extern "C" esp_err_t mesh_nvs_flash_erase(void) {
    ESP_LOGI(TAG, "factory reset from the app: MeshCore's files erased, NVS (Wi-Fi) kept");
    node->dropBlobs(); // only MyMesh formats (in its loop, so node is set): no queued blob comes back (#257)
    return ESP_OK;
}

extern "C" void mesh_ob_enter(void) {
    section_enter<SECTION_OUTBOX>();
}
extern "C" void mesh_ob_exit(void) {
    section_exit<SECTION_OUTBOX>();
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
void mesh_int::mesh_lock(void) {
    xSemaphoreTake(mesh_lock_handle(), portMAX_DELAY);
}
bool mesh_int::mesh_lock_within(uint32_t wait_ms) {
    return xSemaphoreTake(mesh_lock_handle(), wait_ms == MESH_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) ==
           pdTRUE;
}
void mesh_int::mesh_unlock(void) {
    xSemaphoreGive(mesh_lock_handle());
}

// Running or not (#103). A stop keeps node, its tables, the outbox and the store in PSRAM: a start after a stop on
// request only claims the radio again and makes a new task, so nothing is loaded or replayed twice.
TaskHandle_t mesh_int::mesh_task_handle; // valid only while mesh_running()
static bool stop_requested; // mesh_stop -> the task
static bool stop_final;     // with it: the update stop (the journal settles and wipes its key)
static bool task_running;   // set once the first task exists
static bool stopped;        // the task -> everyone: it has saved and is gone (or is about to delete itself)
static bool final_stop;     // stopped for an update: no start again before the restart
static bool task_exited = true; // the task -> mesh_stop / mesh_start: radio released, about to delete itself

bool mesh_int::mesh_is_stopped(void) {
    return __atomic_load_n(&stopped, __ATOMIC_ACQUIRE);
}

// Why a stopped MeshCore refuses, for the setters and senders
char const *mesh_int::stopped_why(void) {
    return __atomic_load_n(&final_stop, __ATOMIC_ACQUIRE) ? "MeshCore stopped for update" : "MeshCore is not running";
}

// The stored identity's check at load (#80, ident_verify): only a passed check transmits
mesh_ident_t mesh_int::ident_state;

bool mesh_int::ident_tx_allowed(void) {
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
uint32_t     mesh_int::tile_advert_s; // uptime + 1 of our last advert, 0 for none (+1: one sent in the first second counts)
portMUX_TYPE mesh_int::tile_mux = portMUX_INITIALIZER_UNLOCKED;

adv_state_t mesh_int::adv; // the periodic advert (#52), under tile_mux: mesh_int.h

// The unread total over all chats, after the store changed (called under the mesh lock)
void mesh_int::tile_publish_unread(void) {
    uint32_t total = mesh_store_unread_where(mesh_notify_counts_unread, NULL); // the number the badge and tile share (#50)
    portENTER_CRITICAL(&tile_mux);
    tile.unread_total = total;
    portEXIT_CRITICAL(&tile_mux);
    // home's count on the icon (#102); its own lock, not inside ours. 0 while stopped (#103): nothing arrives then
    lichen_badge_set(meshcore_ctx(), mesh_is_stopped() ? 0 : total);
}

uint32_t mesh_int::tile_unread(void) {
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
    lichen_badge_set(meshcore_ctx(), total);
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

// MeshCore writes contacts lazily; a firmware update must not lose the last few minutes of them
bool (*const mesh_int::keep_contact)(ContactInfo const &c) = BadgeMesh::keepContact;

// MeshCore's lazy contacts rewrite (#425), DataStore's "/contacts3": on LittleFS a changed chunk rewrites every block
// after it, 0.5-1 s at 552 contacts. While the task runs, the shim holds that file's saves (any task's, all under the
// mesh lock): the loop takes the newest one before its unlock and writes it after, so the mesh task is the one writer
// and the screens never wait for it. A stop or MeshCore's own reboot drops what is held and saves directly.
static char const     CONTACTS_FILE[] = "/contacts3";
static fs_shim_held_t contacts_taken; // the mesh task: taken under the lock, written right after it
static int            contacts_taken_n;

static void contacts_hold_off_locked(void) {
    fs_shim_hold(NULL);
    fs_shim_drop_held(); // the save that follows has the newest state
}

// MeshCore's own reboot (an admin's "reboot" CLI command, the companion protocol) runs on the mesh task inside
// node->loop(), with the mesh lock held. The OS's restart save (system_restart -> stop_for_update, #123) would wait for
// this very task to stop, so the saves a stop makes are made here first, and stop_locked then has nothing to wait for.
// The mesh task -> stop_locked: saved for MeshCore's own reboot. Never cleared: system_restart does not return.
static bool rebooting;

void BadgeBoard::reboot() {
    if (mesh_task_handle && xTaskGetCurrentTaskHandle() == mesh_task_handle) {
        contacts_hold_off_locked(); // the restart comes before the loop would write a held save
        node->saveAll();
        mesh_journal_stop_locked(true);
        __atomic_store_n(&rebooting, true, __ATOMIC_RELEASE);
    }
    // Someone always asked (the phone's reboot or factory reset, an authenticated remote admin's CLI): never waits
    // and never returns, which MeshCore relies on (a factory reset formats first). On trial it goes back to the
    // previous version, and system_restart_asked logs that (#304)
    system_restart_asked("MeshCore reboot");
}

// Our channel messages go into the screens' store as "me: <text>", and their outbox state follows (#46). With the mesh
// lock held (the store is under it). Each pass copies the small states and the first fresh messages' texts in one
// short section under the outbox lock (ob_sync_states, #271); the work is done outside it. A fresh message without a
// text slot stays fresh and gets one within a few passes (round robin), whatever the store does with the others.
static ob_sync_pass_t out_pass; // mesh task only; kept from pass to pass for the round robin of the text slots

// An own message has just ended FAILED (#50): its target and text from this pass's copy, else read again from the
// outbox. One that left the table meanwhile gets no notice: an empty "Not delivered" would say nothing.
static void notify_failed(ob_sync_t const &o) {
    ob_text_t t = {};
    if (o.text_slot >= 0) {
        t = out_pass.text[o.text_slot];
    } else {
        mesh_ob_enter();
        if (outbox) {
            ob_copy_text(outbox, o.id, t.target, t.text);
        }
        mesh_ob_exit();
    }
    if (!t.text[0]) {
        ESP_LOGW(TAG, "outgoing message %lu failed and left the outbox: no notice", (unsigned long)o.id);
        return;
    }
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 2];
    if (o.kind == OB_DM) {
        ContactInfo *c = node ? node->lookupContactByPubKey(o.prefix, MF_PREFIX) : nullptr;
        snprintf(title, sizeof(title), "@%s", c ? c->name : "?");
        mesh_chat_key_dm(&key, o.prefix);
    } else {
        snprintf(title, sizeof(title), "%s", t.target);
        mesh_chat_key_channel(&key, t.target);
    }
    mesh_notify_failed(&key, title, t.text);
}

// A fresh message with its text copied into the screens' store; true when the store took it (it is marked in_store)
static bool sync_add_fresh(ob_sync_t const &o) {
    ob_text_t const &t = out_pass.text[o.text_slot];
    char             line[MESH_TEXT_MAX + 1];
    snprintf(line, sizeof(line), "me: %.*s", (int)(sizeof(line) - 5), t.text); // cut where the plain %s cut it (#46)
    bool            ok;
    mesh_chat_key_t key;
    if (o.kind == OB_DM) {
        char         title[OB_TARGET_MAX];
        ContactInfo *c = node ? node->lookupContactByPubKey(o.prefix, MF_PREFIX) : nullptr;
        snprintf(title, sizeof(title), "@%s", c ? c->name : "?");
        ok = mesh_store_add_outgoing_dm(title, line, o.id, mesh_uptime_s(), o.prefix);
        mesh_chat_key_dm(&key, o.prefix);
    } else {
        ok = mesh_store_add_outgoing(t.target, line, o.id, mesh_uptime_s());
        mesh_chat_key_channel(&key, t.target);
    }
    if (ok) {
        mesh_journal_outgoing_locked(&key); // held until its state is final
        mesh_ob_enter();
        ob_mark_in_store(outbox, o.id); // a no-op when it left the table meanwhile
        mesh_ob_exit();
    }
    return ok;
}

// true when a message was added to the store (which may have evicted a chat, so the unread total can have changed)
static bool sync_outgoing(void) {
    mesh_ob_enter();
    size_t n = outbox ? ob_sync_states(outbox, &out_pass) : 0;
    mesh_ob_exit();
    bool added = false;
    for (size_t i = 0; i < n; i++) {
        ob_sync_t const &o = out_pass.e[i];
        if (o.fresh && o.text_slot >= 0) {
            // in_store only after the store took it: a dropped one is tried again on the next pass (logged once)
            static mesh_msg_id_t failed_logged;
            if (sync_add_fresh(o)) {
                added = true;
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
        ids[i] = out_pass.e[i].id;
    }
    mesh_journal_out_sweep_locked(ids, n);
    return added;
}

static void contacts_write_taken(void) {
    if (!contacts_taken.set) {
        return;
    }
    fs_shim_wrote_t w = File::writeHeld(&contacts_taken);
    ESP_LOGI(TAG, "contacts saved after the unlock: %d contacts, %u B, %u B rewritten, %lu ms%s", contacts_taken_n,
             (unsigned)w.len, (unsigned)w.rewritten, (unsigned long)(w.us / 1000), w.ok ? "" : " (failed)");
}

// The slow loop's file calls of 10 ms and more (#425), as the shim noted them: the first n, the ones under the lock
// (the writes after the unlock come later in the shim's list)
static void log_slow_calls(size_t n) {
    fs_shim_call_t calls[FS_SHIM_SLOW_CALLS];
    fs_shim_slow_calls(calls, FS_SHIM_SLOW_CALLS);
    for (size_t i = 0; i < n && i < FS_SHIM_SLOW_CALLS; i++) {
        fs_shim_call_t const &c = calls[i];
        ESP_LOGW(TAG, "  slow file call: %s %s, %lu B (%lu B rewritten), %lu ms (file system close %lu ms)",
                 fs_shim_op_name(c.op), c.name, (unsigned long)c.bytes, (unsigned long)c.rewritten,
                 (unsigned long)(c.us / 1000), (unsigned long)(c.sync_us / 1000));
    }
    if (n > FS_SHIM_SLOW_CALLS) {
        ESP_LOGW(TAG, "  and %u more slow file call(s)", (unsigned)(n - FS_SHIM_SLOW_CALLS));
    }
}

static void mesh_task(void *arg) {
    (void)arg;
    mesh_lock();
    fs_shim_hold(CONTACTS_FILE);
    mesh_unlock();
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
        uint32_t file_us   = fs_shim_take_busy_us(&file_ops);
        size_t   slow_file = fs_shim_slow_calls(NULL, 0); // a count only: the calls are copied for a slow loop's log
        rx               = radio_driver.getPacketsRecv() - rx;
        tile_set_contacts((size_t)node->getNumContacts());
        advert_tick_locked();
        discover_tick_locked();
        airlog_prepare(); // #408: only after a failed allocation, at most every 30 s
        mesh_rhist_tick(); // the status history's poll (#168): only repeaters the person switched on
        mesh_journal_tick_locked(); // a due flush swaps the pending records out; they are written after the unlock
        node->takeBlob(rx == 0); // an advert's blob (#257): written after the unlock as well, while the radio is quiet
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
                ESP_LOGI(TAG, "stop: %u outgoing message(s) failed, %d queued packet(s) dropped", (unsigned)failed,
                         dropped);
            }
            contacts_hold_off_locked();
            int64_t save_started = esp_timer_get_time();
            store.savePrefs(*node->getNodePrefs());
            int64_t contacts_started = esp_timer_get_time();
            store.saveContacts(node, keep_contact);
            long long contacts_ms = (esp_timer_get_time() - contacts_started) / 1000;
            store.saveChannels(node);
            node->flushBlobs();
            long long save_ms = (esp_timer_get_time() - save_started) / 1000;
            mesh_journal_stop_locked(final);
            __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
            mesh_unlock();
            // Here, not in mesh_stop: a stop that outlasts mesh_stop's wait still gives the radio back (#103)
            lora_radio.end();
            lichen_badge_set(meshcore_ctx(), 0);
            // The contact rewrite is the same one MyMesh does lazily in its loop: its time here says what it costs there
            ESP_LOGI(TAG, "MeshCore stopped%s: prefs, %d contacts, channels saved in %lld ms (contacts %lld ms)",
                     final ? " for the update" : "", node->getNumContacts(), save_ms, contacts_ms);
            __atomic_store_n(&task_exited, true, __ATOMIC_RELEASE);
            return; // lichen_task_start's trampoline takes the task out of the OS's book and deletes it (#181)
        }
        fs_shim_take_held(&contacts_taken);
        contacts_taken_n = node->getNumContacts();
        int64_t took_ms  = (esp_timer_get_time() - started) / 1000;
        mesh_unlock();
        mesh_journal_flush(); // the card's I/O never under the mesh lock: the screens keep their snapshots meanwhile
        node->writeTakenBlob();
        contacts_write_taken();
        contacts_full_post();
        if (mesh_notify_drain()) { // #50: notices posted after the unlock; a mode read or changed moves the badge
            mesh_lock();
            tile_publish_unread();
            mesh_unlock();
        }
        if (took_ms >= MESH_SLOW_LOOP_MS) {
            __atomic_store_n(&slow_loops, slow_loops + 1, __ATOMIC_RELAXED);
            if ((uint32_t)took_ms > slow_max_ms) {
                __atomic_store_n(&slow_max_ms, (uint32_t)took_ms, __ATOMIC_RELAXED);
            }
            if (file_us / 1000 > slow_file_ms) {
                __atomic_store_n(&slow_file_ms, (uint32_t)(file_us / 1000), __ATOMIC_RELAXED);
            }
            // Where it went (#67): file time is DataStore's (prefs, channels; advert blobs since #257 and the contacts
            // rewrite since #425 are written after the unlock), the rest is MeshCore's own
            // work on the packets received (Ed25519 checks of adverts, decryption); the screens skip ticks meanwhile
            ESP_LOGW(TAG, "MeshCore loop took %lld ms: files %lu ms (%lu calls), %lu packet(s) in, waited %lld ms for "
                          "the lock, %d contacts",
                     (long long)took_ms, (unsigned long)(file_us / 1000), (unsigned long)file_ops, (unsigned long)rx,
                     (long long)((started - asked) / 1000), contacts_taken_n);
            log_slow_calls(slow_file);
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_POLL_MS));
    }
}

extern "C" bool mesh_running(void) {
    return __atomic_load_n(&task_running, __ATOMIC_ACQUIRE) && !mesh_is_stopped();
}

extern "C" bool mesh_slow_counts(mesh_slow_counts_t *out) {
    if (!mesh_running()) {
        return false;
    }
    out->slow_loops   = __atomic_load_n(&slow_loops, __ATOMIC_RELAXED);
    out->slow_max_ms  = __atomic_load_n(&slow_max_ms, __ATOMIC_RELAXED);
    out->slow_file_ms = __atomic_load_n(&slow_file_ms, __ATOMIC_RELAXED);
    mesh_journal_stats(&out->journal_pending_hw, &out->journal_dropped);
    return true;
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
        ESP_LOGW(TAG, "MeshCore busy starting or stopping: not stopped");
        return false;
    }
    bool ok = stop_locked(why);
    xSemaphoreGive(run_lock_handle());
    return ok;
}

// The stored identity's check (#80): once, at the first load. A mismatch, or a file MyMesh could not read (#109),
// blocks every transmit until a restart, at the senders (nothing is queued to fail later) and at the radio (MeshCore's
// own ACKs, paths and adverts). The file is never rewritten or regenerated here: it may be the only copy of the identity.
void mesh_int::ident_apply(mesh_ident_t state, bool simulated) {
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
    lichen_notify(meshcore_ctx(), LN_ERROR,
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
        mesh_prefs_set_path_hash(prefs, MESH_PATH_HASH_DEFAULT); // MeshCore's own default is 1 B (#129)
        n->savePrefs();
    }
    radio_driver.setParams(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
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

// The mesh task, new each start; false when it could not be made
static bool task_create(void) {
    // Before the create: the new task must never see the last run's stopped
    bool was_running = __atomic_load_n(&task_running, __ATOMIC_ACQUIRE);
    __atomic_store_n(&stop_requested, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task_exited, false, __ATOMIC_RELEASE);
    __atomic_store_n(&stopped, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task_running, true, __ATOMIC_RELEASE);
    if (!lichen_task_start(meshcore_ctx(), "mesh", mesh_task, NULL, MESH_STACK, LICHEN_PRIO_APP_LOOP,
                           &mesh_task_handle)) {
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
    mesh_store_set_hooks(mesh_mem_ring_alloc, mesh_mem_free, store_warn);
    fs_shim_set_buffer_hooks(mesh_mem_file_alloc, mesh_mem_free);
    mesh_store_set_clock(mesh_epoch_now);
    bool ok = mesh_store_init(STORE_MESSAGES);
    mesh_unlock();
    void *tables_mem = lichen_alloc(meshcore_ctx(), sizeof(SimpleMeshTables), LICHEN_MEM_BULK);
    void *mesh_mem   = lichen_alloc(meshcore_ctx(), sizeof(BadgeMesh), LICHEN_MEM_BULK);
    void *ob_mem     = lichen_alloc(meshcore_ctx(), sizeof(ob_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    void *mt_mem     = lichen_alloc(meshcore_ctx(), sizeof(mt_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    void *rr_mem     = lichen_alloc(meshcore_ctx(), sizeof(rr_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    bool  files      = SPIFFS.begin(true);
    if (!files) {
        mesh_identity_no_files();
    }
    if (!ok || !tables_mem || !mesh_mem || !ob_mem || !mt_mem || !rr_mem || !files) {
        ESP_LOGE(TAG, "no memory or no /storage");
        lichen_free(meshcore_ctx(), tables_mem);
        lichen_free(meshcore_ctx(), mesh_mem);
        lichen_free(meshcore_ctx(), ob_mem);
        lichen_free(meshcore_ctx(), mt_mem);
        lichen_free(meshcore_ctx(), rr_mem);
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
    tl_rate_init(&telem_rate);

    // Everything is loaded before node is published: the screens and the console read MeshCore only through node, and
    // must not walk the contact table while begin() fills it (#65 review)
    adv_load();
    store.begin();
    bool first_boot = !SPIFFS.exists("/prefs.json") && !SPIFFS.exists("/node_prefs");
    radio_driver.begin();
    n->begin();
    ident_verify(n); // before anything can transmit: the task does not exist yet
    apply_boot_radio(n, first_boot);
    n->loadConsoleScope(); // #290: before the task runs, so the first console flood already has it
    mesh_cscope_note(n->consoleScope());
    n->loadChannelScopes(); // #291: after begin() loaded the channels
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
    airlog_prepare(); // #408: before anything can transmit, so no note ever allocates
    n->startPipe(); // connected, so MyMesh's replies and pushes reach us (#45)
    uint8_t const *key = n->self_id.pub_key;
    snprintf(node_id_text, sizeof(node_id_text), "%02x%02x%02x%02x", key[0], key[1], key[2], key[3]);
    mesh_lock();
    settings_from_node(n);
    portENTER_CRITICAL(&tile_mux);
    tile.unread_total = 0; // nothing unread at boot
    tile.contacts     = (size_t)n->getNumContacts();
    portEXIT_CRITICAL(&tile_mux);
    lichen_badge_set(meshcore_ctx(), 0);
    tile_seed_latest(); // the history's newest line; tile_heard_s stays 0, so no "just now" for an old message
    __atomic_store_n(&node, n, __ATOMIC_RELEASE);
    mesh_unlock();
    __atomic_store_n(&node_id_known, true, __ATOMIC_RELEASE);

    if (!task_create()) {
        // As resume_locked: the claim radio_driver.begin() took is released, so another owner (or the next start,
        // which resumes the node published above) can have the radio
        lora_radio.end();
        return false;
    }
    ESP_LOGI(TAG, "MeshCore companion up: %d contacts, BadgeMesh %u bytes in PSRAM, identity %s", n->getNumContacts(),
             (unsigned)sizeof(BadgeMesh), mesh_ident_text(__atomic_load_n(&ident_state, __ATOMIC_ACQUIRE)));
    return true;
}
