// The radio's one owner and the rate limit on its refusals (#179); see lora_owner.h
#include "lora_owner.h"

#include <stddef.h>
#include <string.h>

bool lora_owner_claim(lora_owner_t *o, char const *who) {
    if (!who || (o->name && strcmp(o->name, who) != 0)) {
        return false;
    }
    o->name = who;
    return true;
}

bool lora_owner_release(lora_owner_t *o, char const *who) {
    if (!lora_owner_is(o, who)) {
        return false;
    }
    o->name = NULL;
    return true;
}

bool lora_owner_is(lora_owner_t const *o, char const *who) {
    return who && o->name && strcmp(o->name, who) == 0;
}

bool lora_refusal_due(lora_refusal_log_t *r, uint32_t now_ms, uint32_t *skipped) {
    if (r->logged && now_ms - r->last_ms < LORA_REFUSAL_LOG_MS) {
        r->skipped++;
        return false;
    }
    *skipped   = r->skipped;
    r->logged  = true;
    r->last_ms = now_ms;
    r->skipped = 0;
    return true;
}
