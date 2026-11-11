#include "psram_speed_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void last_sane(psram_fail_t *f) {
    f->fw[PSRAM_FAIL_FW_MAX] = '\0';
    for (char *p = f->fw; *p; p++) {
        if (*p < 0x20 || *p > 0x7e) { // never print bytes that are not text
            *p = '?';
        }
    }
    if (f->kind == PSRAM_FAIL_NONE || f->kind >= PSRAM_FAIL_KINDS || f->at120 > 1) {
        *f = (psram_fail_t){0}; // "none", no left-over facts
    }
    f->pad = 0;
}

void psram_speed_rec_sane(psram_speed_rec_t *r) {
    if ((r->want != 80 && r->want != 120) || r->trial > PSRAM_SPEED_TRIAL_ARMED || r->failed > 1 ||
        r->aborts > PSRAM_SPEED_MAX_ABORTS || r->powers > PSRAM_SPEED_MAX_POWERS) {
        *r = (psram_speed_rec_t){.want = 80};
    }
    last_sane(&r->last);
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void psram_speed_rec_encode(psram_speed_rec_t const *r, uint8_t out[PSRAM_SPEED_REC_SIZE]) {
    memset(out, 0, PSRAM_SPEED_REC_SIZE);
    out[0] = r->want;
    out[1] = r->trial;
    out[2] = r->failed;
    out[3] = r->aborts;
    out[4] = PSRAM_SPEED_REC_VERSION;
    out[5] = r->last.kind;
    out[6] = r->last.reset;
    out[7] = r->last.at120;
    put32(out + 8, r->last.wall_s);
    put32(out + 12, r->last.uptime_s);
    memcpy(out + 16, r->last.fw, strnlen(r->last.fw, PSRAM_FAIL_FW_MAX)); // out[31] stays 0
    out[32] = r->powers;
}

void psram_speed_rec_decode(uint8_t const *in, size_t size, psram_speed_rec_t *r) {
    *r = (psram_speed_rec_t){.want = 80};
    if (size == PSRAM_SPEED_REC_SIZE && in[4] == PSRAM_SPEED_REC_VERSION) {
        *r = (psram_speed_rec_t){.want = in[0], .trial = in[1], .failed = in[2], .aborts = in[3]};
        r->last.kind     = in[5];
        r->last.reset    = in[6];
        r->last.at120    = in[7];
        r->last.wall_s   = get32(in + 8);
        r->last.uptime_s = get32(in + 12);
        memcpy(r->last.fw, in + 16, PSRAM_FAIL_FW_MAX); // [15] stays 0
        r->powers = in[32];
    }
    psram_speed_rec_sane(r);
}

bool psram_speed_arm_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool patched, bool image_pending) {
    return patched && !image_pending && running_mhz != 120 && r->want == 120 && !r->failed && !r->trial;
}

bool psram_speed_leave_120_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool image_pending) {
    return running_mhz == 120 && !image_pending && (r->want != 120 || r->failed);
}

bool psram_speed_arm_commit(psram_speed_rec_t *r, bool (*save)(psram_speed_rec_t const *)) {
    r->trial = PSRAM_SPEED_TRIAL_ARMED; // before arming: a crash in between comes back at 80 still ARMED, no marker
    if (save(r)) {
        return true;
    }
    r->want  = 80; // RAM only: nothing was armed, nothing can follow a record that is not on the flash
    r->trial = 0;
    return false;
}

// A boot at 80 with a trial pending: armed in the record, or the boot before ran at 120 (REACHED) without passing.
// #329: a panic on the way into the trial, still at 80, counted as "120 MHz failed"; the marker tells the two apart.
static bool trial_pending_at_80(psram_speed_rec_t const *r, bool reached) {
    return r->trial == PSRAM_SPEED_TRIAL_ARMED || (reached && r->want == 120 && !r->failed);
}

static psram_speed_act_t judge_at_80(psram_speed_rec_t *r, psram_fail_ctx_t const *ctx) {
    if (ctx->carrier == PSRAM_SPEED_REACHED_WORD) { // it did run at 120 and died before it passed
        psram_speed_fail_at(r, psram_speed_reset_is_crash(ctx->reset) ? PSRAM_FAIL_CRASH : PSRAM_FAIL_RESTART, ctx, true);
        return PSRAM_SPEED_ACT_NONE;
    }
    if (psram_speed_reset_is_crash(ctx->reset)) { // fail-safe until #330 shows STORE15 survives every crash reset
        psram_speed_fail_at(r, PSRAM_FAIL_CRASH_ARMED, ctx, false);
        return PSRAM_SPEED_ACT_NONE;
    }
    if (psram_speed_reset_is_power(ctx->reset)) { // burns no abort, overwrites no failure; only many of them fail
        if (++r->powers >= PSRAM_SPEED_MAX_POWERS) {
            psram_speed_fail_at(r, PSRAM_FAIL_POWERS, ctx, false);
            return PSRAM_SPEED_ACT_NONE;
        }
        return PSRAM_SPEED_ACT_REARM;
    }
    if (++r->aborts >= PSRAM_SPEED_MAX_ABORTS) { // never loop: an arm that keeps dying is given up like a failure
        psram_speed_fail_at(r, PSRAM_FAIL_ABORTS, ctx, false);
        return PSRAM_SPEED_ACT_NONE;
    }
    psram_speed_fail_note(r, PSRAM_FAIL_ABORTED, ctx, false);
    return PSRAM_SPEED_ACT_REARM; // trial stays ARMED; no restart of our own, the next clean one tries 120
}

psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched, bool image_pending,
                                          psram_fail_ctx_t const *ctx, bool *trial_live) {
    *trial_live = false;
    if (!patched) {
        return PSRAM_SPEED_ACT_NONE; // nothing can run 120 MHz: the record keeps what the user asked
    }
    if (running_mhz != 120 && trial_pending_at_80(r, ctx->carrier == PSRAM_SPEED_REACHED_WORD)) {
        return judge_at_80(r, ctx);
    }
    if (running_mhz == 120) {
        if (r->want != 120 || r->failed) { // 120 MHz against the record: get back to 80 (the register is clear)
            psram_speed_fail_at(r, PSRAM_FAIL_AGAINST, ctx, true);
            return image_pending ? PSRAM_SPEED_ACT_NONE : PSRAM_SPEED_ACT_RESTART;
        }
        *trial_live = true; // the record stays as it is: the REACHED marker says this boot runs at 120
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

void psram_speed_fail_note(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx, bool at120) {
    psram_fail_t f = {.kind = (uint8_t)kind, .reset = ctx->reset, .at120 = at120, .uptime_s = ctx->uptime_s};
    f.wall_s       = ctx->wall_s >= PSRAM_FAIL_WALL_MIN ? ctx->wall_s : 0;
    snprintf(f.fw, sizeof(f.fw), "%s", ctx->fw ? ctx->fw : "");
    last_sane(&f);
    r->last = f;
}

void psram_speed_fail_at(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx, bool at120) {
    psram_speed_fail(r);
    psram_speed_fail_note(r, kind, ctx, at120);
}

void psram_speed_pass(psram_speed_rec_t *r) {
    r->trial  = 0;
    r->aborts = 0;
    r->powers = 0;
}

void psram_speed_choose(psram_speed_rec_t *r, unsigned target) { // the last failure stays: it is history
    r->want   = target == 120 ? 120 : 80;
    r->trial  = r->want == 120 ? PSRAM_SPEED_TRIAL_ARMED : 0;
    r->failed = 0;
    r->aborts = 0;
    r->powers = 0;
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

char const *psram_speed_reset_name(uint8_t reset) {
    static char const *const names[] = {"unknown",  "poweron", "ext",  "sw",   "panic", "int_wdt",    "task_wdt",  "wdt",
                                        "deepsleep", "brownout", "sdio", "usb", "jtag",  "efuse",      "pwr_glitch", "cpu_lockup"};
    return reset < sizeof(names) / sizeof(names[0]) ? names[reset] : "?";
}

bool psram_speed_reset_is_crash(uint8_t reset) {
    return reset == 4 || reset == 5 || reset == 6 || reset == 7 || reset == 15; // panic, int/task/other wdt, cpu lockup
}

bool psram_speed_reset_is_power(uint8_t reset) {
    return reset == 1 || reset == 2 || reset == 9; // poweron, ext, brownout
}

char const *psram_speed_fail_kind_text(uint8_t kind) {
    static char const *const texts[PSRAM_FAIL_KINDS] = {"none",         "boot check",
                                                        "DMA gate",     "crash or watchdog",
                                                        "restart before it passed",
                                                        "aborted (ran at 80)", "aborted 3 times, given up",
                                                        "crash or watchdog while armed (no 120 MHz marker)",
                                                        "ran at 120 against the record",
                                                        "power resets while armed"};
    return kind < PSRAM_FAIL_KINDS ? texts[kind] : "?";
}

size_t psram_speed_fail_text(psram_fail_t const *f, char *out, size_t cap) {
    if (f->kind == PSRAM_FAIL_NONE || f->kind >= PSRAM_FAIL_KINDS) {
        return (size_t)snprintf(out, cap, "none");
    }
    char when[32];
    struct tm tm;
    time_t    t = (time_t)f->wall_s;
    if (f->wall_s && gmtime_r(&t, &tm)) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &tm);
    } else {
        snprintf(when, sizeof(when), "up %u s", (unsigned)f->uptime_s);
    }
    return (size_t)snprintf(out, cap, "%s, reset %s, ran at %u, %s, %s", psram_speed_fail_kind_text(f->kind),
                            psram_speed_reset_name(f->reset), f->at120 ? 120u : 80u, when, f->fw[0] ? f->fw : "?");
}
