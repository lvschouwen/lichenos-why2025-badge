#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The station's IPv6 addresses (#497): which ones count, the level the registry key wifi.ip6 reports and the text the
// console and the Wi-Fi screen show. Pure, host-tested (wifi_ip6_test.c); wifi_service.c fills it from the netif.
// Addresses are 16 bytes in network order.
#define WIFI_IP6_MAX      4 // LWIP_IPV6_NUM_ADDRESSES is 3; room to spare
#define WIFI_IP6_TEXT_MAX 40 // 8 groups of 4 hex digits, 7 colons, NUL

typedef enum {
    WIFI_IP6_CLASS_OTHER,     // unspecified, loopback, multicast, v4-mapped and the rest: never kept
    WIFI_IP6_CLASS_LINKLOCAL, // fe80::/10
    WIFI_IP6_CLASS_ULA,       // fc00::/7: routable inside the site only
    WIFI_IP6_CLASS_GLOBAL,    // 2000::/3
} wifi_ip6_class_t;

// The value of wifi.ip6 (enum order is the key's enum_names)
typedef enum {
    WIFI_IP6_NONE,      // no IPv6 address (not connected, or the link-local was not made)
    WIFI_IP6_LINKLOCAL, // only the link-local address
    WIFI_IP6_GLOBAL,    // an address beyond the link: a global one from SLAAC, or a site-local ULA one
} wifi_ip6_level_t;

typedef struct {
    uint8_t addr[WIFI_IP6_MAX][16];
    uint8_t n;
} wifi_ip6_set_t;

wifi_ip6_class_t wifi_ip6_classify(uint8_t const a[16]);

void wifi_ip6_clear(wifi_ip6_set_t *s);
// Keeps a link-local, ULA or global address once; false for any other, a duplicate or a full set
bool wifi_ip6_add(wifi_ip6_set_t *s, uint8_t const a[16]);
wifi_ip6_level_t wifi_ip6_level(wifi_ip6_set_t const *s);
// How many addresses are beyond the link (the global and ULA ones)
unsigned wifi_ip6_global_count(wifi_ip6_set_t const *s);
// The address to show: the first global one, else the first ULA one; false when there is none beyond the link
bool wifi_ip6_best(wifi_ip6_set_t const *s, uint8_t out[16]);
// RFC 5952 text ("2001:db8::1": the longest run of two or more zero groups becomes "::", lower case); the length
// written without the NUL, 0 when cap is under WIFI_IP6_TEXT_MAX
size_t wifi_ip6_text(uint8_t const a[16], char *out, size_t cap);
