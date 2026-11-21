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
    c->roaming = false;
    if (c->lost) {
        c->lost = false;
        bump(&c->reconnects);
    }
    c->up = true;
}

void wifi_count_disconnected(wifi_count_t *c, int reason) {
    if (reason == WIFI_COUNT_REASON_LOCAL) {
        // Asked for: whatever was up is gone on purpose, and a loss still waiting for its reconnect is moot
        // A loss still waiting for its reconnect stays: a live C6 re-attach sends this late after wifi_count_link_gone
        c->lost    = c->lost && !c->up;
        c->up      = false;
        c->roaming = false;
        return;
    }
    if (reason == WIFI_COUNT_REASON_ROAM) {
        if (c->up) {
            c->up      = false;
            c->roaming = true;
            bump(&c->roams);
        }
        return;
    }
    if (c->roaming) {
        // The roam did not arrive: the link that was up before it is lost
        c->roaming = false;
        c->up      = true;
    }
    if (!c->up && !c->lost) {
        return; // nothing of ours was up: a stop event or a failed first join is not a loss we did not ask for
    }
    c->last_reason = clamp_reason(reason);
    if (c->up) {
        c->up   = false;
        c->lost = true;
        bump(&c->disconnects);
    }
}

void wifi_count_link_gone(wifi_count_t *c) {
    if (c->roaming) {
        c->roaming = false;
        c->up      = true;
    }
    if (c->up) {
        c->up   = false;
        c->lost = true;
        bump(&c->disconnects);
    }
}

void wifi_count_owner_down(wifi_count_t *c) {
    c->roaming = false;
    c->up   = false;
    c->lost = false;
}
