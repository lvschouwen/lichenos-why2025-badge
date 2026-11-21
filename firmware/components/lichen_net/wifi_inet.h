#pragma once

#include "esp_event.h"
#include "esp_netif.h"

// The station's IP plumbing, split out of wifi_service.c (#502, file size): DNS slots, IPv6 link-local and stateless
// DHCPv6 (#497), and the SNTP client with its plausibility filter (#8, #326). Log tag "c6link", as the service.

// DNS slots 2-3 hold the IPv4 servers of the lease: copy them at a got-IP, clear them on a disconnect
void wifi_dns_keep_v4(void);
void wifi_dns_clear_kept(void);
// Event handler for WIFI_EVENT_STA_CONNECTED, arg = the STA netif: makes the IPv6 link-local
void wifi_on_sta_connected_ip6(void *arg, esp_event_base_t base, int32_t id, void *data);
void wifi_dhcp6_stateless(esp_netif_t *sta_netif);
// Starts lwIP's SNTP client; the first accepted answer ends the boot clock step. sntp_sync_time() is the override that
// filters the answers.
void wifi_sntp_start(void);
