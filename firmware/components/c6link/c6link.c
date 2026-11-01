#include "c6link.h"

#include "boot_progress.h"
#include "c6_boot_policy.h"
#include "c6link_priv.h"
#include "c6_flasher.h"
#include "c6_paths.h"
#include "fw_console.h"
#include "lichen_clock.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "wifi_join.h"
#include "wifi_list.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include <sys/stat.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static char const TAG[] = "c6link";

// Written by the boot attach and the supervisor (c6_sup.c, #14); read from any task
static atomic_bool  link_up;
// Shared between the event handler, the c6wifi task, the console and the screen: guarded by state_lock. The task is the
// only one that calls esp_wifi_connect/disconnect/set_config after boot.
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         have_credentials;
static wifi_join_t  join;
static char         trial_ssid[33]; // the pair being tried (its password lives only in the task, trial_pass)
static char         pending_ssid[33]; // asked for, not started yet
static char         kept_ssid[33];  // after a failed trial: the pair that came back ("" if none was saved)
static esp_netif_t *sta_netif;
static char         ssid[33];
static SemaphoreHandle_t scan_done; // given by the SCAN_DONE event
static SemaphoreHandle_t scan_lock; // one scan at a time (the screen and the console may ask together)
static portMUX_TYPE ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char         ip_text[16];

bool c6link_up(void) {
    return atomic_load(&link_up);
}

void c6link_set_link(bool up) {
    atomic_store(&link_up, up);
}

bool wifi_has_ip(char *ip, size_t cap) {
    portENTER_CRITICAL(&ip_lock);
    bool have = ip_text[0] != '\0';
    if (have && cap) {
        snprintf(ip, cap, "%s", ip_text);
    }
    portEXIT_CRITICAL(&ip_lock);
    return have;
}

bool wifi_has_credentials(void) {
    portENTER_CRITICAL(&state_lock);
    bool have = have_credentials;
    portEXIT_CRITICAL(&state_lock);
    return have;
}

// The signal strength, read by the c6wifi task (esp_wifi is its alone, never the LVGL task's) and cached for the status bar
static bool   rssi_known;
static int8_t rssi_cached;
// The first connect after boot has neither got an IP nor failed yet: the status bar shows it as joining
static bool first_connect_settled;

static void set_ip(char const *text) {
    portENTER_CRITICAL(&ip_lock);
    snprintf(ip_text, sizeof(ip_text), "%s", text);
    portEXIT_CRITICAL(&ip_lock);
    if (!text[0]) {
        portENTER_CRITICAL(&state_lock);
        rssi_known = false;
        portEXIT_CRITICAL(&state_lock);
    }
}

bool wifi_rssi(int8_t *out) {
    portENTER_CRITICAL(&state_lock);
    bool known = rssi_known;
    if (known && out) {
        *out = rssi_cached;
    }
    portEXIT_CRITICAL(&state_lock);
    return known;
}

bool wifi_joining(void) {
    portENTER_CRITICAL(&state_lock);
    bool joining = join.state == WIFI_JOIN_PENDING || join.state == WIFI_JOIN_TRYING ||
                   (atomic_load(&link_up) && have_credentials && !first_connect_settled);
    portEXIT_CRITICAL(&state_lock);
    return joining;
}

// Everything that touches the connection runs in one task (c6wifi): events and requests are queued to it
typedef enum {
    MSG_CONNECT,
    MSG_JOIN,
    MSG_RECONNECT,
    MSG_FORGET,
    MSG_DISCONNECTED,
    MSG_GOT_IP,
    MSG_CONNECTED,
    MSG_C6_DOWN, // the supervisor re-attaches (#14): stop and deinit the driver
    MSG_C6_UP,   // and it is back: init and start it again
} msg_type_t;
typedef struct {
    msg_type_t type;
    int        reason;
    char       ssid[33];
    char       pass[64];
} msg_t;

static QueueHandle_t queue;

static wifi_link_t link_ap; // c6wifi task only: the SSID the C6 says it is connected to (#15)

// Called from the event loop task, so a short wait is fine; a dropped GOT_IP would roll a good trial back
static bool post(msg_t const *m) {
    bool ok = queue && xQueueSend(queue, m, pdMS_TO_TICKS(50)) == pdTRUE;
    if (!ok && queue) {
        ESP_LOGW(TAG, "wifi: queue full, message %d dropped", (int)m->type);
    }
    return ok;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    msg_t m = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        m.type = MSG_CONNECT;
        post(&m);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xSemaphoreGive(scan_done);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t const *up = data;
        if (up) {
            unsigned len = up->ssid_len > 32 ? 32 : up->ssid_len; // not terminated
            memcpy(m.ssid, up->ssid, len);
            m.ssid[len] = '\0';
            m.type      = MSG_CONNECTED;
            post(&m);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t const *gone = data;
        set_ip("");
        m.type   = MSG_DISCONNECTED;
        m.reason = gone ? gone->reason : 0;
        post(&m);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t const *got = data;
        char                     text[16];
        snprintf(text, sizeof(text), IPSTR, IP2STR(&got->ip_info.ip));
        set_ip(text);
        ESP_LOGW(TAG, "wifi: got ip:%s", text);
        m.type = MSG_GOT_IP;
        post(&m);
    }
}

// The NVS flag "fw c6 reflash" sets; read and cleared here so a forced reflash happens once
static bool take_force_flag(void) {
    nvs_handle_t nvs;
    uint8_t      force = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(nvs, "c6_force", &force) == ESP_OK) {
        nvs_erase_key(nvs, "c6_force");
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return force != 0;
}

static bool load_credentials(wifi_config_t *cfg) {
    char s[33], p[64];
    if (!wifi_cred_load(s, p)) {
        return false;
    }
    // The config's SSID field is 32 bytes without a terminator for a 32-byte name
    memcpy(cfg->sta.ssid, s, strlen(s));
    memcpy(cfg->sta.password, p, strlen(p));
    if (p[0]) {
        cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // a password was set: never join an open AP of that name
    }
    memset(p, 0, sizeof(p));
    return true;
}

static void on_time_sync(struct timeval *tv) {
    ESP_LOGW(TAG, "clock set from SNTP: %lld", (long long)tv->tv_sec);
    boot_progress_end(BOOT_CLOCK, STEP_DONE, NULL);
}

// lwIP's own sntp_sync_time() is weak and calls settimeofday() before the sync callback, so a filter in the callback
// would be too late (#8). This one checks the answer first: an implausible or far-off time is logged and ignored.
void sntp_sync_time(struct timeval *tv) {
    clock_verdict_t v = clock_offer_sntp(tv);
    if (!clock_accepted(v)) {
        ESP_LOGW(TAG, "SNTP time %lld ignored: %s", (long long)tv->tv_sec, clock_verdict_text(v));
        return;
    }
    clock_sync_info_t info = clock_sync_info();
    if (v == CLOCK_RECOVERED) {
        ESP_LOGW(TAG, "SNTP: %s, clock moved", clock_verdict_text(v));
    }
    ESP_LOGW(TAG, "SNTP step %+lld ms after %lld s uptime", (long long)info.step_ms,
             (long long)(info.step_mono_ms / 1000));
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    on_time_sync(tv);
}

// Real time for MeshCore's adverts and messages (#65): other nodes drop adverts that aren't newer than the last they
// saw. lwIP's SNTP client keeps retrying until the station has an IP, then refreshes hourly.
static void start_sntp(void) {
    esp_sntp_config_t cfg  = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb            = on_time_sync;
    esp_err_t err          = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP: %s", esp_err_to_name(err));
    }
}

// Wi-Fi did not start, so the clock will not be set either
static bool wifi_failed(char const *reason) {
    boot_progress_end(BOOT_WIFI, STEP_FAILED, reason);
    boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no Wi-Fi");
    return false;
}

// Once after an update that moved the credentials to NVS badge_fw: whatever the C6's driver persisted earlier (BadgeVMS,
// older firmware) is wiped. The restore also resets the driver's storage mode, so it comes before set_storage(RAM).
static void wipe_driver_store_once(void) {
    nvs_handle_t nvs;
    uint8_t      done = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_u8(nvs, "wifi_drv_wiped", &done) != ESP_OK || !done) {
        esp_err_t err = esp_wifi_restore();
        if (err == ESP_OK) {
            nvs_set_u8(nvs, "wifi_drv_wiped", 1);
            nvs_commit(nvs);
        } else {
            ESP_LOGW(TAG, "Wi-Fi driver restore: %s", esp_err_to_name(err));
        }
    }
    nvs_close(nvs);
}

typedef enum { DRV_STARTED, DRV_NO_CREDENTIALS, DRV_FAILED } drv_result_t;
static drv_result_t wifi_driver_up(char const **why);

static bool start_wifi(void) {
    boot_progress_begin(BOOT_WIFI);
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return wifi_failed("event loop");
    }
    if ((err = esp_netif_init()) != ESP_OK) {
        ESP_LOGE(TAG, "netif: %s", esp_err_to_name(err));
        return wifi_failed("netif");
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    sta_netif = esp_netif_create_default_wifi_sta();
    start_sntp();
    // The boot steps are ended here, not in wifi_driver_up: a re-attach runs it again without them (#14)
    char const  *why = NULL;
    drv_result_t r   = wifi_driver_up(&why);
    if (r == DRV_FAILED) {
        return wifi_failed(why);
    }
    if (r == DRV_NO_CREDENTIALS) {
        boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "no credentials");
        boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no Wi-Fi");
        return true;
    }
    // Associating, the IP and the clock come later and are not waited for; the clock step ends in on_time_sync()
    boot_progress_end(BOOT_WIFI, STEP_DONE, NULL);
    return true;
}

static atomic_bool driver_up; // esp_wifi is initialised and started on the C6 that is attached now (#14)

// The part of the Wi-Fi bring-up that belongs to one ESP-Hosted attach (#14): a re-attach after a C6 reset runs it
// again (MSG_C6_UP), while the event loop, netif, handlers and SNTP above stay from the first boot
static drv_result_t wifi_driver_up(char const **why) {
    esp_err_t          err;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    *why                    = "Wi-Fi init";
    if ((err = esp_wifi_init(&init)) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return DRV_FAILED;
    }
    wipe_driver_store_once();
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return DRV_FAILED;
    }
    // The credentials are ours (NVS badge_fw): the driver must never keep a copy in its own flash storage, so that
    // "fw wifi --clear" really forgets them
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi storage: %s", esp_err_to_name(err));
    }
    wifi_config_t cfg = {0};
    bool          have = load_credentials(&cfg);
    portENTER_CRITICAL(&state_lock);
    have_credentials = have;
    portEXIT_CRITICAL(&state_lock);
    if (!have) {
        ESP_LOGW(TAG, "no Wi-Fi credentials, set them in Settings or with: fw wifi <ssid> <password>");
        // The radio still starts (without connecting) so that the badge can scan and join from Settings.
        // driver_up goes true BEFORE esp_wifi_start: STA_START is handled by the c6wifi task (prio 5) before the
        // boot task gets back from esp_wifi_start, and MSG_CONNECT is dropped while driver_up is false
        atomic_store(&driver_up, true);
        if ((err = esp_wifi_start()) != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
            atomic_store(&driver_up, false);
        }
        return DRV_NO_CREDENTIALS;
    }
    portENTER_CRITICAL(&state_lock);
    snprintf(ssid, sizeof(ssid), "%.32s", (char const *)cfg.sta.ssid);
    portEXIT_CRITICAL(&state_lock);
    atomic_store(&driver_up, true); // before esp_wifi_start, see above
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &cfg)) != ESP_OK || (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        atomic_store(&driver_up, false);
        *why = "Wi-Fi start";
        return DRV_FAILED;
    }
    return DRV_STARTED;
}

// ---- scan, join, reconnect (#66) ---------------------------------------------------------------------------------

#define SCAN_RECORDS  32
#define SCAN_WAIT_MS  6000

static atomic_bool updating; // a firmware update is being received: no scans (#76); read by c6sup too (#14)

void c6link_set_updating(void) {
    atomic_store(&updating, true);
}

bool c6link_updating(void) {
    return atomic_load(&updating);
}

bool wifi_scan(wifi_ap_t *out, size_t max, size_t *n) {
    *n = 0;
    if (atomic_load(&updating) || !atomic_load(&link_up) || !out || max == 0 || !scan_lock || xSemaphoreTake(scan_lock, pdMS_TO_TICKS(SCAN_WAIT_MS + 2000)) != pdTRUE) {
        return false;
    }
    bool                 ok = false;
    wifi_scan_config_t   sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 200;
    xSemaphoreTake(scan_done, 0);
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
    } else if (xSemaphoreTake(scan_done, pdMS_TO_TICKS(SCAN_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "scan did not finish in %d ms", SCAN_WAIT_MS);
        esp_wifi_scan_stop();
    } else {
        uint16_t         count = SCAN_RECORDS;
        wifi_ap_record_t *recs = calloc(SCAN_RECORDS, sizeof(*recs));
        wifi_ap_t        *aps  = calloc(SCAN_RECORDS, sizeof(*aps));
        if (recs && aps) {
            // Fetching also frees the driver's list, even if we use none of it
            if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
                for (uint16_t i = 0; i < count; i++) {
                    size_t len = strnlen((char const *)recs[i].ssid, 32);
                    memcpy(aps[i].ssid, recs[i].ssid, len);
                    aps[i].ssid[len] = '\0';
                    aps[i].rssi      = recs[i].rssi;
                    aps[i].open      = recs[i].authmode == WIFI_AUTH_OPEN;
                }
                size_t clean = wifi_list_clean(aps, count);
                *n           = clean < max ? clean : max;
                memcpy(out, aps, *n * sizeof(*out));
                ok = true;
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(recs);
        free(aps);
    }
    xSemaphoreGive(scan_lock);
    return ok;
}

// ---- the c6wifi task: joining with a trial, reconnecting, forgetting ---------------------------------------------

#define RETRY_MS 2000

static char     trial_pass[64];
static bool     retry_pending;
static uint32_t retry_at;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Log-safe copy of an SSID (they come from the air)
static void clean(char const *in, char *out, size_t cap) {
    wifi_ssid_sanitize(in, out, cap);
}

static bool apply_config(char const *s, char const *p) {
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, s, strlen(s));
    memcpy(cfg.sta.password, p, strlen(p));
    if (p[0]) {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // see load_credentials
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi config: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

// The previous pair is still what NVS holds: put it back in the driver
static void rollback(void) {
    char s[33], p[64];
    bool had = wifi_cred_load(s, p);
    memset(trial_pass, 0, sizeof(trial_pass));
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    portENTER_CRITICAL(&state_lock);
    join.state       = WIFI_JOIN_FAILED_KEPT_OLD;
    have_credentials = had;
    snprintf(kept_ssid, sizeof(kept_ssid), "%s", had ? s : "");
    snprintf(ssid, sizeof(ssid), "%s", had ? s : "");
    portEXIT_CRITICAL(&state_lock);
    if (had && apply_config(s, p)) {
        esp_wifi_connect();
    }
    char a[140], b[140];
    clean(trial_ssid, a, sizeof(a));
    clean(s, b, sizeof(b));
    ESP_LOGW(TAG, "wifi: could not join \"%s\", %s%s%s", a, had ? "back to \"" : "no network saved", had ? b : "",
             had ? "\"" : "");
    memset(p, 0, sizeof(p));
}

static void handle_join(msg_t const *m, uint32_t now) {
    memcpy(trial_pass, m->pass, sizeof(trial_pass));
    portENTER_CRITICAL(&state_lock);
    wifi_join_start(&join, now);
    snprintf(trial_ssid, sizeof(trial_ssid), "%s", m->ssid);
    have_credentials = true;
    snprintf(ssid, sizeof(ssid), "%s", m->ssid);
    portEXIT_CRITICAL(&state_lock);
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    if (apply_config(m->ssid, m->pass)) {
        esp_wifi_connect();
    } else {
        rollback();
    }
}

static void handle_reconnect(void) {
    char s[33], p[64];
    portENTER_CRITICAL(&state_lock);
    bool trying = join.state == WIFI_JOIN_TRYING || join.state == WIFI_JOIN_PENDING;
    portEXIT_CRITICAL(&state_lock);
    if (trying || !wifi_cred_load(s, p)) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    have_credentials = true;
    snprintf(ssid, sizeof(ssid), "%s", s);
    portEXIT_CRITICAL(&state_lock);
    set_ip("");
    esp_wifi_disconnect();
    if (apply_config(s, p)) {
        esp_wifi_connect();
    }
    memset(p, 0, sizeof(p));
}

static void handle_forget(void) {
    wifi_cred_clear();
    portENTER_CRITICAL(&state_lock);
    have_credentials = false;
    ssid[0]          = '\0';
    wifi_join_init(&join);
    portEXIT_CRITICAL(&state_lock);
    memset(trial_pass, 0, sizeof(trial_pass));
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    apply_config("", ""); // the driver's RAM copy goes too
    // Whatever the C6's driver persisted in its own flash earlier (BadgeVMS days, older firmware) goes as well. This
    // resets the driver's other settings to defaults, which is why it only runs here and once after an update.
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi driver restore: %s", esp_err_to_name(err));
    }
    // The restore put the driver's defaults back: flash storage and its own mode
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void handle_got_ip(void) {
    // Only an IP on the trial's own network proves its password: a late GOT_IP of the old connection must not save
    // the new pair. The SSID comes from the CONNECTED event (no RPC that could fail); never from esp_wifi_get_config,
    // which already holds the trial pair
    portENTER_CRITICAL(&state_lock);
    first_connect_settled = true;
    wifi_join_event_t  ev;
    wifi_join_action_t act   = wifi_join_got_ip(&join, &link_ap, trial_ssid, now_ms(), &ev);
    bool               trial = join.state == WIFI_JOIN_TRYING;
    portEXIT_CRITICAL(&state_lock);
    if (act == JOIN_ACT_COMMIT) {
        // Only a pair that works is saved: until now NVS held the previous one
        if (!wifi_cred_save(trial_ssid, trial_pass)) {
            ESP_LOGW(TAG, "wifi: joined, but saving the credentials failed");
        }
        memset(trial_pass, 0, sizeof(trial_pass));
    } else if (trial && ev == JOIN_EV_GOT_IP_OTHER) {
        ESP_LOGW(TAG, "wifi: an IP, but not on the network being tried: not saved, still trying");
    }
}

static void handle_connected(msg_t const *m) {
    wifi_link_connected(&link_ap, m->ssid, strlen(m->ssid));
}

static void handle_disconnected(int reason, uint32_t now) {
    wifi_link_down(&link_ap);
    portENTER_CRITICAL(&state_lock);
    first_connect_settled = true;
    wifi_join_action_t act  = wifi_join_event(&join, JOIN_EV_DISCONNECTED, reason, now);
    bool               have = have_credentials;
    portEXIT_CRITICAL(&state_lock);
    if (act == JOIN_ACT_ROLLBACK) {
        rollback();
    } else if (have) {
        // Keep trying for as long as the badge runs: the network may come back (out of range, router restart)
        retry_pending = true;
        retry_at      = now + RETRY_MS;
    }
}

#define RSSI_POLL_MS 5000

// Reads the AP's signal every 5 s while connected; skipped during a firmware update (the C6 link is busy with the image)
static void poll_rssi(uint32_t now) {
    static uint32_t next_at;
    if ((int32_t)(now - next_at) < 0) {
        return;
    }
    next_at = now + RSSI_POLL_MS;
    wifi_ap_record_t ap;
    if (atomic_load(&updating) || !wifi_has_ip(NULL, 0) || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    rssi_cached = ap.rssi;
    rssi_known  = true;
    portEXIT_CRITICAL(&state_lock);
}

// A trial that was running when the C6 went away has failed: the pair NVS holds stays, nothing is saved, and no driver
// call is made (the C6 is gone, each one would wait for its RPC timeout)
static void abort_trial(void) {
    portENTER_CRITICAL(&state_lock);
    bool trying = join.state == WIFI_JOIN_TRYING || join.state == WIFI_JOIN_PENDING;
    if (trying) {
        join.state = WIFI_JOIN_FAILED_KEPT_OLD;
    }
    portEXIT_CRITICAL(&state_lock);
    if (!trying) {
        return;
    }
    char s[33], p[64];
    bool had = wifi_cred_load(s, p);
    memset(p, 0, sizeof(p));
    memset(trial_pass, 0, sizeof(trial_pass));
    portENTER_CRITICAL(&state_lock);
    have_credentials = had;
    snprintf(kept_ssid, sizeof(kept_ssid), "%s", had ? s : "");
    snprintf(ssid, sizeof(ssid), "%s", had ? s : "");
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGW(TAG, "wifi: the C6 went away during a trial join: not saved");
}

// The supervisor re-attaches (#14 T-B): forget the connection and take the driver down. Errors are expected (the C6
// may be gone) and ignored; each RPC gives up after its own timeout.
static void handle_c6_down(void) {
    // A scan from the screen or the console may be inside the driver: let it end first (it gives up after 6 s)
    bool scan_held = scan_lock && xSemaphoreTake(scan_lock, pdMS_TO_TICKS(SCAN_WAIT_MS + 2000)) == pdTRUE;
    atomic_store(&driver_up, false);
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    abort_trial();
    esp_err_t stop = esp_wifi_stop();
    // A dead C6 never answers the stop, so WIFI_EVENT_STA_DISCONNECTED/STA_STOP never come and the default handlers
    // never take the station netif down and out of lwIP; the STA_START of the re-attach then adds it a second time
    // ("netif_add: netif already added", bench 2026-11-01). Run the two default actions ourselves; if the events do
    // come later (live C6, fw c6 reattach) the handlers find the netif gone and do nothing
    esp_netif_action_disconnected(sta_netif, NULL, 0, NULL);
    esp_netif_action_stop(sta_netif, NULL, 0, NULL);
    esp_err_t gone = esp_wifi_deinit();
    ESP_LOGW(TAG, "wifi: down for the C6 re-attach (stop %s, deinit %s)", esp_err_to_name(stop), esp_err_to_name(gone));
    if (scan_held) {
        xSemaphoreGive(scan_lock); // wifi_scan refuses on its own while link_up is false
    }
}

// The C6 is attached again: the driver comes up as at boot; STA_START then connects with the saved pair
static void handle_c6_up(void) {
    portENTER_CRITICAL(&state_lock);
    first_connect_settled = false; // the status bar shows it as joining again
    portEXIT_CRITICAL(&state_lock);
    char const  *why = NULL;
    drv_result_t r   = DRV_FAILED;
    // A C6 that has only just booted may refuse the first RPCs: two more tries before giving up until the next outage
    for (int i = 0; i < 3 && (r = wifi_driver_up(&why)) == DRV_FAILED; i++) {
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "wifi: %s after the C6 re-attach", r == DRV_FAILED ? why : r == DRV_NO_CREDENTIALS ? "started, no network saved" : "started");
}

static SemaphoreHandle_t c6_msg_done; // given by the task after MSG_C6_DOWN / MSG_C6_UP
static atomic_int        c6_msg_seq;  // the request the last give was for: a late one must not end the next wait

bool c6link_wifi_request(bool up, uint32_t wait_ms) {
    static int seq;
    msg_t      m = {.type = up ? MSG_C6_UP : MSG_C6_DOWN, .reason = ++seq};
    if (!c6_msg_done || !post(&m)) {
        return false;
    }
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
    while (atomic_load(&c6_msg_seq) != m.reason) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0 || xSemaphoreTake(c6_msg_done, until - now) != pdTRUE) {
            return atomic_load(&c6_msg_seq) == m.reason;
        }
    }
    return true;
}

static TaskHandle_t wifi_task_handle;

unsigned c6link_wifi_stack_free(void) {
    return wifi_task_handle ? (unsigned)uxTaskGetStackHighWaterMark(wifi_task_handle) : 0;
}

static void wifi_task(void *arg) {
    (void)arg;
    msg_t m;
    for (;;) {
        bool     got = xQueueReceive(queue, &m, pdMS_TO_TICKS(250)) == pdTRUE;
        uint32_t now = now_ms();
        if (got) {
            switch (m.type) {
                case MSG_CONNECT: {
                    portENTER_CRITICAL(&state_lock);
                    bool have = have_credentials;
                    portEXIT_CRITICAL(&state_lock);
                    if (have && atomic_load(&driver_up)) {
                        esp_wifi_connect();
                    }
                    break;
                }
                case MSG_JOIN: handle_join(&m, now); break;
                case MSG_RECONNECT: handle_reconnect(); break;
                case MSG_FORGET: handle_forget(); break;
                case MSG_DISCONNECTED: handle_disconnected(m.reason, now); break;
                case MSG_GOT_IP: handle_got_ip(); break;
                case MSG_CONNECTED: handle_connected(&m); break;
                case MSG_C6_DOWN:
                case MSG_C6_UP:
                    m.type == MSG_C6_UP ? handle_c6_up() : handle_c6_down();
                    atomic_store(&c6_msg_seq, m.reason);
                    xSemaphoreGive(c6_msg_done);
                    break;
            }
            memset(&m, 0, sizeof(m));
        }
        if (retry_pending && (int32_t)(now - retry_at) >= 0) {
            retry_pending = false;
            if (atomic_load(&driver_up)) {
                esp_wifi_connect();
            }
        }
        poll_rssi(now);
        portENTER_CRITICAL(&state_lock);
        wifi_join_action_t act = wifi_join_event(&join, JOIN_EV_TICK, 0, now);
        portEXIT_CRITICAL(&state_lock);
        if (act == JOIN_ACT_ROLLBACK) {
            rollback();
        }
    }
}

bool wifi_set_credentials(char const *new_ssid, char const *pass) {
    if (!atomic_load(&link_up) || !wifi_cred_valid(new_ssid, pass)) {
        return false;
    }
    msg_t m = {.type = MSG_JOIN};
    snprintf(m.ssid, sizeof(m.ssid), "%s", new_ssid);
    snprintf(m.pass, sizeof(m.pass), "%s", pass);
    // Visible as PENDING at once; only the task starts the trial (SSID and password together) and so only it decides
    // what an IP or a disconnect means. NVS is written once the pair has an IP.
    portENTER_CRITICAL(&state_lock);
    wifi_join_t before = join;
    wifi_join_request(&join);
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", new_ssid);
    portEXIT_CRITICAL(&state_lock);
    bool ok = post(&m);
    memset(&m, 0, sizeof(m));
    if (!ok) {
        portENTER_CRITICAL(&state_lock);
        join = before;
        portEXIT_CRITICAL(&state_lock);
    }
    return ok;
}

wifi_join_state_t wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap) {
    portENTER_CRITICAL(&state_lock);
    wifi_join_state_t state = join.state;
    if (tried && tried_cap) {
        snprintf(tried, tried_cap, "%s", state == WIFI_JOIN_PENDING ? pending_ssid : trial_ssid);
    }
    if (kept && kept_cap) {
        snprintf(kept, kept_cap, "%s", kept_ssid);
    }
    portEXIT_CRITICAL(&state_lock);
    return state;
}

static void cmd_wifi(char const *args) {
    char a[140], b[140];
    if (!*args) {
        char ip[16], cur[33];
        portENTER_CRITICAL(&state_lock);
        bool have = have_credentials;
        snprintf(cur, sizeof(cur), "%s", ssid);
        portEXIT_CRITICAL(&state_lock);
        clean(cur, a, sizeof(a));
        if (!have) {
            console_reply("wifi: no credentials (fw wifi scan | <ssid> <password> | reconnect | --clear)");
        } else if (wifi_has_ip(ip, sizeof(ip))) {
            int8_t rssi;
            if (wifi_rssi(&rssi)) {
                console_reply("wifi: \"%s\" connected, ip %s, rssi %d dBm, %d/4 bars", a, ip, rssi, wifi_bars(rssi));
            } else {
                console_reply("wifi: \"%s\" connected, ip %s, rssi not read yet", a, ip);
            }
        } else {
            console_reply("wifi: \"%s\" not connected", a);
        }
        char tried[33], kept[33];
        wifi_join_state_t st = wifi_join_state(tried, sizeof(tried), kept, sizeof(kept));
        clean(tried, a, sizeof(a));
        clean(kept, b, sizeof(b));
        if (st == WIFI_JOIN_TRYING || st == WIFI_JOIN_PENDING) {
            console_reply("wifi: trying \"%s\" (saved only once it gets an IP, gives up after %d s)", a,
                          WIFI_JOIN_TRIAL_MS / 1000);
        } else if (st == WIFI_JOIN_FAILED_KEPT_OLD) {
            console_reply(kept[0] ? "wifi: couldn't join \"%s\": kept \"%s\"" : "wifi: couldn't join \"%s\": no network saved", a,
                          b);
        }
    } else if (strcmp(args, "--clear") == 0) {
        msg_t m = {.type = MSG_FORGET};
        console_reply(post(&m) ? "wifi: credentials removed" : "wifi: busy, try again");
    } else if (strcmp(args, "reconnect") == 0) {
        char s[33], p[64];
        bool have = atomic_load(&link_up) && wifi_cred_load(s, p);
        memset(p, 0, sizeof(p));
        msg_t m = {.type = MSG_RECONNECT};
        console_reply(have && post(&m) ? "wifi: reconnecting with the saved credentials"
                                       : "wifi: no saved credentials or no C6 link");
    } else if (strcmp(args, "scan") == 0) {
        static wifi_ap_t aps[20];
        size_t           n = 0;
        if (!wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &n)) {
            console_reply("wifi: scan failed");
            return;
        }
        console_reply("wifi: %u networks", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            clean(aps[i].ssid, a, sizeof(a)); // air-sourced: control bytes are escaped as \xNN
            console_reply("  %4d dBm  bars %d  %s  %s", aps[i].rssi, wifi_bars(aps[i].rssi),
                          aps[i].open ? "open  " : "locked", a);
        }
    } else {
        // The SSID is the first word; the password is the rest of the line (it may contain spaces)
        char const *space = strchr(args, ' ');
        size_t      len   = space ? (size_t)(space - args) : strlen(args);
        char        new_ssid[33];
        if (len == 0 || len > 32) {
            console_reply("wifi <ssid> <password> (SSID without spaces, at most 32 characters)");
            return;
        }
        memcpy(new_ssid, args, len);
        new_ssid[len] = '\0';
        char const *pass = space ? space + 1 : "";
        clean(new_ssid, a, sizeof(a));
        if (!wifi_cred_valid(new_ssid, pass)) {
            console_reply("wifi: the password is empty (open network) or 8-63 characters");
        } else if (!wifi_set_credentials(new_ssid, pass)) {
            console_reply("wifi: could not start (no C6 link or busy)");
        } else {
            console_reply("wifi: trying \"%s\", saved only if it gets an IP within %d s; fw wifi shows the result", a,
                          WIFI_JOIN_TRIAL_MS / 1000);
        }
    }
}

static void cmd_net(char const *args) {
    (void)args;
    cmd_wifi("");
}

static void cmd_c6log(char const *args) {
    uint32_t ms = *args ? (uint32_t)strtoul(args, NULL, 10) : 10000;
    if (ms == 0 || ms > 60000) {
        console_reply("c6log [1-60000 ms]");
        return;
    }
    c6_log_console(ms);
}

static bool nvs_write_boot_state(char const *key, bool verify);
static void cmd_c6_status(void);

static void cmd_c6(char const *args) {
    if (c6_sup_command(args)) { // reset, reset rom, reattach (#14)
        return;
    }
    if (strcmp(args, "verify") == 0) {
        if (!nvs_write_boot_state(NULL, true)) {
            console_reply("c6: can't save the verify flag");
            return;
        }
        console_reply("C6 is checked fully during the next boot, rebooting");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        system_restart("C6 verify requested");
    }
    if (strcmp(args, "reflash") != 0) {
        cmd_c6_status();
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK || nvs_set_u8(nvs, "c6_force", 1) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK) {
        console_reply("c6: can't save the reflash flag");
        return;
    }
    nvs_close(nvs);
    console_reply("C6 is flashed again during the next boot, rebooting");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    system_restart("C6 reflash requested");
}


// ---- Skipping the flasher check on warm restarts (#36) ----
// NVS (FW_NVS_NAMESPACE): c6_ok_key = bundle key stored after the last successful verify/flash, c6_verify = the last
// attach after a skip failed, so the next boot checks fully. Only a software reset can skip; the full path never restarts.
static char const KEY_OK[]     = "c6_ok_key";
static char const KEY_VERIFY[] = "c6_verify";

static struct {
    char const *plan;     // "skip" / "full"
    char const *why;
    uint32_t    c6_ms;    // time spent in the C6 step this boot
    char        key[C6_BUNDLE_KEY_MAX];
} boot_info = {.plan = "?", .why = "?"};

// md5 text and size of one bundle file, the same files the flasher compares (no hashing of the 1 MB app)
static bool read_bundle_file(char const *name, char md5[40], unsigned long *size) {
    char path[64], md5_name[40];
    snprintf(md5_name, sizeof(md5_name), "%s.md5", name);
    if (!c6_image_path(name, path, sizeof(path))) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return false;
    }
    *size = (unsigned long)st.st_size;
    if (!c6_image_path(md5_name, path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(md5, 1, 39, f);
    fclose(f);
    md5[n] = '\0';
    return n >= 32;
}

static bool current_bundle_key(char *out, size_t cap) {
    char          m[3][40];
    unsigned long sz[3];
    static char const *const names[3] = {"bootloader.bin", "partitions.bin", "firmware.bin"};
    for (int i = 0; i < 3; i++) {
        if (!read_bundle_file(names[i], m[i], &sz[i])) {
            return false;
        }
    }
    return c6_bundle_key(m[0], m[1], m[2], sz[0], sz[1], sz[2], out, cap);
}

static void nvs_read_boot_state(char *stored, size_t cap, bool *verify) {
    nvs_handle_t nvs;
    uint8_t      v = 0;
    stored[0]      = '\0';
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = cap;
    if (nvs_get_str(nvs, KEY_OK, stored, &len) != ESP_OK) {
        stored[0] = '\0';
    }
    nvs_get_u8(nvs, KEY_VERIFY, &v);
    nvs_close(nvs);
    *verify = v != 0;
}

// Writes only what changes (key == NULL erases it): the NVS flash wears per write. True only when everything asked for
// is committed.
static bool nvs_write_boot_state(char const *key, bool verify) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool   ok                     = true;
    char   cur[C6_BUNDLE_KEY_MAX] = "";
    size_t len                    = sizeof(cur);
    bool   have                   = nvs_get_str(nvs, KEY_OK, cur, &len) == ESP_OK;
    if (key && (!have || strcmp(cur, key) != 0)) {
        ok = nvs_set_str(nvs, KEY_OK, key) == ESP_OK && ok;
    } else if (!key && have) {
        ok = nvs_erase_key(nvs, KEY_OK) == ESP_OK && ok;
    }
    uint8_t v     = 0;
    bool    vhave = nvs_get_u8(nvs, KEY_VERIFY, &v) == ESP_OK;
    if (verify && (!vhave || !v)) {
        ok = nvs_set_u8(nvs, KEY_VERIFY, 1) == ESP_OK && ok;
    } else if (!verify && vhave) {
        ok = nvs_erase_key(nvs, KEY_VERIFY) == ESP_OK && ok;
    }
    ok = nvs_commit(nvs) == ESP_OK && ok;
    nvs_close(nvs);
    return ok;
}

void c6link_bundle_changed(void) {
    nvs_write_boot_state(NULL, false);
}

// The C6 was lost at runtime and the badge restarts for it (#14): check it fully during the next boot, keeping the key
bool c6link_set_verify_flag(void) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    return nvs_write_boot_state(stored[0] ? stored : NULL, true);
}

static void cmd_c6_status(void) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    console_reply("c6: this boot %s (%s), C6 step %u ms", boot_info.plan, boot_info.why, (unsigned)boot_info.c6_ms);
    console_reply("c6: stored key %s, verify flag %d", stored[0] ? stored : "(none)", verify);
    char line[C6W_REASON_MAX + 64];
    c6_sup_watch_line(line, sizeof(line));
    console_reply("%s", line);
    console_reply("c6 reflash | verify (full check on the next boot) | reset [rom] (pulse C6 EN now) | reattach (#14)");
}

// The first boot of a fresh OTA image: until it is marked valid, one reset makes the bootloader roll it back
bool c6link_image_is_trial(void) {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

// What to do about the flasher this boot. Returns true to run it.
static bool plan_full_check(bool force, char *key, size_t key_cap) {
    char stored[C6_BUNDLE_KEY_MAX];
    bool verify = false;
    nvs_read_boot_state(stored, sizeof(stored), &verify);
    if (!current_bundle_key(key, key_cap)) {
        key[0] = '\0';
    }
    c6_reset_kind_t kind = esp_reset_reason() == ESP_RST_SW ? C6_RST_SOFTWARE : C6_RST_OTHER;
    c6_plan_t       plan = c6_boot_plan(kind, force, verify, c6link_image_is_trial(), stored, key, &boot_info.why);
    boot_info.plan       = plan == C6_PLAN_SKIP ? "skip" : "full";
    return plan == C6_PLAN_FULL_CHECK;
}

// P4 GPIO13 is the C6 GPIO9 strap: low at a reset means download mode. esp_hosted now resets the C6 through EN (GPIO12), so
// the strap has to be high first, on every path (the flasher may have bailed out early, #2). The level is set before the
// pin becomes an output so it never glitches. Low only for "fw c6 reset rom" (#14).
void hold_c6_boot(int level) {
    gpio_set_level(GPIO_NUM_13, level);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_13,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(GPIO_NUM_13, level);
}

bool c6link_start(void) {
    console_register("c6log", "c6log [ms] (copy the C6's console into the log)", cmd_c6log);
    console_register("c6", "c6 [reflash | verify | reset [rom] | reattach]", cmd_c6);
    clock_console_register();
    console_register("net", "net (Wi-Fi state and IP)", cmd_net);
    console_register("wifi", "wifi [scan | reconnect | <ssid> <password> | --clear]", cmd_wifi);
    c6_sup_start();
    scan_done   = xSemaphoreCreateBinary();
    scan_lock   = xSemaphoreCreateMutex();
    c6_msg_done = xSemaphoreCreateBinary();
    queue       = xQueueCreate(16, sizeof(msg_t));
    wifi_join_init(&join);
    if (!scan_done || !scan_lock || !c6_msg_done || !queue ||
        // 5 KB: since #14 it also runs wifi_driver_up (esp_wifi_init, a wifi_config_t, RPCs) after a re-attach
        xTaskCreatePinnedToCore(wifi_task, "c6wifi", 5120, NULL, 5, &wifi_task_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi task did not start");
    }

    boot_progress_begin(BOOT_C6);
    int64_t   t0    = esp_timer_get_time();
    bool      force = take_force_flag();
    char      key[C6_BUNDLE_KEY_MAX];
    bool      full = plan_full_check(force, key, sizeof(key));
    esp_err_t err  = ESP_OK;
    if (full) {
        ESP_LOGW(TAG, "C6 flasher check runs (%s)", boot_info.why);
        err = flash_c6_if_needed(force);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C6 image check: %s (going on with what the C6 has)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "C6 bundle unchanged, ROM check skipped (SW reset)");
    }
    // The C6 was reset into its application by the flasher; only now may ESP-Hosted talk to it
    hold_c6_boot(1);
    bool attached = esp_hosted_init() == 0 && esp_hosted_connect_to_slave() == 0;
    // The key is stored only after a successful attach; a failed one sets the verify flag (full check next boot)
    c6_state_write_t w         = c6_state_after_attach(full, attached, err == ESP_OK, key[0] != '\0');
    bool             persisted = w.write && nvs_write_boot_state(w.store_key ? key : NULL, w.verify);
    if (!attached) {
        ESP_LOGE(TAG, "no connection to the C6 over ESP-Hosted");
        // A skip is a bet that the C6 is fine; when it loses, check fully next boot. Never restart when that could not
        // be saved (next boot would skip again: a loop without Wi-Fi OTA); the full path never restarts.
        if (c6_restart_after_attach(full, attached, persisted)) {
            system_restart("C6 attach failed after skip");
        }
        boot_progress_end(BOOT_C6, STEP_FAILED, "no ESP-Hosted link");
        boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "no C6");
        boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no C6");
        return false;
    }
    atomic_store(&link_up, true);
    c6_sup_attached(); // from now on c6sup notices a C6 reset and recovers from it (#14)
    boot_info.c6_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, "C6 step %s: %u ms", boot_info.plan, (unsigned)boot_info.c6_ms);
    char note[BOOT_REASON_LEN];
    snprintf(note, sizeof(note), "%s%s%u ms", full ? "" : "check skipped, ", err == ESP_OK ? "" : "image check failed, ",
             (unsigned)boot_info.c6_ms);
    boot_progress_end(BOOT_C6, STEP_DONE, note);
    if (!start_wifi()) {
        ESP_LOGE(TAG, "Wi-Fi not started");
    }
    return true;
}
