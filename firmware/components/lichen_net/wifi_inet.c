// The station's IP plumbing, split out of wifi_service.c (#502): see wifi_inet.h
#include "wifi_inet.h"

#include "boot_progress.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "lichen_clock.h"
#include "lwip/dhcp6.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include <sys/time.h>

static char const TAG[] = "c6link";

// DNS slots (#497). lwIP's RDNSS (every router advertisement) and stateless DHCPv6 write the servers from slot 0 up,
// as does the IPv4 DHCP client, and lwIP has no offset for either. With CONFIG_LWIP_DNS_MAX_SERVERS=4 the layout is:
// slots 0-1 whichever of IPv6 (RDNSS, DHCPv6) or IPv4 wrote last, slots 2-3 the IPv4 servers of the lease (copied here
// at every got-IP, renewals included), so a network whose IPv6 resolvers fail still resolves over IPv4: lwIP's
// resolver walks all slots on a timeout. Only a DHCPv6 reply with 3 or more servers reaches into slots 2-3.
// Slots 2-3 are cleared first (and on a disconnect): IDF's dns_clear_servers keeps the last slot, and a lease with no
// DNS must not leave the previous network's servers behind.
void wifi_dns_clear_kept(void) {
    for (u8_t i = 2; i < DNS_MAX_SERVERS; i++) {
        dns_setserver(i, NULL);
    }
}

void wifi_dns_keep_v4(void) {
    LOCK_TCPIP_CORE();
    wifi_dns_clear_kept();
    u8_t out = 2;
    for (u8_t i = 0; i < 2 && out < DNS_MAX_SERVERS; i++) {
        ip_addr_t dns = *dns_getserver(i);
        if (IP_GET_TYPE(&dns) == IPADDR_TYPE_V4 && !ip_addr_isany(&dns)) {
            dns_setserver(out++, &dns);
        }
    }
    UNLOCK_TCPIP_CORE();
}


// The STA's link-local (#497): IDF only makes it for NAN (wifi_default.c), and without one there is no router
// solicitation and no SLAAC. esp_netif_create_ip6_linklocal fails while the netif is down, and IDF brings it up in its
// own STA_CONNECTED handler. The event loop runs the handlers of the whole event base (ANY_ID, as on_event) before the
// ones of one id, and the ones of an id in registration order. So this id-specific handler is registered after
// esp_netif_create_default_wifi_sta, which registers the default one, and runs behind it. arg: the STA netif.
void wifi_on_sta_connected_ip6(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_netif_t *sta_netif = arg;
    (void)base;
    (void)id;
    (void)data;
    esp_ip6_addr_t have;
    if (esp_netif_get_ip6_linklocal(sta_netif, &have) == ESP_OK) {
        return; // a roam (11k/v) is a CONNECTED too: re-creating it would restart DAD on the working address
    }
    esp_err_t err = esp_netif_create_ip6_linklocal(sta_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: no IPv6 link-local: %s", esp_err_to_name(err));
    }
}


static void on_time_sync(struct timeval *tv) {
    ESP_LOGI(TAG, "clock set from SNTP: %lld", (long long)tv->tv_sec);
    boot_progress_end(BOOT_CLOCK, STEP_DONE, NULL);
}

// lwIP's own sntp_sync_time() is weak and calls settimeofday() before the sync callback, so a filter in the callback
// would be too late (#8). This one checks the answer first: an implausible or far-off time is logged and ignored.
void sntp_sync_time(struct timeval *tv) {
    clock_answer_t  a;
    clock_verdict_t v = clock_offer_sntp(tv, &a);
    if (!clock_accepted(v)) {
        ESP_LOGW(TAG, "SNTP time %lld ignored: %s", (long long)tv->tv_sec, clock_verdict_text(v));
        return;
    }
    if (a.how == CLOCK_FIX_NONE) {
        ESP_LOGW(TAG, "SNTP time %lld accepted but not set", (long long)tv->tv_sec);
        return;
    }
    if (v == CLOCK_RECOVERED) {
        ESP_LOGW(TAG, "SNTP: %s, clock moved", clock_verdict_text(v));
    }
    // #326: an offset under a second is slewed (adjtime), so the hourly corrections log "slewed", not a step. From
    // this answer's own result: a second answer in between cannot relabel it.
    ESP_LOGI(
        TAG,
        "SNTP %s %+lld ms after %lld s uptime",
        a.how == CLOCK_FIX_SLEW ? "slewed" : "step",
        (long long)a.step_ms,
        (long long)(a.mono_ms / 1000)
    );
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    on_time_sync(tv);
}

// Real time for MeshCore's adverts and messages (#65): other nodes drop adverts that aren't newer than the last they
// saw. lwIP's SNTP client keeps retrying until the station has an IP, then refreshes hourly.
void wifi_sntp_start(void) {
    esp_sntp_config_t cfg  = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb            = on_time_sync;
    esp_err_t err          = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP: %s", esp_err_to_name(err));
    }
}


// Stateless DHCPv6 (#497): lwIP asks for the DNS servers only after a router advertisement with the "other config"
// flag. Needs the lwIP netif, and with TCPIP_CORE_LOCKING the core lock around the call.
void wifi_dhcp6_stateless(esp_netif_t *sta_netif) {
    struct netif *n = esp_netif_get_netif_impl(sta_netif);
    if (!n) {
        return;
    }
    LOCK_TCPIP_CORE();
    err_t err = dhcp6_enable_stateless(n);
    UNLOCK_TCPIP_CORE();
    if (err != ERR_OK) {
        ESP_LOGW(TAG, "wifi: stateless DHCPv6 not enabled (%d)", (int)err);
    }
}
