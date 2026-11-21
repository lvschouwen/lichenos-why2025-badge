#include "wifi_console.h"

#include "esp_attr.h"
#include "esp_wifi.h"
#include "fw_console.h"
#include "lichen_mdns.h"
#include "wifi_join_apply.h"
#include "wifi_regdom.h"
#include "wifi_service.h"

#include <string.h>

void wifi_console_ip6(void) {
    char     text[WIFI_IP6_TEXT_MAX];
    unsigned n;
    switch (wifi_ip6_get(text, sizeof(text), &n)) {
        case WIFI_IP6_GLOBAL: console_reply("wifi: ipv6 %s%s", text, n > 1 ? " (+more)" : ""); break;
        case WIFI_IP6_LINKLOCAL: console_reply("wifi: ipv6 link-local only (no router advertisement yet)"); break;
        default: console_reply("wifi: ipv6 none"); break;
    }
}

void wifi_console_scan(wifi_cmd_t const *c) {
    (void)c;
    EXT_RAM_BSS_ATTR static wifi_ap_t aps[20];
    char                              a[140];
    size_t                            n = 0;
    wifi_req_why_t why = WIFI_REQ_OK;
    if (!wifi_scan_why(aps, sizeof(aps) / sizeof(aps[0]), &n, &why)) {
        console_reply("wifi: scan not done: %s", wifi_req_why_text(why));
        return;
    }
    console_reply("wifi: %u networks", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        wifi_ssid_sanitize(aps[i].ssid, a, sizeof(a)); // air-sourced: control bytes are escaped as \xNN
        console_reply(
            "  %4d dBm  bars %d  %s  %s",
            aps[i].rssi,
            wifi_bars(aps[i].rssi),
            aps[i].open ? "open  " : "locked",
            a
        );
    }
}

void wifi_console_join_usage(wifi_cmd_t const *c) {
    (void)c;
    console_reply("wifi <ssid> <password> (SSID without spaces, at most 32 characters)");
}

void wifi_console_join(wifi_cmd_t const *c) {
    char a[140];
    wifi_ssid_sanitize(c->ssid, a, sizeof(a));
    if (!wifi_cred_valid(c->ssid, c->pass)) {
        console_reply("wifi: the password is empty (open network) or 8-63 characters");
    } else if (!wifi_set_credentials(c->ssid, c->pass)) {
        console_reply("wifi: could not start (no C6 link or busy)");
    } else {
        console_reply(
            "wifi: trying \"%s\", saved only if it gets an IP within %d s; fw wifi shows the result",
            a,
            WIFI_JOIN_TRIAL_MS / 1000
        );
    }
}

// `fw wifi ps|tx|bw` (#311, #502): ps is an alias of the registry's settable wifi.ps. tx and bw are not thin aliases:
// wifi.txpwr and wifi.bw are RO|TX in the registry and these commands are their guarded path. Each echoes the value the
// driver applies (read back now, the same read `fw get wifi.` shows), never the request alone
static void tuning_echo(char kind, unsigned asked_mhz, bool reconnecting) {
    wifi_readback_t r;
    char            line[120];
    wifi_readback(&r, true);
    if (kind == 't') {
        wifi_tx_reply(&r, line, sizeof(line));
    } else if (kind == 'b') {
        wifi_bw_reply(&r, asked_mhz, reconnecting, line, sizeof(line));
    } else {
        wifi_ps_reply(&r, line, sizeof(line));
    }
    console_reply("%s", line);
}

void wifi_console_ps_usage(wifi_cmd_t const *c) {
    (void)c;
    console_reply("wifi ps [none | min]"); // a typo must not fall through to joining an SSID called "ps"
}

void wifi_console_ps(wifi_cmd_t const *c) {
    if (c->set && !wifi_set_ps(c->ps_min)) {
        console_reply("wifi: power save not set (no C6 link?)");
    }
    tuning_echo('p', 0, false);
}

void wifi_console_tx_usage(wifi_cmd_t const *c) {
    (void)c;
    console_reply("wifi tx <8-80> (0.25 dBm units: 34 = 8.5, 52 = 13, 80 = 20 dBm) | wifi tx 0 (default)");
}

void wifi_console_tx(wifi_cmd_t const *c) {
    wifi_req_why_t why = WIFI_REQ_OK;
    if (c->tx_raw >= 0 && !wifi_set_tx(c->tx_raw, &why)) { // bare "tx" (-1) reads back only; the range is wifi_set_tx's
        console_reply("wifi: TX power not applied: %s", wifi_req_why_text(why));
    }
    tuning_echo('t', 0, false);
}

void wifi_console_proto_usage(wifi_cmd_t const *c) {
    (void)c;
    console_reply("wifi proto n | ax (n: b/g/n only, no Wi-Fi 6; reconnects to apply, not saved)");
}

// An alias of the registry's wifi.protocol (settable): the same set, then the applied read-back
void wifi_console_proto(wifi_cmd_t const *c) {
    wifi_req_why_t why = WIFI_REQ_OK;
    if (!wifi_set_proto(c->proto_n, &why)) {
        console_reply("wifi: protocol not applied: %s", wifi_req_why_text(why));
    }
    wifi_readback_t r;
    char            line[120];
    wifi_readback(&r, true);
    wifi_proto_reply(&r, line, sizeof(line));
    console_reply("%s", line);
}

void wifi_console_bw(wifi_cmd_t const *c) {
    bool reconnecting = false;
    if (c->bw_mhz) {
        wifi_req_why_t why = WIFI_REQ_OK;
        if (!wifi_set_bw(c->bw_mhz, &why)) {
            console_reply("wifi: bandwidth not applied: %s", wifi_req_why_text(why));
        } else {
            reconnecting = wifi_has_credentials() && wifi_enabled(); // only then the task issues a reconnect
        }
    }
    tuning_echo('b', c->bw_mhz, reconnecting);
}

void wifi_console_stats(wifi_cmd_t const *c) {
    (void)c;
    wifi_tuning_t t;
    wifi_tuning_get(&t);
    wifi_ap_record_t ap;
    if (!t.started || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        console_reply("wifi: not connected");
        return;
    }
    wifi_bandwidth_t bw     = 0;
    int8_t           pwr    = 0;
    bool             bw_ok  = esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) == ESP_OK;
    bool             pwr_ok = esp_wifi_get_max_tx_power(&pwr) == ESP_OK;
    console_reply(
        "wifi: rssi %d dBm, channel %u%s, phy%s%s%s%s",
        ap.rssi,
        ap.primary,
        ap.second == WIFI_SECOND_CHAN_ABOVE   ? "+above"
        : ap.second == WIFI_SECOND_CHAN_BELOW ? "+below"
                                              : "",
        ap.phy_11b ? " b" : "",
        ap.phy_11g ? " g" : "",
        ap.phy_11n ? " n" : "",
        ap.phy_11ax ? " ax" : ""
    );
    console_reply(
        "wifi: bandwidth %s, max TX power %s%d.%02d dBm (%s), power save %s",
        !bw_ok            ? "?"
        : bw == WIFI_BW20 ? "HT20"
        : bw == WIFI_BW40 ? "HT40"
                          : "other",
        pwr_ok ? "" : "? ",
        pwr / 4,
        (pwr % 4) * 25,
        t.tx_raw ? "set" : "default",
        wifi_ps_name(t.ps_mode)
    );
    // The C6's own copy of the STA config: proof the 11k/v flags crossed the link (#495). It holds the password: wiped
    wifi_config_t cfg    = {0};
    bool          cfg_ok = esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK;
    bool          rm = cfg.sta.rm_enabled, btm = cfg.sta.btm_enabled;
    memset(&cfg, 0, sizeof(cfg));
    console_reply(
        "wifi: roaming 11k %s, 11v %s, roams since boot %lu",
        !cfg_ok ? "?" : rm ? "on" : "off",
        !cfg_ok ? "?" : btm ? "on" : "off",
        (unsigned long)t.roams
    );
}

// The connection line(s) of `fw wifi`: radio, credentials, link
static void status_link(wifi_tuning_t const *t) {
    char a[140], ip[16];
    bool have = t->have_credentials;
    wifi_ssid_sanitize(t->ssid, a, sizeof(a));
    if (!wifi_enabled()) {
        console_reply("wifi: radio switched off%s (fw wifi on)", t->airplane ? ", airplane mode" : "");
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

void wifi_console_status(wifi_cmd_t const *c) {
    (void)c;
    char            a[140], b[140];
    wifi_tuning_t   t;
    wifi_tuning_get(&t);
    status_link(&t);
    wifi_apply_console_status();
    if (wifi_has_ip(NULL, 0)) {
        wifi_console_ip6();
    }
    // The chosen mode, not a read-back: the status line must not wait on a C6 RPC (`fw wifi ps` reads back)
    console_reply("wifi: power save %s", wifi_ps_name(t.ps_mode));
    char              tried[33], kept[33];
    wifi_join_state_t st = wifi_join_state(tried, sizeof(tried), kept, sizeof(kept));
    wifi_ssid_sanitize(tried, a, sizeof(a));
    wifi_ssid_sanitize(kept, b, sizeof(b));
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
