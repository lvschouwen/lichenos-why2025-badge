#include "wifi_console.h"

#include "esp_attr.h"
#include "fw_console.h"
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
    if (!wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &n)) {
        console_reply("wifi: scan failed");
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
