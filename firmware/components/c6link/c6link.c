#include "c6link.h"

#include "boot_progress.h"
#include "c6_flasher.h"
#include "fw_console.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "wifi_join.h"
#include "wifi_list.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static char const TAG[] = "c6link";

static bool         link_up;
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
    return link_up;
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
                   (link_up && have_credentials && !first_connect_settled);
    portEXIT_CRITICAL(&state_lock);
    return joining;
}

// Everything that touches the connection runs in one task (c6wifi): events and requests are queued to it
typedef enum { MSG_CONNECT, MSG_JOIN, MSG_RECONNECT, MSG_FORGET, MSG_DISCONNECTED, MSG_GOT_IP, MSG_CONNECTED } msg_type_t;
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

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return wifi_failed("Wi-Fi init");
    }
    wipe_driver_store_once();
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return wifi_failed("Wi-Fi init");
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
        // The radio still starts (without connecting) so that the badge can scan and join from Settings
        if ((err = esp_wifi_start()) != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        }
        boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "no credentials");
        boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no Wi-Fi");
        return true;
    }
    portENTER_CRITICAL(&state_lock);
    snprintf(ssid, sizeof(ssid), "%.32s", (char const *)cfg.sta.ssid);
    portEXIT_CRITICAL(&state_lock);
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &cfg)) != ESP_OK || (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        return wifi_failed("Wi-Fi start");
    }
    // Associating, the IP and the clock come later and are not waited for; the clock step ends in on_time_sync()
    boot_progress_end(BOOT_WIFI, STEP_DONE, NULL);
    return true;
}

// ---- scan, join, reconnect (#66) ---------------------------------------------------------------------------------

#define SCAN_RECORDS  32
#define SCAN_WAIT_MS  6000

static bool updating; // a firmware update is being received: no scans (#76)

void c6link_set_updating(void) {
    updating = true;
}

bool wifi_scan(wifi_ap_t *out, size_t max, size_t *n) {
    *n = 0;
    if (updating || !link_up || !out || max == 0 || !scan_lock || xSemaphoreTake(scan_lock, pdMS_TO_TICKS(SCAN_WAIT_MS + 2000)) != pdTRUE) {
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
    if (updating || !wifi_has_ip(NULL, 0) || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    rssi_cached = ap.rssi;
    rssi_known  = true;
    portEXIT_CRITICAL(&state_lock);
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
                    if (have) {
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
            }
            memset(&m, 0, sizeof(m));
        }
        if (retry_pending && (int32_t)(now - retry_at) >= 0) {
            retry_pending = false;
            esp_wifi_connect();
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
    if (!link_up || !wifi_cred_valid(new_ssid, pass)) {
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
        bool have = link_up && wifi_cred_load(s, p);
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

static void cmd_c6(char const *args) {
    if (strcmp(args, "reflash") != 0) {
        console_reply("c6 reflash (write the C6 image again on the next boot, then reboot)");
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

bool c6link_start(void) {
    console_register("c6log", "c6log [ms] (copy the C6's console into the log)", cmd_c6log);
    console_register("c6", "c6 reflash", cmd_c6);
    console_register("net", "net (Wi-Fi state and IP)", cmd_net);
    console_register("wifi", "wifi [scan | reconnect | <ssid> <password> | --clear]", cmd_wifi);
    scan_done = xSemaphoreCreateBinary();
    scan_lock = xSemaphoreCreateMutex();
    queue     = xQueueCreate(16, sizeof(msg_t));
    wifi_join_init(&join);
    if (!scan_done || !scan_lock || !queue ||
        xTaskCreatePinnedToCore(wifi_task, "c6wifi", 4096, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi task did not start");
    }

    boot_progress_begin(BOOT_C6);
    esp_err_t err = flash_c6_if_needed(take_force_flag());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 image check: %s (going on with what the C6 has)", esp_err_to_name(err));
    }
    // The C6 was reset into its application by the flasher; only now may ESP-Hosted talk to it
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "no connection to the C6 over ESP-Hosted");
        boot_progress_end(BOOT_C6, STEP_FAILED, "no ESP-Hosted link");
        boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "no C6");
        boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no C6");
        return false;
    }
    link_up = true;
    boot_progress_end(BOOT_C6, STEP_DONE, err == ESP_OK ? NULL : "image check failed");
    if (!start_wifi()) {
        ESP_LOGE(TAG, "Wi-Fi not started");
    }
    return true;
}
