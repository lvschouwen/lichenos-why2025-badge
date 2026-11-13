// MeshCore's settings (#67) and our adverts, location and auto-add (#52).
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"

using namespace mesh_int;

static char const TAG[] = "mesh";

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

// Under the mesh lock: the copies the screens read
void mesh_int::settings_from_node(MyMesh *n) {
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
    // Read before the spinlock: the prefs are in PSRAM
    bool             share = prefs->advert_loc_policy != ADVERT_LOC_NONE;
    advcfg_autoadd_t autoadd;
    mesh_prefs_get_autoadd(prefs, &autoadd);
    portENTER_CRITICAL(&settings_mux);
    settings_cache = s;
    acfg_cache.lat     = sensors.node_lat; // what an advert carries, and what savePrefs stores
    acfg_cache.lon     = sensors.node_lon;
    acfg_cache.share   = share;
    acfg_cache.autoadd = autoadd;
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
char const *mesh_int::settings_lock(void) {
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
        ESP_LOGI(TAG, "radio set: %.3f MHz, %.2f kHz, SF%u, CR%u, %d dBm", freq, bw, sf, cr, tx_dbm);
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

extern "C" char const *mesh_set_console_scope(char const *name) {
    char const *why = settings_lock();
    if (!why) {
        why = node->setConsoleScope(name);
        mesh_cscope_note(node->consoleScope());
        mesh_unlock();
    }
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
// ---- our adverts (#52) ---------------------------------------------------------------------------------------------

// The advert keys live in the app's settings (a.meshcore)
static lichen_ctx_t adv_app(void) {
    return meshcore_ctx();
}

// At start, before the task runs
void mesh_int::adv_load(void) {
    lichen_ctx_t app      = adv_app();
    uint32_t     interval = app_settings_get_u32(app, ADV_KEY_INTERVAL, ADVCFG_INTERVAL_DEFAULT);
    uint32_t     last     = app_settings_get_u32(app, ADV_KEY_LAST, 0);
    uint8_t      flood    = app_settings_get_u8(app, ADV_KEY_FLOOD, 0);
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
static char const *advert_tx_locked(bool flood, mesh_origin_t from = MESH_FROM_SCREEN) {
    if (char const *why = flood ? mesh_cscope_refusal(from == MESH_FROM_CONSOLE) : NULL) {
        return why; // #290: a console line's flood advert needs the console scope
    }
    if (char const *why = mesh_onair_refusal(from == MESH_FROM_CONSOLE, ONAIR_TX_ADVERT, NULL, NULL)) {
        return why; // #289: an advert has no target; the allow list lets it pass (mesh_onair.h), decided there
    }
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
    if (!node->advertNow(flood, from == MESH_FROM_CONSOLE)) {
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
void mesh_int::advert_tick_locked(void) {
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

static bool send_advert_mode(bool flood, mesh_origin_t from = MESH_FROM_SCREEN) {
    char const *why = settings_lock();
    if (!why) {
        why = advert_tx_locked(flood, from);
        mesh_unlock();
    }
    if (why) {
        ESP_LOGW(TAG, "advert not sent: %s", why);
    }
    return why == NULL;
}

extern "C" bool mesh_send_advert_mode(bool flood, mesh_origin_t from) {
    return send_advert_mode(flood, from);
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
void mesh_int::contacts_full_post(void) {
    if (__atomic_exchange_n(&contacts_full_pending, false, __ATOMIC_ACQ_REL)) {
        lichen_notify_to(meshcore_ctx(), LN_WARN, "contacts-full", "Contacts full",
                         "New nodes are not added. Remove contacts, or turn on Overwrite oldest in Auto-add.",
                         LN_COALESCE);
    }
}
