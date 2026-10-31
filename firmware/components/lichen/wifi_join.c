#include "wifi_join.h"

#include <string.h>

void wifi_join_init(wifi_join_t *j) {
    j->state      = WIFI_JOIN_NONE;
    j->started_ms = 0;
}

void wifi_join_request(wifi_join_t *j) {
    j->state = WIFI_JOIN_PENDING;
}

void wifi_join_start(wifi_join_t *j, uint32_t now_ms) {
    j->state      = WIFI_JOIN_TRYING;
    j->started_ms = now_ms;
}

// WIFI_REASON_MIC_FAILURE 14, 4WAY_HANDSHAKE_TIMEOUT 15, AUTH_FAIL 202, HANDSHAKE_TIMEOUT 204
bool wifi_reason_is_auth_failure(int reason) {
    return reason == 14 || reason == 15 || reason == 202 || reason == 204;
}

wifi_join_event_t wifi_join_ip_event(char const *trial_ssid, char const *ap_ssid) {
    bool same = trial_ssid && ap_ssid && trial_ssid[0] && strcmp(trial_ssid, ap_ssid) == 0;
    return same ? JOIN_EV_GOT_IP : JOIN_EV_GOT_IP_OTHER;
}

wifi_join_action_t wifi_join_event(wifi_join_t *j, wifi_join_event_t ev, int reason, uint32_t now_ms) {
    if (j->state != WIFI_JOIN_TRYING) {
        return JOIN_ACT_NONE;
    }
    switch (ev) {
        case JOIN_EV_GOT_IP:
            j->state = WIFI_JOIN_JOINED;
            return JOIN_ACT_COMMIT;
        case JOIN_EV_GOT_IP_OTHER: return JOIN_ACT_NONE; // not the trial's network: it still has to prove itself
        case JOIN_EV_DISCONNECTED:
            if (wifi_reason_is_auth_failure(reason)) {
                j->state = WIFI_JOIN_FAILED_KEPT_OLD;
                return JOIN_ACT_ROLLBACK;
            }
            return JOIN_ACT_NONE;
        case JOIN_EV_TICK:
            if ((uint32_t)(now_ms - j->started_ms) >= WIFI_JOIN_TRIAL_MS) {
                j->state = WIFI_JOIN_FAILED_KEPT_OLD;
                return JOIN_ACT_ROLLBACK;
            }
            return JOIN_ACT_NONE;
    }
    return JOIN_ACT_NONE;
}
