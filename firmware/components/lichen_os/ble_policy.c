#include "ble_policy.h"

#include <string.h>

// Elapsed since then, right across the counter's wrap
static uint32_t since(uint32_t now_ms, uint32_t then_ms) {
    return now_ms - then_ms;
}

void ble_pair_init(ble_pair_t *p) {
    memset(p, 0, sizeof(*p));
}

// A lockout that ran out clears itself, with its failures: the next window starts fresh
static void pair_unlock_due(ble_pair_t *p, uint32_t now_ms) {
    if (p->locked && since(now_ms, p->locked_ms) >= BLE_PAIR_LOCKOUT_MS) {
        p->locked = false;
        p->fails  = 0;
    }
}

void ble_pair_open(ble_pair_t *p, uint32_t now_ms) {
    p->open      = true;
    p->opened_ms = now_ms;
}

void ble_pair_close(ble_pair_t *p) {
    p->open = false;
}

bool ble_pair_is_open(ble_pair_t const *p, uint32_t now_ms) {
    return p->open && since(now_ms, p->opened_ms) < BLE_PAIR_WINDOW_MS;
}

uint32_t ble_pair_left_ms(ble_pair_t const *p, uint32_t now_ms) {
    return ble_pair_is_open(p, now_ms) ? BLE_PAIR_WINDOW_MS - since(now_ms, p->opened_ms) : 0;
}

uint32_t ble_pair_locked_ms(ble_pair_t const *p, uint32_t now_ms) {
    if (!p->locked || since(now_ms, p->locked_ms) >= BLE_PAIR_LOCKOUT_MS) {
        return 0;
    }
    return BLE_PAIR_LOCKOUT_MS - since(now_ms, p->locked_ms);
}

ble_pair_verdict_t ble_pair_begin(ble_pair_t *p, uint32_t now_ms, unsigned bonds) {
    pair_unlock_due(p, now_ms);
    if (!ble_pair_is_open(p, now_ms)) {
        return BLE_PAIR_CLOSED;
    }
    if (p->locked) {
        return BLE_PAIR_LOCKED;
    }
    if (p->busy) {
        return BLE_PAIR_BUSY;
    }
    if (bonds >= BLE_BONDS_MAX) {
        return BLE_PAIR_BONDS_FULL;
    }
    p->busy = true;
    return BLE_PAIR_GO;
}

bool ble_pair_end(ble_pair_t *p, uint32_t now_ms, bool ok) {
    if (!p->busy) {
        return !ok; // no pairing began (a stale callback): nothing counted, a failed link still goes
    }
    p->busy = false;
    if (ok) {
        p->fails = 0;
        p->open  = false; // one pairing per window
        return false;
    }
    if (++p->fails >= BLE_PAIR_FAILS_MAX) {
        p->locked    = true;
        p->locked_ms = now_ms;
    }
    return true;
}

ble_access_t ble_access(lichen_bt_level_t need, ble_link_sec_t sec) {
    switch (need) {
        case LICHEN_BT_OPEN: return BLE_ACCESS_OK;
        case LICHEN_BT_ENCRYPTED: return sec.encrypted ? BLE_ACCESS_OK : BLE_ACCESS_NEED_ENC;
        case LICHEN_BT_BONDED:
            return sec.encrypted && sec.authenticated && sec.bonded ? BLE_ACCESS_OK : BLE_ACCESS_NEED_AUTH;
    }
    return BLE_ACCESS_NEED_AUTH; // an unknown level is the strictest
}

uint8_t ble_access_att_error(ble_access_t a) {
    switch (a) {
        case BLE_ACCESS_OK: return 0;
        case BLE_ACCESS_NEED_ENC: return 0x0f;
        case BLE_ACCESS_NEED_AUTH: break;
    }
    return 0x05;
}

bool ble_conn_may(ble_conns_t const *c, lichen_bt_role_t role) {
    if ((unsigned)c->central + c->peripheral >= BLE_CONN_MAX) {
        return false;
    }
    if (role == LICHEN_BT_ROLE_CENTRAL) {
        return c->central < BLE_CONN_CENTRAL_MAX;
    }
    return role == LICHEN_BT_ROLE_PERIPHERAL && c->peripheral < BLE_CONN_PERIPHERAL_MAX;
}

bool ble_conn_add(ble_conns_t *c, lichen_bt_role_t role) {
    if (!ble_conn_may(c, role)) {
        return false;
    }
    if (role == LICHEN_BT_ROLE_CENTRAL) {
        c->central++;
    } else {
        c->peripheral++;
    }
    return true;
}

void ble_conn_remove(ble_conns_t *c, lichen_bt_role_t role) {
    if (role == LICHEN_BT_ROLE_CENTRAL && c->central > 0) {
        c->central--;
    } else if (role == LICHEN_BT_ROLE_PERIPHERAL && c->peripheral > 0) {
        c->peripheral--;
    }
}

bool ble_conn_may_advertise(ble_conns_t const *c) {
    return ble_conn_may(c, LICHEN_BT_ROLE_PERIPHERAL);
}

void ble_link_init(ble_link_t *l, lichen_bt_role_t role, uint32_t now_ms) {
    memset(l, 0, sizeof(*l));
    l->role  = role;
    l->kind  = BLE_LINK_PLAIN;
    l->up_ms = now_ms;
}

void ble_link_pairing(ble_link_t *l, uint32_t now_ms, bool under_way) {
    l->pairing    = under_way;
    l->pairing_ms = now_ms;
}

bool ble_link_should_drop(ble_link_t const *l, uint32_t now_ms) {
    if (l->role != LICHEN_BT_ROLE_PERIPHERAL || l->kind != BLE_LINK_PLAIN || l->sec.bonded) {
        return false;
    }
    if (l->pairing) {
        return since(now_ms, l->pairing_ms) >= BLE_PAIR_SMP_MS;
    }
    return since(now_ms, l->up_ms) >= BLE_UNBONDED_DROP_MS;
}

// 2^32 rounded down to whole millions: below it, r % 10^6 is uniform
#define PASSKEY_LIMIT 4294000000u

bool ble_passkey_from_random(uint32_t r, uint32_t *passkey) {
    if (r >= PASSKEY_LIMIT) {
        return false;
    }
    *passkey = r % (BLE_PASSKEY_MAX + 1u);
    return true;
}

bool ble_passkey_format(uint32_t passkey, char out[BLE_PASSKEY_TEXT]) {
    out[0] = '\0';
    if (passkey > BLE_PASSKEY_MAX) {
        return false;
    }
    for (int i = 6; i >= 0; i--) {
        if (i == 3) {
            out[i] = ' ';
            continue;
        }
        out[i]   = (char)('0' + passkey % 10u);
        passkey /= 10u;
    }
    out[7] = '\0';
    return true;
}
