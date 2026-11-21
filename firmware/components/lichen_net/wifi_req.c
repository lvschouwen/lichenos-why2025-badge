#include "wifi_req.h"

void wifi_req_init(wifi_req_t *r) {
    *r = (wifi_req_t){0};
}

uint32_t wifi_req_begin(wifi_req_t *r) {
    r->seq++;
    if (r->seq == 0) { // wrapped: 0 stays "none"
        r->seq = 1;
    }
    r->ok  = false;
    r->why = WIFI_REQ_BUSY;
    return r->seq;
}

bool wifi_req_finish(wifi_req_t *r, uint32_t seq, bool ok, wifi_req_why_t why) {
    if (seq == 0 || seq != r->seq) {
        return false;
    }
    r->ok       = ok;
    r->why      = why;
    r->done_seq = seq;
    return true;
}

bool wifi_req_result(wifi_req_t const *r, uint32_t seq, bool *ok, wifi_req_why_t *why) {
    if (seq == 0 || seq != r->seq || r->done_seq != seq) {
        return false;
    }
    *ok  = r->ok;
    *why = r->why;
    return true;
}

bool wifi_req_wanted(wifi_req_t const *r, uint32_t seq) {
    return seq != 0 && seq == r->seq && r->done_seq != seq;
}

wifi_req_why_t wifi_scan_gate(bool link_up, bool updating, bool radio_on, bool driver_up, wifi_join_state_t join, bool roaming) {
    if (updating) {
        return WIFI_REQ_UPDATING;
    }
    if (!link_up) {
        return WIFI_REQ_NO_LINK;
    }
    if (!radio_on) {
        return WIFI_REQ_OFF;
    }
    if (!driver_up) {
        return WIFI_REQ_DRIVER;
    }
    // A scan hops channels: it would break the association a join or a roam is waiting for
    if (join == WIFI_JOIN_PENDING || join == WIFI_JOIN_TRYING) {
        return WIFI_REQ_JOINING;
    }
    return roaming ? WIFI_REQ_ROAMING : WIFI_REQ_OK;
}

wifi_req_why_t wifi_link_gate(wifi_join_state_t join, bool roaming) {
    if (join == WIFI_JOIN_PENDING || join == WIFI_JOIN_TRYING) {
        return WIFI_REQ_JOINING;
    }
    return roaming ? WIFI_REQ_ROAMING : WIFI_REQ_OK;
}

wifi_req_plan_t wifi_req_plan(wifi_req_t const *r, uint32_t seq, wifi_req_why_t gate) {
    if (!wifi_req_wanted(r, seq)) {
        return (wifi_req_plan_t){0};
    }
    return (wifi_req_plan_t){.run = gate == WIFI_REQ_OK, .answer = true, .why = gate};
}

wifi_req_why_t wifi_apply_gate(bool started, wifi_join_state_t join, bool roaming, bool link) {
    if (!started) {
        return WIFI_REQ_DRIVER;
    }
    return link ? wifi_link_gate(join, roaming) : WIFI_REQ_OK;
}

int wifi_apply_during(bool run, int old_value, int new_value) {
    return run ? new_value : old_value;
}

int wifi_apply_after(bool run, bool ok, int old_value, int new_value) {
    return run && ok ? new_value : old_value;
}

wifi_req_why_t wifi_apply_why(bool run, bool ok, wifi_req_why_t refused) {
    return !run ? refused : ok ? WIFI_REQ_OK : WIFI_REQ_FAILED;
}

char const *wifi_req_why_text(wifi_req_why_t why) {
    switch (why) {
        case WIFI_REQ_OK: return "ok";
        case WIFI_REQ_UPDATING: return "the C6 is being updated";
        case WIFI_REQ_NO_LINK: return "no C6 link";
        case WIFI_REQ_OFF: return "Wi-Fi is switched off";
        case WIFI_REQ_DRIVER: return "the Wi-Fi driver is not running";
        case WIFI_REQ_JOINING: return "a join is in progress, try again in a moment";
        case WIFI_REQ_ROAMING: return "the link is roaming, try again in a moment";
        case WIFI_REQ_BUSY: return "Wi-Fi is busy, try again";
        case WIFI_REQ_FAILED: return "the driver refused it";
        case WIFI_REQ_BAD_ARG: return "not a value it takes";
    }
    return "unknown";
}
