#include "wifi_count.h"

#include <string.h>

void wifi_count_init(wifi_count_t *c) {
    memset(c, 0, sizeof(*c));
}

void wifi_count_got_ip(wifi_count_t *c) {
    if (c->lost) {
        c->lost = false;
        if (c->reconnects < UINT32_MAX) {
            c->reconnects++;
        }
    }
    c->up = true;
}

void wifi_count_disconnected(wifi_count_t *c, int reason) {
    if (reason == WIFI_COUNT_REASON_LOCAL) {
        // Asked for: whatever was up is gone on purpose, and a loss still waiting for its reconnect is moot
        c->up   = false;
        c->lost = false;
        return;
    }
    c->last_reason = (uint16_t)(reason < 0 ? 0 : reason > UINT16_MAX ? UINT16_MAX : reason);
    if (c->up) {
        c->up   = false;
        c->lost = true;
        if (c->disconnects < UINT32_MAX) {
            c->disconnects++;
        }
    }
}
