#include "bt_sync.h"

bt_sync_act_t bt_sync_decide(bt_sync_in_t const *in) {
    if (in->support == RP_OLD_C6 || in->support == RP_NO_DOMAIN || in->unknown_id) {
        return BT_SYNC_UNSUPPORTED;
    }
    if (in->support == RP_WAIT_CAPS) {
        return BT_SYNC_WAIT;
    }
    if (in->applied_known && in->applied_on == in->want) {
        return BT_SYNC_IDLE;
    }
    if (in->tries > 0 && in->since_sent_ms < BT_SYNC_RETRY_MS) {
        return BT_SYNC_WAIT; // the answer to the last SET may still come
    }
    return in->tries >= BT_SYNC_TRIES ? BT_SYNC_GIVE_UP : BT_SYNC_SEND;
}

void bt_sync_wish(bt_sync_t *s, bool on) {
    if (!s->asked || s->want != on) {
        s->tries = 0;
        s->said  = false;
    }
    s->asked = true;
    s->want  = on;
}

void bt_sync_link(bt_sync_t *s) {
    s->tries = 0;
    s->said  = false;
}

bt_sync_in_t bt_sync_input(bt_sync_t const *s, reg_proxy_slot_t const *slot, rp_support_t support, uint32_t now_ms) {
    bool const fresh = slot && slot->have && !slot->stale;
    return (bt_sync_in_t){.want          = s->want,
                          .support       = support,
                          .applied_known = fresh && slot->status == REG_OK,
                          .applied_on    = fresh && slot->status == REG_OK && slot->value != 0,
                          .unknown_id    = fresh && slot->status == REG_UNKNOWN,
                          .tries         = s->tries,
                          .since_sent_ms = now_ms - s->sent_ms};
}

bt_sync_act_t bt_sync_step(bt_sync_t const *s, reg_proxy_slot_t const *slot, rp_support_t support, uint32_t now_ms) {
    if (!s->asked) {
        return BT_SYNC_IDLE;
    }
    bt_sync_in_t const in = bt_sync_input(s, slot, support, now_ms);
    return bt_sync_decide(&in);
}

void bt_sync_sent(bt_sync_t *s, uint32_t now_ms) {
    if (s->tries < 255) {
        s->tries++;
    }
    s->sent_ms = now_ms;
}

bool bt_sync_say(bt_sync_t *s) {
    bool first = !s->said;
    s->said    = true;
    return first;
}

bool bt_sync_in_flight(bt_sync_t const *s, uint32_t now_ms) {
    return s->tries > 0 && now_ms - s->sent_ms < BT_SYNC_RETRY_MS;
}
