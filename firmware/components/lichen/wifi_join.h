#pragma once

#include <stdbool.h>
#include <stdint.h>

// The trial join (#66): new Wi-Fi credentials are applied to the driver but only saved once they got an IP. This is the
// pure decision part (host-tested); c6link.c feeds it events and does what it says.

typedef enum {
    WIFI_JOIN_NONE,            // no trial since boot
    WIFI_JOIN_PENDING,         // asked for, the task has not started it: events of the old connection decide nothing
    WIFI_JOIN_TRYING,          // new pair applied, waiting for an IP
    WIFI_JOIN_JOINED,          // the pair got an IP and was saved
    WIFI_JOIN_FAILED_KEPT_OLD, // no IP (or authentication failed): the previous pair is back
} wifi_join_state_t;

// GOT_IP_OTHER: an IP, but not on the trial's network (a late event of the old connection): decides nothing
typedef enum { JOIN_EV_GOT_IP, JOIN_EV_GOT_IP_OTHER, JOIN_EV_DISCONNECTED, JOIN_EV_TICK } wifi_join_event_t;
typedef enum { JOIN_ACT_NONE, JOIN_ACT_COMMIT, JOIN_ACT_ROLLBACK } wifi_join_action_t;

#define WIFI_JOIN_TRIAL_MS 20000

typedef struct {
    wifi_join_state_t state;
    uint32_t          started_ms;
} wifi_join_t;

void wifi_join_init(wifi_join_t *j);
// The request is queued: from now on events are ignored until wifi_join_start() runs on the task
void wifi_join_request(wifi_join_t *j);
void wifi_join_start(wifi_join_t *j, uint32_t now_ms);
// reason: the disconnect reason of JOIN_EV_DISCONNECTED (ignored otherwise)
wifi_join_action_t wifi_join_event(wifi_join_t *j, wifi_join_event_t ev, int reason, uint32_t now_ms);
// Which IP event it is: GOT_IP only when the connected AP's SSID (NULL: unknown) is the trial's, exactly
wifi_join_event_t wifi_join_ip_event(char const *trial_ssid, char const *ap_ssid);
// MIC failure, 4-way handshake timeout, auth failure, handshake timeout: a wrong password (or security mismatch)
bool wifi_reason_is_auth_failure(int reason);
