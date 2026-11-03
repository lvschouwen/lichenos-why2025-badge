#include "psram_speed_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void psram_speed_rec_sane(psram_speed_rec_t *r) {
    if ((r->want != 80 && r->want != 120) || r->trial > 1 || r->failed > 1) {
        *r = (psram_speed_rec_t){.want = 80};
    }
    r->pad = 0;
}

bool psram_speed_arm_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool patched, bool image_pending) {
    return patched && !image_pending && running_mhz != 120 && r->want == 120 && !r->failed && !r->trial;
}

bool psram_speed_leave_120_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool image_pending) {
    return running_mhz == 120 && !image_pending && (r->want != 120 || r->failed);
}

bool psram_speed_arm_commit(psram_speed_rec_t *r, bool (*save)(psram_speed_rec_t const *)) {
    r->trial = 1; // before arming: a crash in between is counted as a failed trial
    if (save(r)) {
        return true;
    }
    r->want  = 80; // RAM only: nothing was armed, nothing can follow a record that is not on the flash
    r->trial = 0;
    return false;
}

psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched, bool image_pending,
                                          bool *trial_live) {
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
            return image_pending ? PSRAM_SPEED_ACT_NONE : PSRAM_SPEED_ACT_RESTART;
        }
        r->trial   = 1; // a restart at 120 MHz after a passed trial starts a trial of its own
        *trial_live = true;
        return PSRAM_SPEED_ACT_NONE;
    }
    // The arm itself (trial set, saved, register, restart) is the glue's, here or from the tick once the image is valid
    return psram_speed_arm_wanted(r, running_mhz, patched, image_pending) ? PSRAM_SPEED_ACT_ARM_RESTART : PSRAM_SPEED_ACT_NONE;
}

psram_speed_cmd_t psram_speed_parse(char const *args, unsigned *target) {
    char buf[32];
    if (strlen(args) >= sizeof(buf)) {
        return PSRAM_SPEED_CMD_INVALID; // never judge a truncated line
    }
    snprintf(buf, sizeof(buf), "%s", args);
    char *save = NULL, *tok = strtok_r(buf, " \t\r\n", &save);
    if (!tok) {
        return PSRAM_SPEED_CMD_STATUS;
    }
    for (char const *p = tok; *p; p++) {
        if (*p < '0' || *p > '9') {
            return PSRAM_SPEED_CMD_INVALID;
        }
    }
    unsigned long v = strtoul(tok, NULL, 10);
    if (strlen(tok) > 3 || (v != 80 && v != 120)) {
        return PSRAM_SPEED_CMD_INVALID;
    }
    *target = (unsigned)v;
    tok     = strtok_r(NULL, " \t\r\n", &save);
    if (!tok) {
        return PSRAM_SPEED_CMD_ASK;
    }
    if (strcmp(tok, "yes") != 0 || strtok_r(NULL, " \t\r\n", &save)) {
        return PSRAM_SPEED_CMD_INVALID;
    }
    return PSRAM_SPEED_CMD_DO;
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
