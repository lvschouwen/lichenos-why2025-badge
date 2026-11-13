#include "ota_valid.h"

// "ARRV": a mark in LP RAM that is not this and its check is noise from a power-on
#define ARRIVAL_MAGIC 0x41525256u

static bool proven(ota_trial_in_t const *in) {
    bool ui_ok = in->display == OTA_DISPLAY_SKIPPED || (in->display == OTA_DISPLAY_UP && in->shell_up);
    return in->uptime_s >= OTA_VALID_AFTER_S && in->c6_up && ui_ok;
}

// #411: an image the Wi-Fi receiver installed is proven only back on Wi-Fi, unless the user switched Wi-Fi off
static ota_trial_act_t wifi_step(ota_trial_in_t const *in) {
    if (proven(in) && in->wifi_joined) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (ota_should_roll_back_now(true, in->c6_up, in->uptime_s)) {
        return OTA_TRIAL_ROLL_BACK; // no C6 link: as before, sooner than the Wi-Fi bound
    }
    if (in->wifi_joined && in->uptime_s >= OTA_VALID_AFTER_S) {
        return OTA_TRIAL_STAY; // UI down, but reachable over Wi-Fi: as before with the C6 up
    }
    if (in->uptime_s >= OTA_VALID_NEEDS_WIFI_S) {
        return OTA_TRIAL_ROLL_BACK_NO_WIFI;
    }
    return OTA_TRIAL_WAIT;
}

ota_trial_act_t ota_trial_step(ota_trial_in_t const *in) {
    if (in->image == OTA_IMAGE_UNKNOWN) {
        return OTA_TRIAL_UNKNOWN;
    }
    if (in->image != OTA_IMAGE_ON_TRIAL) {
        return OTA_TRIAL_DONE;
    }
    if (in->arrival == OTA_ARRIVAL_WIFI && !in->wifi_off) {
        return wifi_step(in);
    }
    if (proven(in)) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (ota_should_roll_back_now(true, in->c6_up, in->uptime_s)) {
        return OTA_TRIAL_ROLL_BACK;
    }
    // The C6 link down is watched up to the rollback (#295)
    if (!in->c6_up || in->uptime_s < OTA_VALID_AFTER_S) {
        return OTA_TRIAL_WAIT;
    }
    return OTA_TRIAL_STAY;
}

bool ota_image_may_be_on_trial(ota_image_state_t state) {
    return state != OTA_IMAGE_NOT_ON_TRIAL;
}

bool ota_update_refused_on_trial(bool stops_apps, ota_image_state_t state) {
    return stops_apps && state == OTA_IMAGE_ON_TRIAL;
}

restart_gate_t restart_gate_decide(bool asked, ota_image_state_t image, bool trial_settled, uint32_t uptime_s,
                                   bool power_off) {
    if (!ota_image_may_be_on_trial(image)) {
        return RESTART_GATE_GO;
    }
    if (asked) {
        return RESTART_GATE_GO_ASKED;
    }
    if (power_off) {
        return RESTART_GATE_GO_POWER_OFF;
    }
    if (trial_settled) {
        return RESTART_GATE_GO_SETTLED;
    }
    if (uptime_s >= OTA_RESTART_GATE_MAX_S) {
        return RESTART_GATE_GO_BOUND;
    }
    return RESTART_GATE_WAIT;
}

char const *restart_gate_name(restart_gate_t gate) {
    switch (gate) {
    case RESTART_GATE_GO_ASKED: return "asked for";
    case RESTART_GATE_GO_SETTLED: return "the trial ended without the valid mark";
    case RESTART_GATE_GO_BOUND: return "waited for the valid mark long enough";
    case RESTART_GATE_GO_POWER_OFF: return "a power off was asked";
    case RESTART_GATE_GO:
    case RESTART_GATE_WAIT:
    default: return "";
    }
}

ota_trial_end_t ota_trial_run(ota_trial_io_t const *io) {
    unsigned read_fails = 0, mark_fails = 0;
    for (;;) {
        ota_trial_in_t in = {0};
        io->read(io->ctx, &in);
        switch (ota_trial_step(&in)) {
        case OTA_TRIAL_DONE:
            return OTA_TRIAL_END_NOT_ON_TRIAL;
        case OTA_TRIAL_UNKNOWN:
            if (++read_fails >= OTA_TRIAL_TRIES) {
                return OTA_TRIAL_END_NO_STATE;
            }
            break;
        case OTA_TRIAL_MARK_VALID:
            if (io->mark_valid(io->ctx)) {
                return OTA_TRIAL_END_VALID;
            }
            if (++mark_fails >= OTA_TRIAL_TRIES) {
                return OTA_TRIAL_END_MARK_FAILED;
            }
            break;
        case OTA_TRIAL_ROLL_BACK:
            return OTA_TRIAL_END_ROLL_BACK;
        case OTA_TRIAL_ROLL_BACK_NO_WIFI:
            return OTA_TRIAL_END_NO_WIFI;
        case OTA_TRIAL_STAY:
            return OTA_TRIAL_END_STAY;
        case OTA_TRIAL_WAIT:
        default:
            break;
        }
        io->wait(io->ctx);
    }
}

bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s) {
    return pending_verify && !c6_up && uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
}

static uint32_t arrival_check(uint32_t slot_addr) {
    return ~(ARRIVAL_MAGIC ^ slot_addr);
}

void ota_arrival_mark(ota_arrival_mark_t *m, uint32_t slot_addr) {
    m->slot_addr = slot_addr;
    m->check     = arrival_check(slot_addr);
    m->magic     = ARRIVAL_MAGIC;
    m->test_no_wifi = m->test_no_wifi == ARRIVAL_MAGIC ? ARRIVAL_MAGIC : 0;
}

void ota_arrival_arm_test(ota_arrival_mark_t *m, bool no_wifi) {
    m->test_no_wifi = no_wifi ? ARRIVAL_MAGIC : 0;
}

ota_arrival_taken_t ota_arrival_take(ota_arrival_mark_t *m, uint32_t running_addr) {
    bool intact = m->magic == ARRIVAL_MAGIC && m->check == arrival_check(m->slot_addr);
    ota_arrival_taken_t t = {.arrival = OTA_ARRIVAL_OTHER};
    if (intact && m->slot_addr == running_addr) {
        t.arrival      = OTA_ARRIVAL_WIFI;
        t.test_no_wifi = m->test_no_wifi == ARRIVAL_MAGIC;
    }
    *m = (ota_arrival_mark_t){0}; // consumed: a later reboot of this image, valid or not, is not a new arrival
    return t;
}

bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id) {
    // magic 0xE9 at 0, chip id little-endian at 12 (esp_app_format.h)
    return head && len >= OTA_IMAGE_HEADER_LEN && head[0] == 0xE9 &&
           (uint16_t)(head[12] | (uint16_t)head[13] << 8) == chip_id;
}
