#include "psram_speed_logic.h"

#include <stdio.h>

void psram_speed_rec_sane(psram_speed_rec_t *r) {
    if ((r->want != 80 && r->want != 120) || r->trial > 1 || r->failed > 1) {
        *r = (psram_speed_rec_t){.want = 80};
    }
    r->pad = 0;
}

psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched, bool *trial_live) {
    *trial_live = false;
    if (!patched) {
        return PSRAM_SPEED_ACT_NONE; // nothing can run 120 MHz: the record keeps what the user asked
    }
    if (r->trial && running_mhz != 120) { // armed or running, then a boot at 80: the 120 MHz boot did not get through
        psram_speed_fail(r);
        return PSRAM_SPEED_ACT_NONE;
    }
    if (running_mhz == 120) {
        if (r->want != 120 || r->failed) { // 120 MHz against the record: get back to 80 (the register is clear)
            psram_speed_fail(r);
            return PSRAM_SPEED_ACT_RESTART;
        }
        r->trial   = 1; // a restart at 120 MHz (kept armed on a clean restart) starts a trial of its own
        *trial_live = true;
        return PSRAM_SPEED_ACT_NONE;
    }
    if (r->want == 120 && !r->failed) {
        r->trial = 1; // before arming: a crash in between is counted as a failed trial
        return PSRAM_SPEED_ACT_ARM_RESTART;
    }
    return PSRAM_SPEED_ACT_NONE;
}

psram_speed_trial_t psram_speed_trial_step(unsigned uptime_s, bool boot_bad, bool gate_failed) {
    if (boot_bad || gate_failed) {
        return PSRAM_SPEED_TRIAL_FAIL;
    }
    return uptime_s >= PSRAM_SPEED_TRIAL_SECONDS ? PSRAM_SPEED_TRIAL_PASS : PSRAM_SPEED_TRIAL_RUNNING;
}

void psram_speed_fail(psram_speed_rec_t *r) {
    r->want   = 80;
    r->trial  = 0;
    r->failed = 1;
}

void psram_speed_pass(psram_speed_rec_t *r) {
    r->trial = 0;
}

void psram_speed_choose(psram_speed_rec_t *r, unsigned target) {
    r->want   = target == 120 ? 120 : 80;
    r->trial  = r->want == 120 ? 1 : 0;
    r->failed = 0;
}

size_t psram_speed_value_text(psram_speed_rec_t const *r, unsigned running_mhz, char *out, size_t cap) {
    if (r->failed) {
        return (size_t)snprintf(out, cap, "120 MHz failed, back at 80");
    }
    if (running_mhz == 120) {
        return (size_t)snprintf(out, cap, r->trial ? "120 MHz, testing" : "120 MHz");
    }
    return (size_t)snprintf(out, cap, r->want == 120 ? "80 MHz, switching to 120" : "80 MHz");
}
