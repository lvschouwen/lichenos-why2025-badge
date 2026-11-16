#include "wifi_count.h"

#include <string.h>

static void bump(uint32_t *n) {
    if (*n < UINT32_MAX) {
        (*n)++;
    }
}

static uint16_t clamp_reason(int reason) {
    if (reason < 0) {
        return 0;
    }
    return reason > UINT16_MAX ? UINT16_MAX : (uint16_t)reason;
}

void wifi_count_init(wifi_count_t *c) {
    memset(c, 0, sizeof(*c));
}

void wifi_count_got_ip(wifi_count_t *c) {
    if (c->lost) {
        c->lost = false;
        bump(&c->reconnects);
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
    c->last_reason = clamp_reason(reason);
    if (c->up) {
        c->up   = false;
        c->lost = true;
        bump(&c->disconnects);
    }
}

void wifi_count_link_gone(wifi_count_t *c) {
    if (c->up) {
        c->up   = false;
        c->lost = true;
        bump(&c->disconnects);
    }
}

void wifi_count_owner_down(wifi_count_t *c) {
    c->up   = false;
    c->lost = false;
}
