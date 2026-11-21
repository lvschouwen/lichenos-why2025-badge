// Lichen's Wi-Fi service (wifi_service.h): moved out of c6link (#223) unchanged in what it does. c6link starts it
// through the net hooks registered in wifi_service_init (attached, no_link, the re-attach's driver down/up); the
// link state comes from c6link_up/c6link_updating. The log tag stays "c6link" so the console lines read as before.
#include "wifi_service.h"

#include "boot_progress.h"
#include "c6link.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_clock.h"
#include "lichen_mdns.h"
#include "lwip/dhcp6.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "wifi_cmd.h"
#include "wifi_inet.h"
#include "wifi_scan_drv.h"
#include "wifi_console.h"
#include "wifi_join_apply.h"
#include "wifi_ip6.h"
#include "wifi_count.h"
#include "wifi_regdom.h"
#include "wifi_req.h"
#include "wifi_text.h"
#include "wifi_saved.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static char const TAG[] = "c6link";

static wifi_radio_hooks_t radio; // from wifi_service_init, read-only after it

// The OS Wi-Fi switch (#108, lichen_radio): off = the driver stays initialised on the C6 but is not started, so the
// link, LoRa and the C6 watch are untouched and nothing here looks like a lost C6. wifi_enable() may come before
// wifi_service_init(). wifi_started: esp_wifi_start was called and not stopped since (set by the boot task, then the c6wifi task).
static atomic_bool wifi_on = true;
static atomic_bool wifi_started;
static atomic_bool driver_ready; // esp_wifi_init done on the C6 that is attached now; false until MSG_C6_UP has run
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
static SemaphoreHandle_t saved_lock; // the saved-network list (#160): the task saves, the screen forgets
// Requests to the c6wifi task (#502): scan, power save, TX power, protocol/bandwidth. req_lock: one caller at a time (the
// screen and the console may ask together); req holds the answer (req_mux), req_done wakes the waiting caller.
static SemaphoreHandle_t req_lock;
static SemaphoreHandle_t req_done;
static wifi_req_t        req;
static portMUX_TYPE      req_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t      wifi_task_handle;
static portMUX_TYPE ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char         ip_text[16];
static wifi_count_t counts; // #415: written by the c6wifi task, read by wifi_link_counts, both under state_lock

bool wifi_has_ip(char *ip, size_t cap) {
    portENTER_CRITICAL(&ip_lock);
    bool have = ip_text[0] != '\0';
    if (have && ip && cap) { // wifi_has_ip(NULL, 0) only asks whether there is one
        wifi_text_copy(ip, cap, ip_text);
    }
    portEXIT_CRITICAL(&ip_lock);
    return have;
}

// Read from the netif on every call, never cached: only preferred addresses count (a deprecated or withdrawn one is
// gone at once) and a netif that is down has none, so there is no stale state to clear or race with a disconnect. The
// read takes no lock (esp_netif_get_all_preferred_ip6 only looks at the lwIP arrays), so any task may call it.
static void ip6_read(wifi_ip6_set_t *set) {
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    wifi_ip6_clear(set);
    int n = sta_netif ? esp_netif_get_all_preferred_ip6(sta_netif, addrs) : 0;
    for (int i = 0; i < n && i < CONFIG_LWIP_IPV6_NUM_ADDRESSES; i++) {
        uint8_t raw[16];
        memcpy(raw, addrs[i].addr, sizeof(raw)); // lwIP keeps the words in network order
        wifi_ip6_add(set, raw);
    }
}

wifi_ip6_level_t wifi_ip6_get(char *text, size_t cap, unsigned *globals) {
    wifi_ip6_set_t set;
    uint8_t        best[16];
    ip6_read(&set);
    if (globals) {
        *globals = wifi_ip6_global_count(&set);
    }
    if (text && cap) {
        text[0] = '\0';
        if (wifi_ip6_best(&set, best)) {
            char tmp[WIFI_IP6_TEXT_MAX];
            wifi_ip6_text(best, tmp, sizeof(tmp));
            wifi_text_copy(text, cap, tmp);
        }
    }
    return wifi_ip6_level(&set);
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
    wifi_text_copy(ip_text, sizeof(ip_text), text);
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
    bool joining = atomic_load(&wifi_on) && (join.state == WIFI_JOIN_PENDING || join.state == WIFI_JOIN_TRYING ||
                   (c6link_up() && have_credentials && !first_connect_settled));
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
    MSG_WIFI_SYNC, // the OS Wi-Fi switch changed (#108): start or stop the driver to match
    MSG_SCAN,      // (#502) scan into scan_res; reason = the request number (wifi_req.h)
    MSG_SET_PS,    // apply the chosen power save, TX power, or protocol/bandwidth: the values are the atomics, the
    MSG_SET_TX,    // message only says "now"; reason = the request number, so a repeat is idempotent
    MSG_SET_PROTO, // value: 'n' or 'x' ...
    MSG_SET_BW,    // ... or the MHz. The task stores a value only once the driver took it (#502)
} msg_type_t;
typedef struct {
    msg_type_t type;
    int        reason;
    int        value; // MSG_SET_*: the value asked for
    char       ssid[33];
    char       pass[64];
    wifi_eap_t *eap; // MSG_JOIN of an Enterprise network: a heap copy the c6wifi task owns from the post on (#496)
} msg_t;

static QueueHandle_t queue;
static atomic_uint    queue_full; // messages dropped for a full queue, since boot (fw soak, #419)

uint32_t wifi_queue_drops(void) {
    return atomic_load(&queue_full);
}

void wifi_link_counts(uint32_t *disconnects, uint32_t *reconnects, uint32_t *last_reason) {
    portENTER_CRITICAL(&state_lock);
    wifi_count_t c = counts;
    portEXIT_CRITICAL(&state_lock);
    *disconnects = c.disconnects;
    *reconnects  = c.reconnects;
    *last_reason = c.last_reason;
}

static wifi_link_t link_ap; // c6wifi task only: the SSID the C6 says it is connected to (#15)

// Called from the event loop task, so a short wait is fine; a dropped GOT_IP would roll a good trial back
static bool post(msg_t const *m) {
    bool ok = queue && xQueueSend(queue, m, pdMS_TO_TICKS(50)) == pdTRUE;
    if (!ok && queue) {
        atomic_fetch_add(&queue_full, 1);
        ESP_LOGW(TAG, "wifi: queue full, message %d dropped", (int)m->type);
    }
    return ok;
}

// The driver's applied values for the registry (#311), under rb_lock: the last read that got anything, and a counter
// every setter bumps, so a read from before a set is never reused for that set's echo (wifi_readback_reusable)
static portMUX_TYPE    rb_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_readback_t readback;
static uint32_t        set_gen;

static void readback_invalidate(void) {
    portENTER_CRITICAL(&rb_lock);
    set_gen++;
    portEXIT_CRITICAL(&rb_lock);
}

// Wi-Fi power save (#146). The IDF default WIFI_PS_MIN_MODEM wakes the C6 only at DTIM beacons: RTT spikes of a few
// hundred ms and truncated screenshots on a busy channel, without a disconnect. Off by default; `fw wifi ps` switches
// it for a comparison (not saved). Applied on every STA_START, i.e. after each esp_wifi_start. Battery policy: #76.
static _Atomic int wifi_ps_mode = WIFI_PS_NONE;

// TX power and STA bandwidth for the #146 link tests: 0 = the driver's own choice (20 dBm, HT40 allowed). `fw wifi tx`
// and `fw wifi bw` set them at runtime (not saved). TX power is in the driver's 0.25 dBm unit (52 = 13 dBm).
static _Atomic int wifi_tx_raw;
static _Atomic int wifi_bw_mhz;

#define WIFI_TX_DEFAULT_RAW WIFI_RB_TX_DEFAULT

// wifi_regdom.h names the driver's numbers without its headers
_Static_assert(WIFI_RB_POLICY_MANUAL == WIFI_COUNTRY_POLICY_MANUAL, "country policy");
_Static_assert(WIFI_RB_PS_NONE == WIFI_PS_NONE && WIFI_RB_PS_MIN == WIFI_PS_MIN_MODEM, "power save");
_Static_assert(WIFI_RB_PROTO_11BGN == (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N) && WIFI_RB_PROTO_11AX == WIFI_PROTOCOL_11AX, "protocol");
_Static_assert(WIFI_BW20 == 1 && WIFI_BW40 == 2 && WIFI_BW80 == 3 && WIFI_BW160 == 4 && WIFI_BW80_BW80 == 5, "bandwidth");

static bool set_wifi_tx(int raw) {
    esp_err_t err = esp_wifi_set_max_tx_power((int8_t)raw);
    readback_invalidate();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: TX power not set: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void apply_wifi_tx(void) {
    int raw = atomic_load(&wifi_tx_raw);
    if (raw) {
        set_wifi_tx(raw);
    }
}

// 802.11ax on or off for the station (#146): 0 = driver default (b/g/n/ax), 'n' = b/g/n only, no HE (no TWT)
static _Atomic int wifi_proto;

// False when the driver refused the protocol or the bandwidth
static bool apply_wifi_bw(void) {
    bool      ok = true;
    int       bw = atomic_load(&wifi_bw_mhz);
    int       proto = atomic_load(&wifi_proto);
    esp_err_t err;
    readback_invalidate();
    if (proto) {
        uint8_t mask = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | (proto == 'n' ? 0 : WIFI_PROTOCOL_11AX);
        if ((err = esp_wifi_set_protocol(WIFI_IF_STA, mask)) != ESP_OK) {
            ESP_LOGW(TAG, "wifi: protocol not set: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    if (bw && (err = esp_wifi_set_bandwidth(WIFI_IF_STA, bw == 20 ? WIFI_BW20 : WIFI_BW40)) != ESP_OK) {
        ESP_LOGW(TAG, "wifi: bandwidth not set: %s", esp_err_to_name(err));
        ok = false;
    }
    return ok;
}

static void apply_wifi_ps(void) {
    esp_err_t err = esp_wifi_set_ps((wifi_ps_type_t)atomic_load(&wifi_ps_mode));
    readback_invalidate();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: power save not set: %s", esp_err_to_name(err));
    }
    apply_wifi_bw(); // before esp_wifi_connect: the bandwidth is agreed when associating
    apply_wifi_tx();
}

static void country_into(wifi_readback_t *r, wifi_country_t const *c) {
    memcpy(r->cc, c->cc, sizeof(r->cc));
    r->schan           = c->schan;
    r->nchan           = c->nchan;
    r->policy          = (uint8_t)c->policy;
    r->country_max_dbm = c->max_tx_power;
}

static bool country_read(wifi_readback_t *r) {
    wifi_country_t c = {0};
    *r               = (wifi_readback_t){0};
    if ((r->country_ok = esp_wifi_get_country(&c) == ESP_OK)) {
        country_into(r, &c);
    }
    return r->country_ok;
}

// The last set failed: the next STA_START tries once more (boot task or c6wifi task)
static atomic_bool country_retry;

// The regulatory domain (#311, Lucas 2026-11-10): NL with 802.11d off (policy manual: an AP's country IE never changes
// it), before every esp_wifi_start and after a driver restore. The driver's own default is "01" (world safe, channels
// 1-11, 802.11d on). It stores the country in the C6's flash, so it is set only when the driver does not hold NL as set
// already (wifi_country_needs_set). The regulatory line is logged every time. A failed set leaves the driver on its
// default, which is narrower, so the start goes ahead.
static void set_country(void) {
    wifi_readback_t r;
    esp_err_t       err = ESP_OK;
    country_read(&r);
    if (wifi_country_needs_set(&r)) {
        char was[64];
        wifi_regdom_text(&r, was, sizeof(was));
        err = esp_wifi_set_country_code(WIFI_COUNTRY, false);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "wifi: country set to %s (the driver had %s)", WIFI_COUNTRY, was);
        }
        country_read(&r);
    }
    atomic_store(&country_retry, err != ESP_OK);
    readback_invalidate();
    char text[64];
    wifi_regdom_text(&r, text, sizeof(text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi: country %s not set: %s (the driver keeps %s)", WIFI_COUNTRY, esp_err_to_name(err), text);
    } else {
        ESP_LOG_LEVEL(wifi_regdom_expected(&r) ? ESP_LOG_INFO : ESP_LOG_WARN, TAG, "wifi: regulatory %s", text);
    }
}

static char const *wifi_ps_name(wifi_ps_type_t ps) {
    return ps == WIFI_PS_NONE ? "off" : ps == WIFI_PS_MIN_MODEM ? "min (modem sleep)" : "max (modem sleep)";
}

// The log line for IP_EVENT_GOT_IP6 (link-local ready, SLAAC address preferred). The prefix only: SLAAC puts the
// MAC in the low 64 bits, and a log line travels (bug reports, the console log). The console and the screen show all.
static void ip6_log(void) {
    wifi_ip6_set_t set;
    uint8_t        best[16];
    char           prefix[WIFI_IP6_TEXT_MAX + 4] = "";
    ip6_read(&set);
    if (wifi_ip6_best(&set, best)) {
        wifi_ip6_prefix_text(best, prefix, sizeof(prefix));
    }
    wifi_ip6_level_t level = wifi_ip6_level(&set);
    char const      *name  = level == WIFI_IP6_GLOBAL ? "global" : level == WIFI_IP6_LINKLOCAL ? "link-local" : "none";
    ESP_LOGI(TAG, "wifi: ipv6 %s %s", name, prefix);
}

static void on_ip_event(int32_t id, void *data) {
    if (id == IP_EVENT_GOT_IP6) {
        ip6_log();
    } else if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t const *got = data;
        char                     text[16];
        snprintf(text, sizeof(text), IPSTR, IP2STR(&got->ip_info.ip));
        set_ip(text);
        wifi_dns_keep_v4();
        ESP_LOGI(TAG, "wifi: got ip:%s", text);
        msg_t m = {.type = MSG_GOT_IP};
        post(&m);
    }
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
        if (!gone || !wifi_reason_is_roam(gone->reason)) { // a roam keeps the lease, so keep its servers
            LOCK_TCPIP_CORE();
            wifi_dns_clear_kept();
            UNLOCK_TCPIP_CORE();
        }
        m.type   = MSG_DISCONNECTED;
        m.reason = gone ? gone->reason : 0;
        post(&m);
    } else if (base == IP_EVENT) {
        on_ip_event(id, data);
    }
}

// Wi-Fi did not start, so the clock will not be set either
static bool wifi_failed(char const *reason) {
    boot_progress_end(BOOT_WIFI, STEP_FAILED, reason);
    boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no Wi-Fi");
    return false;
}

// After a factory reset (#342): the C6 driver keeps its own copy of the credentials; clear it so old ones never survive.
// The restore also resets the driver's storage mode, so it comes before set_storage(RAM). Retried next boot on failure.
static void wipe_driver_store_if_ordered(void) {
    if (!wifi_driver_wipe_pending()) {
        return;
    }
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi driver restore: %s", esp_err_to_name(err));
        return;
    }
    if (!wifi_driver_wipe_done()) {
        ESP_LOGW(TAG, "Wi-Fi driver restore done but its order is not cleared: it runs again next boot");
    }
}

typedef enum { DRV_STARTED, DRV_NO_CREDENTIALS, DRV_FAILED, DRV_OFF } drv_result_t;
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
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_event, NULL, NULL);
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_on_sta_connected_ip6, sta_netif, NULL);
    wifi_dhcp6_stateless(sta_netif);
    wifi_sntp_start();
    boot_progress_begin_background(BOOT_CLOCK); // SNTP waits for an IP; the clock step ends in on_time_sync() (#39)
    // The boot steps are ended here, not in wifi_driver_up: a re-attach runs it again without them (#14)
    char const  *why = NULL;
    drv_result_t r   = wifi_driver_up(&why);
    if (r == DRV_FAILED) {
        return wifi_failed(why);
    }
    if (r == DRV_OFF) {
        boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "switched off");
        boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no Wi-Fi");
        return true;
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

static int         auth_cached; // under state_lock, valid while rssi_known (wifi.auth, #496)
static bool apply_config(char const *s, char const *p);

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
    wipe_driver_store_if_ordered();
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        return DRV_FAILED;
    }
    atomic_store(&driver_ready, true);
    // The credentials are ours (NVS badge_fw): the driver must never keep a copy in its own flash storage, so that
    // "fw wifi --clear" really forgets them
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi storage: %s", esp_err_to_name(err));
    }
    set_country(); // before either esp_wifi_start below, and before handle_wifi_sync's
    wifi_apply_reset(); // a new driver on the C6 holds no Enterprise state
    char cred_ssid[33], cred_pass[64];
    bool have = wifi_cred_load(cred_ssid, cred_pass);
    portENTER_CRITICAL(&state_lock);
    have_credentials = have;
    portEXIT_CRITICAL(&state_lock);
    if (!atomic_load(&wifi_on)) {
        // Not started: Settings or "fw wifi on" does it (wifi_enable). The saved pair goes to the driver then
        if (have) {
            apply_config(cred_ssid, cred_pass);
        }
        memset(cred_pass, 0, sizeof(cred_pass));
        ESP_LOGI(TAG, "Wi-Fi is switched off");
        return DRV_OFF;
    }
    if (!have) {
        ESP_LOGI(TAG, "no Wi-Fi credentials, set them in Settings or with: fw wifi <ssid> <password>");
        // The radio still starts (without connecting) so that the badge can scan and join from Settings.
        // driver_up goes true BEFORE esp_wifi_start: STA_START is handled by the c6wifi task (prio 5) before the
        // boot task gets back from esp_wifi_start, and MSG_CONNECT is dropped while driver_up is false
        atomic_store(&driver_up, true);
        if ((err = esp_wifi_start()) != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
            atomic_store(&driver_up, false);
        } else {
            atomic_store(&wifi_started, true);
        }
        return DRV_NO_CREDENTIALS;
    }
    portENTER_CRITICAL(&state_lock);
    wifi_text_copy(ssid, sizeof(ssid), cred_ssid);
    portEXIT_CRITICAL(&state_lock);
    atomic_store(&driver_up, true); // before esp_wifi_start, see above
    // A pair that cannot be applied (an Enterprise network on a C6 without 802.1X) still lets the radio start: scans
    // and the Wi-Fi screen work, config_bad only keeps it from connecting
    bool applied = apply_config(cred_ssid, cred_pass);
    memset(cred_pass, 0, sizeof(cred_pass));
    err = applied || wifi_apply_blocked() ? esp_wifi_start() : ESP_FAIL; // any other refusal is the old failure
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
        atomic_store(&driver_up, false);
        *why = "Wi-Fi start";
        return DRV_FAILED;
    }
    atomic_store(&wifi_started, true);
    return DRV_STARTED;
}

// ---- scan, join, reconnect (#66) ---------------------------------------------------------------------------------

#define SCAN_RECORDS  WIFI_SCAN_RECORDS
#define SCAN_WAIT_MS  WIFI_SCAN_WAIT_MS

static bool            scan_clean; // the request's flavour: the req_lock holder writes it before it posts
static size_t          scan_n;     // the result: the task writes both before it answers, the holder reads after
EXT_RAM_BSS_ATTR static wifi_ap_t scan_res[SCAN_RECORDS];

static wifi_req_why_t request_locked(msg_type_t type, int value, uint32_t answer_ms, bool *ok);

// The caller's side of a scan: refused early when the link is plainly not there, else the c6wifi task decides (it knows
// about a join or a roam in flight) and scans. The wait is bounded; a caller that gives up is not answered later.
static bool scan_ask(wifi_ap_t *out, size_t max, size_t *n, bool clean_list, wifi_req_why_t *why_out) {
    *n = 0;
    wifi_req_why_t why = WIFI_REQ_BUSY;
    bool           ok  = false;
    if (out && max && req_lock) {
        if (c6link_updating() || !c6link_up() || !atomic_load(&wifi_on)) {
            why = c6link_updating() ? WIFI_REQ_UPDATING : !c6link_up() ? WIFI_REQ_NO_LINK : WIFI_REQ_OFF;
        } else if (xSemaphoreTake(req_lock, pdMS_TO_TICKS(SCAN_WAIT_MS + 2000)) == pdTRUE) {
            scan_clean = clean_list;
            why        = request_locked(MSG_SCAN, 0, SCAN_WAIT_MS + 3000, &ok);
            if (ok) {
                *n = scan_n < max ? scan_n : max;
                memcpy(out, scan_res, *n * sizeof(*out));
            }
            xSemaphoreGive(req_lock);
        }
    }
    if (why_out) {
        *why_out = ok ? WIFI_REQ_OK : why;
    }
    return ok;
}

bool wifi_scan(wifi_ap_t *out, size_t max, size_t *n) {
    return scan_ask(out, max, n, true, NULL);
}

bool wifi_scan_raw(wifi_ap_t *out, size_t max, size_t *n) {
    return scan_ask(out, max, n, false, NULL);
}

bool wifi_scan_why(wifi_ap_t *out, size_t max, size_t *n, wifi_req_why_t *why) {
    return scan_ask(out, max, n, true, why);
}

// ---- the c6wifi task: joining with a trial, reconnecting, forgetting ---------------------------------------------


static char     trial_pass[64];
static wifi_eap_t *trial_eap; // c6wifi task only: the Enterprise credential being tried (heap), like trial_pass

static void saved_release(void *p);

// Wipes what a trial held; every path that ends or drops one calls this
static void trial_wipe(void) {
    memset(trial_pass, 0, sizeof(trial_pass));
    if (trial_eap) {
        memset(trial_eap, 0, sizeof(*trial_eap));
        saved_release(trial_eap);
        trial_eap = NULL;
    }
}
static bool     retry_pending;
static bool     retry_roam; // the pending retry is a roam's fallback: a CONNECTED cancels it (#495)
static uint32_t retry_at;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- the driver's applied values for the registry (#311) --------------------------------------------------------


static void requested_into(wifi_readback_t *r) {
    int proto     = atomic_load(&wifi_proto);
    r->req_tx_raw = (int8_t)atomic_load(&wifi_tx_raw);
    r->req_bw_mhz = (uint8_t)atomic_load(&wifi_bw_mhz);
    r->req_proto  = proto ? (uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                                     (proto == 'n' ? 0 : WIFI_PROTOCOL_11AX))
                          : 0;
    r->req_ps     = (uint8_t)atomic_load(&wifi_ps_mode);
}

// One read of everything over the RPCs (each waits for the C6: never on the LVGL task). False when nothing could be
// read; the cache then keeps the read before.
static bool read_back(void) {
    wifi_readback_t  r     = {0};
    wifi_country_t   c     = {0};
    uint8_t          proto = 0;
    wifi_bandwidth_t bw    = 0;
    int8_t           tx    = 0;
    wifi_ps_type_t   ps    = WIFI_PS_NONE;
    portENTER_CRITICAL(&rb_lock);
    uint32_t gen = set_gen;
    portEXIT_CRITICAL(&rb_lock);
    // The country is readable whenever the driver is initialised: when it fails the C6 is not answering, and the other
    // four would only wait out their timeouts too
    if (!(r.country_ok = esp_wifi_get_country(&c) == ESP_OK)) {
        return false;
    }
    country_into(&r, &c);
    r.proto_ok = esp_wifi_get_protocol(WIFI_IF_STA, &proto) == ESP_OK;
    r.bw_ok    = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) == ESP_OK;
    r.tx_ok    = esp_wifi_get_max_tx_power(&tx) == ESP_OK;
    r.ps_ok    = esp_wifi_get_ps(&ps) == ESP_OK;
    r.proto  = proto;
    r.bw_mhz = wifi_bw_to_mhz((int)bw);
    r.tx_raw = tx;
    r.ps     = (uint8_t)ps;
    r.have    = true;
    r.at_ms   = now_ms();
    r.set_gen = gen;
    portENTER_CRITICAL(&rb_lock);
    readback = r;
    portEXIT_CRITICAL(&rb_lock);
    return true;
}

// Only while the link and the driver are up: the c6wifi task and the console never wait out RPC timeouts on a C6
// that is gone or being updated
static bool read_back_if_up(void) {
    return c6link_up() && !c6link_updating() && atomic_load(&driver_ready) && read_back();
}

bool wifi_readback(wifi_readback_t *out, bool live) {
    uint32_t t = now_ms();
    portENTER_CRITICAL(&rb_lock);
    bool recent = wifi_readback_reusable(&readback, set_gen, t); // `fw get wifi.` asks once for its five keys
    portEXIT_CRITICAL(&rb_lock);
    bool fresh = live && (recent || read_back_if_up());
    portENTER_CRITICAL(&rb_lock);
    *out = readback;
    portEXIT_CRITICAL(&rb_lock);
    requested_into(out);
    return fresh;
}

// The caller's side of ps, tx and link: store the choice (the task applies what the atomics say), ask, wait a bounded
// time. Before the first start there is nothing to apply: STA_START does it.
#define REQ_LOCK_MS   2000
#define REQ_ANSWER_MS 4000

static _Atomic int *choice_slot(msg_type_t type) {
    return type == MSG_SET_PS ? &wifi_ps_mode : type == MSG_SET_TX ? &wifi_tx_raw : type == MSG_SET_PROTO ? &wifi_proto : &wifi_bw_mhz;
}

// The choice is stored by the c6wifi task, and only once the gate passed and the driver took it (#502): a refused,
// timed-out or failed set leaves the stored value as it was, so no later apply picks it up. Before the first start there
// is nothing to refuse and nothing to apply: it is stored here and STA_START applies it.
static bool apply_now(msg_type_t type, int value, wifi_req_why_t *why) {
    wifi_req_why_t w  = WIFI_REQ_OK;
    bool           ok = true;
    if (!atomic_load(&wifi_started)) {
        atomic_store(choice_slot(type), value);
        readback_invalidate();
    } else {
        ok = false;
        w  = WIFI_REQ_BUSY;
        if (req_lock && xSemaphoreTake(req_lock, pdMS_TO_TICKS(REQ_LOCK_MS)) == pdTRUE) {
            w = request_locked(type, value, REQ_ANSWER_MS, &ok);
            xSemaphoreGive(req_lock);
        }
    }
    if (why) {
        *why = ok ? WIFI_REQ_OK : w;
    }
    return ok;
}

static bool bad_arg(wifi_req_why_t *why) {
    if (why) {
        *why = WIFI_REQ_BAD_ARG;
    }
    return false;
}

bool wifi_set_ps(bool min) {
    return apply_now(MSG_SET_PS, min ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE, NULL);
}

bool wifi_set_tx(int raw, wifi_req_why_t *why) {
    if (raw != 0 && (raw < WIFI_TX_RAW_MIN || raw > WIFI_TX_RAW_MAX)) {
        return bad_arg(why);
    }
    return apply_now(MSG_SET_TX, raw, why);
}

bool wifi_set_bw(unsigned mhz, wifi_req_why_t *why) {
    if (mhz != 20 && mhz != 40) {
        return bad_arg(why);
    }
    return apply_now(MSG_SET_BW, (int)mhz, why);
}

bool wifi_set_proto(bool n_only, wifi_req_why_t *why) {
    return apply_now(MSG_SET_PROTO, n_only ? 'n' : 'x', why);
}

// Log-safe copy of an SSID (they come from the air)
static void clean(char const *in, char *out, size_t cap) {
    wifi_ssid_sanitize(in, out, cap);
}

// The raw scan choose_network reads, c6wifi task only. In PSRAM (EXT_RAM_BSS, ~2 KB): a stronger open twin of a
// saved secured name must not hide the real AP, so every BSSID is kept
EXT_RAM_BSS_ATTR static wifi_ap_t pick_aps[SCAN_RECORDS];

// The UI asks from the LVGL task: a short wait, never the C6 RPC's length (#249)
#define SAVED_UI_WAIT_MS 200 // longer than the NVS writes at GOT_IP (wifi_cred_save/activate hold the lock for them)
#define SAVED_FORGET_WAIT_MS 500

static bool saved_take(uint32_t wait_ms) {
    return xSemaphoreTake(saved_lock, wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}
static void saved_give(void) {
    xSemaphoreGive(saved_lock);
}
// The list copy's memory comes from the OS (wifi_bind_alloc): lichen_net is L3 and may not call lichen_alloc itself
static void *(*bound_alloc)(size_t size);
static void (*bound_release)(void *p);
static void *saved_alloc(size_t size) {
    if (!bound_alloc) { // a boot-order bug: the saved list, Forget and failover between saved networks all stop
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "wifi: no allocator bound (wifi_bind_alloc): saved networks unusable");
        }
        return NULL;
    }
    return bound_alloc(size);
}
static void saved_release(void *p) {
    if (bound_release) {
        bound_release(p);
    }
}
void wifi_bind_alloc(void *(*alloc)(size_t size), void (*release)(void *p)) {
    bound_alloc   = alloc;
    bound_release = release;
    wifi_list_bind_alloc(alloc, release);
}

// Under saved_lock, once the RPC went through and the name is still saved: from here on Forget compares against it
static void pick_committed(char const *name) {
    portENTER_CRITICAL(&state_lock);
    wifi_text_copy(ssid, sizeof(ssid), name);
    portEXIT_CRITICAL(&state_lock);
}

static void handle_forget(void);
static void handle_reconnect(bool force);

static wifi_lock_t const saved_ops = {
    .take = saved_take, .give = saved_give, .alloc = saved_alloc, .release = saved_release};

// ---- applying a pair to the driver (#496): wifi_join_apply.c -------------------------------------------------------

static bool apply_config(char const *s, char const *p) {
    return wifi_apply_saved(s, p, &saved_ops);
}

// Several networks saved (#160): scan, then load the strongest one in range into the driver. tried_mask holds the
// networks that failed in this round (bit = list index); when all of them did, the round starts over. Scans disturb a
// link (#146), so one at most every SCAN_GAP_MS. One saved network or no scan: the pair in use stays. Task only.
#define SCAN_GAP_MS 10000
#define FAILS_PER_ROUND 3 // retries (WIFI_RETRY_MS apart) before the next saved network gets its turn

static uint8_t  tried_mask;
static uint8_t  fail_count;
static uint32_t next_scan_at;

// Loads the strongest saved network in range into the driver (the scan is in pick_aps). One that cannot even be applied
// (an Enterprise network on a C6 without 802.1X) counts as tried and the next one gets its turn; when none applies the
// pair in use goes back, so an old C6 still joins its PSK networks.
static void join_strongest(wifi_saved_t *list, size_t n) {
    bool failed = false;
    for (int round = 0; round < WIFI_SAVED_MAX; round++) {
        int pick = wifi_saved_pick(list, pick_aps, n, tried_mask);
        if (pick < 0 && tried_mask && round == 0) { // all tried: start over with the strongest
            tried_mask = 0;
            pick       = wifi_saved_pick(list, pick_aps, n, 0);
        }
        if (pick < 0) {
            break;
        }
        // The scan took seconds: a Forget may have come in meanwhile, so the pick is checked against the list as it is
        // now. The C6 RPC runs with saved_lock released (#249: the UI must not wait on it) and the list is checked once
        // more afterwards, since a Forget in that window does not know the driver holds the name yet.
        char name[33];
        wifi_text_copy(name, sizeof(name), list->e[pick].ssid);
        wifi_apply_t res = wifi_saved_apply_with(&saved_ops, name, apply_config, pick_committed);
        if (res == WIFI_APPLY_FAILED) {
            tried_mask |= (uint8_t)(1u << pick);
            failed = true;
            continue;
        }
        if (res == WIFI_APPLY_APPLIED) {
            ESP_LOGI(TAG, "wifi: %d saved networks, joining the strongest in range", (int)list->n); // no names
        } else if (res == WIFI_APPLY_FORGOTTEN) {
            // Forgotten during the RPC: the driver holds it. We are the c6wifi task, so revoke directly (a queue post
            // could be dropped): clear the driver when nothing is saved any more, else apply the pair now in use
            char s[33], p[64];
            bool left = wifi_cred_load(s, p);
            memset(p, 0, sizeof(p));
            if (left) {
                handle_reconnect(true);
            } else {
                handle_forget();
            }
        }
        return;
    }
    if (failed) {
        char s[33], p[64];
        if (wifi_cred_load(s, p)) {
            apply_config(s, p);
        }
        memset(p, 0, sizeof(p));
    }
}

static void choose_network(bool current_failed) {
    static wifi_saved_t list;
    uint32_t            now = now_ms();
    size_t              n   = 0;
    if (c6link_updating() || (int32_t)(now - next_scan_at) < 0) {
        return;
    }
    xSemaphoreTake(saved_lock, portMAX_DELAY);
    wifi_saved_load(&list);
    xSemaphoreGive(saved_lock);
    if (list.n < 2) {
        wifi_saved_wipe(&list);
        return;
    }
    char cur[33];
    portENTER_CRITICAL(&state_lock);
    wifi_text_copy(cur, sizeof(cur), ssid);
    portEXIT_CRITICAL(&state_lock);
    int at = wifi_saved_find(&list, cur);
    if (current_failed && at >= 0) {
        tried_mask |= (uint8_t)(1u << at);
    }
    next_scan_at = now + SCAN_GAP_MS;
    if (wifi_scan_drv(scan_done, pick_aps, SCAN_RECORDS, &n, false)) {
        join_strongest(&list, n);
    }
    wifi_saved_wipe(&list);
}

// The previous pair is still what NVS holds: put it back in the driver
static void rollback(void) {
    char s[33], p[64];
    bool had = wifi_cred_load(s, p);
    trial_wipe();
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    portENTER_CRITICAL(&state_lock);
    join.state       = WIFI_JOIN_FAILED_KEPT_OLD;
    have_credentials = had;
    wifi_text_copy(kept_ssid, sizeof(kept_ssid), had ? s : "");
    wifi_text_copy(ssid, sizeof(ssid), had ? s : "");
    portEXIT_CRITICAL(&state_lock);
    if (had && apply_config(s, p) && !wifi_apply_blocked()) {
        esp_wifi_connect();
    }
    char a[140], b[140];
    clean(trial_ssid, a, sizeof(a));
    clean(s, b, sizeof(b));
    ESP_LOGW(TAG, "wifi: could not join \"%s\", %s%s%s", a, had ? "back to \"" : "no network saved", had ? b : "",
             had ? "\"" : "");
    memset(p, 0, sizeof(p));
}

static void handle_join(msg_t *m, uint32_t now) {
    trial_wipe(); // a trial still held (a second request before the first ended) goes first
    memcpy(trial_pass, m->pass, sizeof(trial_pass));
    trial_eap = m->eap; // ownership moves here; wiped by trial_wipe
    m->eap    = NULL;
    wifi_apply_clear_fail();
    portENTER_CRITICAL(&state_lock);
    wifi_join_start(&join, now);
    wifi_text_copy(trial_ssid, sizeof(trial_ssid), m->ssid);
    have_credentials = true;
    wifi_text_copy(ssid, sizeof(ssid), m->ssid);
    portEXIT_CRITICAL(&state_lock);
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    if (wifi_apply_net(m->ssid, m->pass, trial_eap, &saved_ops)) {
        esp_wifi_connect();
    } else {
        rollback(); // the apply module keeps the reason: fw wifi shows it next to "couldn't join"
    }
}

// force: a saved network was forgotten (#160); the driver may still hold it, even in the middle of a trial, so the trial
// is dropped and the pair in NVS is applied
static void handle_reconnect(bool force) {
    char s[33], p[64];
    portENTER_CRITICAL(&state_lock);
    bool trying = join.state == WIFI_JOIN_TRYING || join.state == WIFI_JOIN_PENDING;
    if (trying && force) {
        wifi_join_init(&join);
        trying = false;
    }
    portEXIT_CRITICAL(&state_lock);
    if (force) {
        trial_wipe();
    }
    if (trying || !atomic_load(&wifi_on)) {
        return;
    }
    if (!wifi_cred_load(s, p)) {
        if (force) { // the forgotten network was the last: do not leave it in the driver, or have_credentials/ssid stale
            handle_forget();
        }
        return;
    }
    portENTER_CRITICAL(&state_lock);
    have_credentials = true;
    wifi_text_copy(ssid, sizeof(ssid), s);
    portEXIT_CRITICAL(&state_lock);
    set_ip("");
    esp_wifi_disconnect();
    apply_wifi_bw(); // `fw wifi bw` reconnects to apply it (#146)
    if (apply_config(s, p) && !wifi_apply_blocked()) {
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
    trial_wipe();
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    esp_wifi_disconnect();
    apply_config("", ""); // the driver's RAM copy goes too (and the Enterprise state with it)
    wifi_apply_forgotten();
    // Whatever the C6's driver persisted in its own flash goes as well. This resets the driver's other settings to
    // defaults, which is why it only runs here and after a factory reset (wipe_driver_store_if_ordered).
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi driver restore: %s", esp_err_to_name(err));
    }
    // The restore put the driver's defaults back: flash storage and its own mode
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    set_country(); // and its country "01": NL again (#311)
}

static void handle_got_ip(void) {
    // Only an IP on the trial's own network proves its password: a late GOT_IP of the old connection must not save
    // the new pair. The SSID comes from the CONNECTED event (no RPC that could fail); never from esp_wifi_get_config,
    // which already holds the trial pair
    portENTER_CRITICAL(&state_lock);
    first_connect_settled = true;
    wifi_count_got_ip(&counts);
    wifi_join_event_t  ev;
    wifi_join_action_t act   = wifi_join_got_ip(&join, &link_ap, trial_ssid, now_ms(), &ev);
    bool               trial = join.state == WIFI_JOIN_TRYING;
    portEXIT_CRITICAL(&state_lock);
    tried_mask = fail_count = 0;
    if (act == JOIN_ACT_COMMIT) {
        // Only a pair that works is saved: until now NVS held the previous one
        xSemaphoreTake(saved_lock, portMAX_DELAY);
        bool saved = trial_eap ? wifi_apply_save_trial(trial_ssid, trial_eap, &saved_ops) : wifi_cred_save(trial_ssid, trial_pass);
        xSemaphoreGive(saved_lock);
        if (!saved) {
            ESP_LOGE(TAG, "wifi: joined, but saving the credentials failed");
        }
        wifi_apply_clear_fail();
        trial_wipe();
    } else if (!trial) {
        // One of the saved networks was picked by choose_network: it becomes the pair in use (first in the list)
        char cur[33];
        portENTER_CRITICAL(&state_lock);
        wifi_text_copy(cur, sizeof(cur), ssid);
        portEXIT_CRITICAL(&state_lock);
        xSemaphoreTake(saved_lock, portMAX_DELAY);
        bool kept = wifi_cred_activate(cur);
        xSemaphoreGive(saved_lock);
        if (!kept) { // forgotten while it was being joined: drop it
            msg_t m = {.type = MSG_RECONNECT, .reason = 1};
            post(&m);
        }
    } else if (trial && ev == JOIN_EV_GOT_IP_OTHER) {
        ESP_LOGW(TAG, "wifi: an IP, but not on the network being tried: not saved, still trying");
    }
}

static void handle_connected(msg_t const *m) {
    wifi_link_connected(&link_ap, m->ssid, strlen(m->ssid));
    if (retry_roam) {
        retry_roam    = false;
        retry_pending = false; // the roam arrived: a connect now would tear the new link down
    }
    read_back_if_up(); // the bandwidth and protocol are agreed at association: the registry's cache follows (#311)
}

static void handle_disconnected(int reason, uint32_t now) {
    bool was_connected = wifi_link_ssid(&link_ap) != NULL;
    wifi_link_down(&link_ap);
    portENTER_CRITICAL(&state_lock);
    first_connect_settled = true;
    wifi_count_disconnected(&counts, reason);
    wifi_join_action_t act    = wifi_join_event(&join, JOIN_EV_DISCONNECTED, reason, now);
    bool               have   = have_credentials;
    bool               trying = join.state == WIFI_JOIN_TRYING || join.state == WIFI_JOIN_PENDING;
    portEXIT_CRITICAL(&state_lock);
    if (act == JOIN_ACT_ROLLBACK) {
        rollback();
    } else if (have && atomic_load(&wifi_on)) {
        wifi_retry_t r = wifi_disconnect_retry(reason, was_connected);
        // Several saved networks: when this one keeps failing, the next known one in range gets its turn (#160)
        if (r.counts_as_failure && !trying && ++fail_count >= FAILS_PER_ROUND) {
            fail_count = 0;
            choose_network(true);
        }
        // Keep trying for as long as the badge runs: the network may come back (out of range, router restart). A roam
        // reconnects on the C6 by itself; the retry is only its fallback
        retry_pending = true;
        retry_roam    = r.cancel_on_connect;
        retry_at      = now + r.delay_ms;
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
    if (c6link_updating() || !wifi_has_ip(NULL, 0) || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    rssi_cached = ap.rssi;
    auth_cached = ap.authmode;
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
    trial_wipe();
    portENTER_CRITICAL(&state_lock);
    have_credentials = had;
    wifi_text_copy(kept_ssid, sizeof(kept_ssid), had ? s : "");
    wifi_text_copy(ssid, sizeof(ssid), had ? s : "");
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGW(TAG, "wifi: the C6 went away during a trial join: not saved");
}

// The supervisor re-attaches (#14 T-B): forget the connection and take the driver down. Errors are expected (the C6
// may be gone) and ignored; each RPC gives up after its own timeout, or at once when ESP-Hosted knows the transport
// is down (#90).
static void handle_c6_down(void) {
    atomic_store(&driver_up, false);
    atomic_store(&wifi_started, false);
    atomic_store(&driver_ready, false);
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    portENTER_CRITICAL(&state_lock);
    wifi_count_link_gone(&counts); // no disconnect event comes from a dead C6 (#415)
    portEXIT_CRITICAL(&state_lock);
    abort_trial();
    esp_err_t stop = esp_wifi_stop();
    // A dead C6 never answers the stop, so WIFI_EVENT_STA_DISCONNECTED/STA_STOP never come and the default handlers
    // never take the station netif down and out of lwIP; the STA_START of the re-attach then adds it a second time
    // ("netif_add: netif already added", bench 2026-11-01). Run the two default actions ourselves; if the events do
    // come later (live C6, fw c6 reattach) the handlers find the netif gone and do nothing
    esp_netif_action_disconnected(sta_netif, NULL, 0, NULL);
    esp_netif_action_stop(sta_netif, NULL, 0, NULL);
    esp_err_t gone = esp_wifi_deinit();
    ESP_LOGI(TAG, "wifi: down for the C6 re-attach (stop %s, deinit %s)", esp_err_to_name(stop), esp_err_to_name(gone));
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
        atomic_store(&driver_ready, false);
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOG_LEVEL(
        r == DRV_FAILED ? ESP_LOG_ERROR : ESP_LOG_INFO,
        TAG,
        "wifi: %s after the C6 re-attach",
        r == DRV_FAILED           ? why
        : r == DRV_OFF            ? "ready, switched off"
        : r == DRV_NO_CREDENTIALS ? "started, no network saved"
                                  : "started"
    );
}

// Wi-Fi switched off or on (#108). Off stops the radio on a live C6 (stop, not deinit: the next start is quick and the
// credentials stay in the driver), forgets the IP and an unfinished trial; the default handlers take the netif down on
// STA_STOP. On starts it; STA_START then connects with the saved pair. While the link is down the next attach decides.
static void handle_wifi_sync(void) {
    bool want = atomic_load(&wifi_on);
    if (!c6link_up() || !atomic_load(&driver_ready) || want == atomic_load(&wifi_started)) {
        return;
    }
    if (want) {
        portENTER_CRITICAL(&state_lock);
        first_connect_settled = false;
        portEXIT_CRITICAL(&state_lock);
        atomic_store(&driver_up, true); // before the start, as in wifi_driver_up
        set_country();
        esp_err_t err = esp_wifi_start();
        atomic_store(&wifi_started, err == ESP_OK);
        atomic_store(&driver_up, err == ESP_OK);
        ESP_LOG_LEVEL(
            err == ESP_OK ? ESP_LOG_INFO : ESP_LOG_ERROR,
            TAG,
            "wifi: switched on (%s)",
            esp_err_to_name(err)
        );
        return;
    }
    atomic_store(&driver_up, false);
    retry_pending = false;
    set_ip("");
    wifi_link_down(&link_ap);
    portENTER_CRITICAL(&state_lock);
    wifi_count_owner_down(&counts); // the stop's disconnect event may carry another reason than a local one (#415)
    portEXIT_CRITICAL(&state_lock);
    abort_trial();
    esp_err_t stop = esp_wifi_stop();
    atomic_store(&wifi_started, false);
    ESP_LOGI(TAG, "wifi: switched off (%s)", esp_err_to_name(stop));
}

size_t wifi_saved_names(char (*out)[33], size_t max) {
    return saved_lock && out ? wifi_saved_names_with(&saved_ops, SAVED_UI_WAIT_MS, out, max) : 0;
}

bool wifi_forget_network(char const *name) {
    if (!name || !saved_lock) {
        return false;
    }
    bool was_active = false, left = false;
    if (!saved_take(SAVED_FORGET_WAIT_MS)) { // c6wifi is busy with the list: the screen says "try again"
        return false;
    }
    bool ok = wifi_cred_forget(name, &was_active, &left);
    saved_give();
    // The driver may hold it without NVS saying so (a saved network joined by choose_network, or a trial under way)
    char cur[33], tried[33];
    portENTER_CRITICAL(&state_lock);
    wifi_text_copy(cur, sizeof(cur), ssid);
    wifi_text_copy(tried, sizeof(tried), trial_ssid);
    portEXIT_CRITICAL(&state_lock);
    if ((ok || was_active) && (was_active || strcmp(cur, name) == 0 || strcmp(tried, name) == 0)) { // the next saved one (or none) takes over
        msg_t m = {.type = left ? MSG_RECONNECT : MSG_FORGET, .reason = 1};
        for (int tries = 0; tries < 3 && !post(&m); tries++) { // a dropped revoke leaves the forgotten network in the driver
        }
    }
    return ok;
}

void wifi_tuning_get(wifi_tuning_t *t) {
    t->started = atomic_load(&wifi_started);
    t->tx_raw  = atomic_load(&wifi_tx_raw);
    t->ps_mode = atomic_load(&wifi_ps_mode);
    portENTER_CRITICAL(&state_lock);
    t->roams = counts.roams;
    portEXIT_CRITICAL(&state_lock);
}

void wifi_enable(bool on) {
    atomic_store(&wifi_on, on);
    msg_t m = {.type = MSG_WIFI_SYNC};
    post(&m); // before the task exists the flag alone is enough: the boot start reads it
}

bool wifi_enabled(void) {
    return atomic_load(&wifi_on);
}

static SemaphoreHandle_t c6_msg_done; // given by the task after MSG_C6_DOWN / MSG_C6_UP
static atomic_int        c6_msg_seq;  // the request the last give was for: a late one must not end the next wait

// Ask the c6wifi task to take the driver down (up=false) or bring it up again, and wait at most wait_ms for it
static bool driver_request(bool up, uint32_t wait_ms) {
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

static unsigned wifi_stack_free(void) {
    return wifi_task_handle ? (unsigned)uxTaskGetStackHighWaterMark(wifi_task_handle) : 0;
}

// ---- requests answered on the c6wifi task (#502) ------------------------------------------------------------------

// The caller (req_lock held) posts the request and waits for its answer. The slot's number keeps an answer that comes
// after the caller gave up from reaching the next caller.
static wifi_req_why_t request_locked(msg_type_t type, int value, uint32_t answer_ms, bool *ok) {
    wifi_req_why_t why = WIFI_REQ_BUSY;
    *ok                = false;
    portENTER_CRITICAL(&req_mux);
    uint32_t seq = wifi_req_begin(&req);
    portEXIT_CRITICAL(&req_mux);
    xSemaphoreTake(req_done, 0);
    msg_t m = {.type = type, .reason = (int)seq, .value = value};
    if (!post(&m)) {
        return why;
    }
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(answer_ms);
    for (;;) {
        portENTER_CRITICAL(&req_mux);
        bool done = wifi_req_result(&req, seq, ok, &why);
        portEXIT_CRITICAL(&req_mux);
        TickType_t left = end - xTaskGetTickCount();
        if (done || (int32_t)left <= 0) {
            break;
        }
        xSemaphoreTake(req_done, left);
    }
    return why;
}

static void answer(msg_t const *m, bool ok, wifi_req_why_t why) {
    portENTER_CRITICAL(&req_mux);
    bool mine = wifi_req_finish(&req, (uint32_t)m->reason, ok, why);
    portEXIT_CRITICAL(&req_mux);
    if (mine) {
        xSemaphoreGive(req_done);
    }
}

// The refusal that applies to a request of this kind right now (the plan, wifi_req.c, decides what to do with it)
static wifi_req_why_t gate_now(msg_type_t type) {
    portENTER_CRITICAL(&state_lock);
    wifi_join_state_t js = join.state;
    portEXIT_CRITICAL(&state_lock);
    bool roaming = retry_pending && retry_roam;
    if (type == MSG_SCAN) {
        return wifi_scan_gate(c6link_up(), c6link_updating(), atomic_load(&wifi_on), atomic_load(&wifi_started), js, roaming);
    }
    return (type == MSG_SET_PROTO || type == MSG_SET_BW) && atomic_load(&wifi_started) ? wifi_link_gate(js, roaming) : WIFI_REQ_OK;
}

static wifi_req_plan_t plan_for(msg_t const *m) {
    portENTER_CRITICAL(&req_mux);
    wifi_req_plan_t plan = wifi_req_plan(&req, (uint32_t)m->reason, WIFI_REQ_OK);
    portEXIT_CRITICAL(&req_mux);
    if (plan.run) { // the gate reads other locks: not under req_mux; the slot is only ours to answer
        plan.why = gate_now(m->type);
        plan.run = plan.why == WIFI_REQ_OK;
    }
    return plan;
}

static void handle_scan(msg_t const *m) {
    wifi_req_plan_t plan = plan_for(m); // a caller that gave up: do not spend seconds on the air for nobody
    size_t          n    = 0;
    bool            ok   = false;
    if (plan.run) {
        ok = wifi_scan_drv(scan_done, scan_res, SCAN_RECORDS, &n, scan_clean);
        plan.why = ok ? WIFI_REQ_OK : WIFI_REQ_FAILED;
    }
    if (plan.answer) {
        scan_n = n;
        answer(m, ok, plan.why);
    }
}

// The protocol and the bandwidth are agreed at association (the gate keeps a join or a roam out): set them in the
// driver first, then a station with credentials reconnects to take them. True only when the driver took them.
static bool apply_link(void) {
    bool ok = apply_wifi_bw();
    portENTER_CRITICAL(&state_lock);
    bool have = have_credentials;
    portEXIT_CRITICAL(&state_lock);
    if (have && atomic_load(&wifi_on)) {
        handle_reconnect(false);
    }
    readback_invalidate();
    return ok;
}

static bool apply_one(msg_type_t type) {
    bool ok = true;
    switch (type) {
        case MSG_SET_PS:
            ok = esp_wifi_set_ps((wifi_ps_type_t)atomic_load(&wifi_ps_mode)) == ESP_OK;
            readback_invalidate();
            break;
        case MSG_SET_TX: {
            int raw = atomic_load(&wifi_tx_raw);
            ok      = set_wifi_tx(raw ? raw : WIFI_TX_DEFAULT_RAW);
            break;
        }
        default: ok = apply_link(); break;
    }
    return ok;
}

static void handle_apply(msg_t const *m) {
    wifi_req_plan_t plan = plan_for(m);
    bool            ok   = false;
    if (plan.run) {
        _Atomic int *slot = choice_slot(m->type);
        int          old  = atomic_load(slot);
        atomic_store(slot, m->value); // the apply functions read the choice from here
        ok = apply_one(m->type);
        atomic_store(slot, wifi_req_keep(ok, old, m->value));
        readback_invalidate();
        plan.why = ok ? WIFI_REQ_OK : WIFI_REQ_FAILED;
    }
    if (plan.answer) {
        answer(m, ok, plan.why);
    }
}

static void handle_request(msg_t const *m) {
    if (m->type == MSG_SCAN) {
        handle_scan(m);
    } else {
        handle_apply(m);
    }
}

// STA_START, on the c6wifi task: the country once more if its set before this start failed (#311), the power save,
// bandwidth and TX power choices, then the registry's cache
static void on_sta_start(void) {
    if (atomic_load(&country_retry)) {
        set_country();
    }
    apply_wifi_ps();
    read_back_if_up();
}

// STA_START: the driver is up; connect with the pair in use, after letting the saved list pick the strongest in range
static void handle_connect(void) {
    portENTER_CRITICAL(&state_lock);
    bool have = have_credentials;
    portEXIT_CRITICAL(&state_lock);
    if (!atomic_load(&driver_up)) {
        return;
    }
    on_sta_start(); // the driver is up, also without credentials (scan only)
    if (!have) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    bool trying = join.state == WIFI_JOIN_TRYING || join.state == WIFI_JOIN_PENDING;
    portEXIT_CRITICAL(&state_lock);
    if (!trying) {
        tried_mask = fail_count = 0;
        choose_network(false);
    }
    if (wifi_apply_blocked()) {
        portENTER_CRITICAL(&state_lock);
        first_connect_settled = true; // no connect, so no event will settle it: the status bar must not stay "joining"
        portEXIT_CRITICAL(&state_lock);
    } else {
        esp_wifi_connect();
    }
}

static void wifi_task(void *arg) {
    (void)arg;
    msg_t m;
    for (;;) {
        bool     got = xQueueReceive(queue, &m, pdMS_TO_TICKS(250)) == pdTRUE;
        uint32_t now = now_ms();
        if (got) {
            switch (m.type) {
                case MSG_CONNECT: handle_connect(); break;
                case MSG_JOIN: handle_join(&m, now); break;
                case MSG_RECONNECT: handle_reconnect(m.reason == 1); break;
                case MSG_FORGET: handle_forget(); break;
                case MSG_DISCONNECTED: handle_disconnected(m.reason, now); break;
                case MSG_GOT_IP: handle_got_ip(); break;
                case MSG_WIFI_SYNC: handle_wifi_sync(); break;
                case MSG_CONNECTED: handle_connected(&m); break;
                case MSG_C6_DOWN:
                case MSG_C6_UP:
                    m.type == MSG_C6_UP ? handle_c6_up() : handle_c6_down();
                    atomic_store(&c6_msg_seq, m.reason);
                    xSemaphoreGive(c6_msg_done);
                    break;
                default: handle_request(&m); break; // scan, ps, tx, protocol, bandwidth (#502)
            }
            memset(&m, 0, sizeof(m));
        }
        if (retry_pending && (int32_t)(now - retry_at) >= 0) {
            retry_pending = false;
            if (atomic_load(&driver_up) && !wifi_apply_blocked()) {
                esp_wifi_connect();
            }
        }
        static uint32_t next_sync;
        if (!got && (int32_t)(now - next_sync) >= 0) {
            next_sync = now + 5000;
            handle_wifi_sync(); // idempotent: catches a MSG_WIFI_SYNC that was dropped on a full queue
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

// Visible as PENDING at once; only the task starts the trial (SSID and credential together) and so only it decides what an
// IP or a disconnect means. NVS is written once the pair has an IP. m.eap (a heap block) belongs to whoever takes the
// message; when the post fails it is wiped and freed here.
static bool request_join(msg_t *m) {
    portENTER_CRITICAL(&state_lock);
    wifi_join_t before = join;
    wifi_join_request(&join);
    wifi_text_copy(pending_ssid, sizeof(pending_ssid), m->ssid);
    portEXIT_CRITICAL(&state_lock);
    bool ok = post(m);
    if (!ok) {
        if (m->eap) {
            memset(m->eap, 0, sizeof(*m->eap));
            saved_release(m->eap);
        }
        portENTER_CRITICAL(&state_lock);
        join = before;
        portEXIT_CRITICAL(&state_lock);
    }
    memset(m, 0, sizeof(*m));
    return ok;
}

bool wifi_set_credentials(char const *new_ssid, char const *pass) {
    if (!c6link_up() || !atomic_load(&wifi_on) || !wifi_cred_valid(new_ssid, pass)) {
        return false;
    }
    msg_t m = {.type = MSG_JOIN};
    snprintf(m.ssid, sizeof(m.ssid), "%s", new_ssid);
    snprintf(m.pass, sizeof(m.pass), "%s", pass);
    return request_join(&m);
}

bool wifi_set_eap_credentials(char const *new_ssid, wifi_eap_t const *eap) {
    if (!c6link_up() || !atomic_load(&wifi_on) || !new_ssid || !wifi_cred_valid(new_ssid, "") || !wifi_eap_valid(eap)) {
        return false;
    }
    msg_t m = {.type = MSG_JOIN};
    m.eap   = saved_alloc(sizeof(*m.eap)); // the queue copies messages by value: the credential travels as a heap block
    if (!m.eap) {
        return false;
    }
    *m.eap = *eap;
    snprintf(m.ssid, sizeof(m.ssid), "%s", new_ssid);
    return request_join(&m);
}

wifi_auth_class_t wifi_auth_get(void) {
    portENTER_CRITICAL(&state_lock);
    bool known = rssi_known;
    int  mode  = auth_cached;
    portEXIT_CRITICAL(&state_lock);
    return wifi_auth_class(mode, known);
}

wifi_join_state_t wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap) {
    portENTER_CRITICAL(&state_lock);
    wifi_join_state_t state = join.state;
    if (tried && tried_cap) {
        wifi_text_copy(tried, tried_cap, state == WIFI_JOIN_PENDING ? pending_ssid : trial_ssid);
    }
    if (kept && kept_cap) {
        wifi_text_copy(kept, kept_cap, kept_ssid);
    }
    portEXIT_CRITICAL(&state_lock);
    return state;
}

// The radio switch hooks from main.c (wifi_radio_hooks_t), each with its default when it was not given
static bool airplane_on(void) {
    return radio.airplane && radio.airplane();
}

// `fw wifi on|off`: the OS switch is saved and its listener calls wifi_enable
static void cmd_wifi_switch(bool on) {
    if (!radio.set) {
        console_reply("wifi: no radio switch (not started)");
        return;
    }
    radio.set(on);
    console_reply("wifi: radio %s", (radio.get ? radio.get() : wifi_enabled()) ? "on" : "off");
}

// The connection line(s) of `fw wifi`: radio, credentials, link
static void wifi_status_link(void) {
    char a[140], ip[16], cur[33];
    portENTER_CRITICAL(&state_lock);
    bool have = have_credentials;
    wifi_text_copy(cur, sizeof(cur), ssid);
    portEXIT_CRITICAL(&state_lock);
    clean(cur, a, sizeof(a));
    if (!wifi_enabled()) {
        console_reply("wifi: radio switched off%s (fw wifi on)", airplane_on() ? ", airplane mode" : "");
    } else if (!have) {
        console_reply("wifi: no credentials (fw wifi scan | <ssid> <password> | reconnect | --clear)");
    } else if (wifi_has_ip(ip, sizeof(ip))) {
        int8_t rssi;
        if (wifi_rssi(&rssi)) {
            console_reply(
                "wifi: \"%s\" connected, ip %s (%s.local), rssi %d dBm, %d/4 bars",
                a,
                ip,
                lichen_mdns_name(),
                rssi,
                wifi_bars(rssi)
            );
        } else {
            console_reply("wifi: \"%s\" connected, ip %s (%s.local), rssi not read yet", a, ip, lichen_mdns_name());
        }
    } else {
        console_reply("wifi: \"%s\" not connected", a);
    }
}

static void cmd_wifi_status(wifi_cmd_t const *c) {
    (void)c;
    char a[140], b[140];
    wifi_status_link();
    wifi_apply_console_status();
    if (wifi_has_ip(NULL, 0)) {
        wifi_console_ip6();
    }
    // The chosen mode, not a read-back: the status line must not wait on a C6 RPC (`fw wifi ps` reads back)
    console_reply("wifi: power save %s", wifi_ps_name((wifi_ps_type_t)atomic_load(&wifi_ps_mode)));
    char              tried[33], kept[33];
    wifi_join_state_t st = wifi_join_state(tried, sizeof(tried), kept, sizeof(kept));
    clean(tried, a, sizeof(a));
    clean(kept, b, sizeof(b));
    if (st == WIFI_JOIN_TRYING || st == WIFI_JOIN_PENDING) {
        console_reply(
            "wifi: trying \"%s\" (saved only once it gets an IP, gives up after %d s)",
            a,
            WIFI_JOIN_TRIAL_MS / 1000
        );
    } else if (st == WIFI_JOIN_FAILED_KEPT_OLD) {
        console_reply(
            kept[0] ? "wifi: couldn't join \"%s\": kept \"%s\"" : "wifi: couldn't join \"%s\": no network saved",
            a,
            b
        );
    }
}

static void cmd_wifi_on_off(wifi_cmd_t const *c) {
    cmd_wifi_switch(c->on);
}

static void cmd_wifi_clear(wifi_cmd_t const *c) {
    (void)c;
    msg_t m = {.type = MSG_FORGET};
    console_reply(post(&m) ? "wifi: credentials removed" : "wifi: busy, try again");
}

static void cmd_wifi_reconnect(wifi_cmd_t const *c) {
    (void)c;
    char s[33], p[64];
    bool have = c6link_up() && wifi_cred_load(s, p);
    memset(p, 0, sizeof(p));
    msg_t m = {.type = MSG_RECONNECT};
    console_reply(
        have && post(&m) ? "wifi: reconnecting with the saved credentials" : "wifi: no saved credentials or no C6 link"
    );
}

static void cmd_wifi_eap(wifi_cmd_t const *c) {
    wifi_apply_console_eap(c->eap_args, &saved_ops);
}

static void cmd_wifi(char const *args) {
    static void (*const handler[WIFI_CMD_COUNT])(wifi_cmd_t const *) = {
        [WIFI_CMD_STATUS]      = cmd_wifi_status,
        [WIFI_CMD_SWITCH]      = cmd_wifi_on_off,
        [WIFI_CMD_PS]          = wifi_console_ps,
        [WIFI_CMD_PS_USAGE]    = wifi_console_ps_usage,
        [WIFI_CMD_STATS]       = wifi_console_stats,
        [WIFI_CMD_TX]          = wifi_console_tx,
        [WIFI_CMD_TX_USAGE]    = wifi_console_tx_usage,
        [WIFI_CMD_PROTO]       = wifi_console_proto,
        [WIFI_CMD_PROTO_USAGE] = wifi_console_proto_usage,
        [WIFI_CMD_BW]          = wifi_console_bw,
        [WIFI_CMD_CLEAR]       = cmd_wifi_clear,
        [WIFI_CMD_RECONNECT]   = cmd_wifi_reconnect,
        [WIFI_CMD_SCAN]        = wifi_console_scan,
        [WIFI_CMD_JOIN]        = wifi_console_join,
        [WIFI_CMD_JOIN_USAGE]  = wifi_console_join_usage,
        [WIFI_CMD_EAP]         = cmd_wifi_eap,
    };
    wifi_cmd_t c;
    wifi_cmd_parse(args, &c);
    handler[c.kind](&c);
}

static void cmd_net(char const *args) {
    (void)args;
    cmd_wifi("");
}

// ---- c6link's net hooks (#223) and the start ------------------------------------------------------------------

static bool hook_driver_down(uint32_t wait_ms) {
    return driver_request(false, wait_ms);
}

static bool hook_driver_up(uint32_t wait_ms) {
    return driver_request(true, wait_ms);
}

static void on_attached(void) {
    if (!start_wifi()) {
        ESP_LOGE(TAG, "Wi-Fi not started");
    }
}

static void on_no_link(void) {
    boot_progress_end(BOOT_WIFI, STEP_SKIPPED, "no C6");
    boot_progress_end(BOOT_CLOCK, STEP_SKIPPED, "no C6");
}

void wifi_service_init(wifi_radio_hooks_t const *radio_hooks) {
    radio = radio_hooks ? *radio_hooks : (wifi_radio_hooks_t){0};
    clock_console_register();
    console_register("net", "net (Wi-Fi state and IP)", cmd_net);
    console_register("wifi", "wifi [on | off | scan | reconnect | stats | ps [none | min] | tx <8-80> | bw 20|40 | proto n|ax | <ssid> <password> | eap <ssid> <peap|ttls> <mschapv2|pap> <identity|-> <username> <ca-path|-> <password> | --clear]", cmd_wifi);
    scan_done   = xSemaphoreCreateBinary();
    req_lock    = xSemaphoreCreateMutex();
    req_done    = xSemaphoreCreateBinary();
    saved_lock  = xSemaphoreCreateMutex();
    c6_msg_done = xSemaphoreCreateBinary();
    queue       = xQueueCreate(16, sizeof(msg_t));
    wifi_join_init(&join);
    if (!scan_done || !req_lock || !req_done || !saved_lock || !c6_msg_done || !queue ||
        // 5 KB: since #14 it also runs wifi_driver_up (esp_wifi_init, a wifi_config_t, RPCs) after a re-attach
        xTaskCreatePinnedToCore(wifi_task, "c6wifi", 5120, NULL, 5, &wifi_task_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi task did not start");
    }
    c6link_net_hooks_t const hooks = {
        .attached    = on_attached,
        .no_link     = on_no_link,
        .driver_down = hook_driver_down,
        .driver_up   = hook_driver_up,
        .stack_free  = wifi_stack_free,
    };
    c6link_set_net_hooks(&hooks);
}
