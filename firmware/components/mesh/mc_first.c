#include "mc_first.h"

mc_first_action_t mc_first_action(mc_first_t const *s) {
    if (s->name_busy) {
        return MC_FIRST_WAIT;
    }
    if (!s->name_asked) {
        return MC_FIRST_ASK_NAME;
    }
    if (!s->name_answered) {
        return MC_FIRST_HOME;
    }
    if (!s->region_asked) {
        return MC_FIRST_ASK_REGION;
    }
    return s->region_answered ? MC_FIRST_FINISH : MC_FIRST_HOME;
}

void mc_first_asked_name(mc_first_t *s) {
    s->name_asked = true;
}

void mc_first_name_submitted(mc_first_t *s) {
    s->name_answered = true;
    s->name_busy     = true;
}

void mc_first_name_result(mc_first_t *s, bool ok) {
    s->name_busy = false;
    if (!ok) {
        s->name_asked = s->name_answered = false;
    }
}

void mc_first_asked_region(mc_first_t *s) {
    s->region_asked = true;
}

void mc_first_region_chosen(mc_first_t *s) {
    s->region_answered = true;
}
