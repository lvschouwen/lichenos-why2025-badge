#include "lichen_mdns.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "mdns_name.h"

static char const *TAG = "mdns";

static char name[MDNS_NAME_MAX];
static bool up, service_up;

char const *lichen_mdns_name(void) {
    if (!name[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        mdns_name_from_mac(mac, name, sizeof(name));
    }
    return name;
}

static void bring_up(void) {
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(lichen_mdns_name());
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS not started: %s", esp_err_to_name(err));
        mdns_free(); // retried on the next call
        return;
    }
    mdns_instance_name_set("Lichen badge");
    up = true;
    ESP_LOGI(TAG, "answering to %s.local", lichen_mdns_name());
}

void lichen_mdns_update(bool net, bool server, unsigned short port) {
    if (net && !up) {
        bring_up();
    } else if (!net && up) {
        mdns_free(); // takes the service with it
        up = service_up = false;
        ESP_LOGI(TAG, "stopped");
    }
    if (!up) {
        return;
    }
    if (server && !service_up) {
        if (mdns_service_add("Lichen update server", "_http", "_tcp", port, NULL, 0) == ESP_OK) {
            service_up = true;
        }
    } else if (!server && service_up) {
        mdns_service_remove("_http", "_tcp");
        service_up = false;
    }
}
