#pragma once

#include "wifi_eap.h"
#include "wifi_list.h" // wifi_lock_t: only its alloc/release are used here

// The Enterprise calls on the C6 driver (#496): esp_eap_client_* and esp_wifi_sta_enterprise_* travel as ESP-Hosted RPCs
// and keep their state in the C6's RAM, which a C6 reset loses. Task rule as for esp_wifi_set_config: the c6wifi task
// (or the boot path before the driver starts), never the LVGL task. The password never leaves e and the RPC buffer.

// Step 1 of 2: reads the CA from e->ca_path (first, into a heap block that is freed on every path; no path clears the
// previous network's CA), then sets identity, username, password, method, the CA and the server name (e->server; there is
// no call that clears a name alone, wifi_eap_name_plan says how a stale one is dropped). *touched tells whether any call got
// through, i.e. whether the C6 holds Enterprise state now (an old C6 fails the first call: false, nothing to clear later).
// A refused call is a failed join, the caller must not connect. mem supplies the CA buffer (PSRAM).
wifi_eap_fail_t wifi_eap_drv_set(wifi_eap_t const *e, wifi_lock_t const *mem, bool *touched);
// Step 2 of 2, after esp_wifi_set_config (esp_eap_client.h: the Wi-Fi configuration is set before Enterprise is enabled)
wifi_eap_fail_t wifi_eap_drv_enable(void);
// Switches Enterprise off and clears the credentials and CA the C6 holds. The RPCs have no failure to report here.
void wifi_eap_drv_clear(void);
// The P4 side was reset (a new driver start, a reattach): the C6 may still hold a server name, so the next name-less join drops it
void wifi_eap_drv_reset(void);
