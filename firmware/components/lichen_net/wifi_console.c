#include "wifi_console.h"

#include "esp_attr.h"
#include "fw_console.h"
#include "wifi_regdom.h"
#include "wifi_service.h"

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
    if (c->tx_raw >= 0 && !wifi_set_tx(c->tx_raw)) { // bare "tx" (-1) reads back only; the range is wifi_set_tx's
        console_reply("wifi: TX power not applied (busy or refused)");
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
