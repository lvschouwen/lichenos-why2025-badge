#pragma once

#include "wifi_eap.h"
#include "wifi_list.h" // wifi_lock_t: only its alloc/release are used here

// The Enterprise calls on the C6 driver (#496): esp_eap_client_* and esp_wifi_sta_enterprise_* travel as ESP-Hosted RPCs
// and keep their state in the C6's RAM, which a C6 reset loses. Task rule as for esp_wifi_set_config: the c6wifi task
// (or the boot path before the driver starts), never the LVGL task. The password never leaves e and the RPC buffer.

// Sets identity, username, password, the method, and the CA (read from e->ca_path; no path clears the previous
// network's CA), then switches Enterprise on. WIFI_EAP_FAIL_NONE on success; a refused call is a failed join, the
// caller must not connect. mem supplies the CA buffer (PSRAM, not the 5 KB task stack).
wifi_eap_fail_t wifi_eap_drv_set(wifi_eap_t const *e, wifi_lock_t const *mem);
// Switches Enterprise off and clears the credentials and CA the C6 holds. The RPCs have no failure to report here.
void wifi_eap_drv_clear(void);
