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
