#include "psram_speed_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool kind_known(uint8_t kind) {
    return kind == PSRAM_FAIL_BOOT_CHECK || kind == PSRAM_FAIL_DMA_GATE || kind == PSRAM_FAIL_CANARY ||
           kind == PSRAM_FAIL_HEAP;
}

static void last_sane(psram_fail_t *f) {
    f->fw[PSRAM_FAIL_FW_MAX] = '\0';
    for (char *p = f->fw; *p; p++) {
        if (*p < 0x20 || *p > 0x7e) { // never print bytes that are not text
            *p = '?';
        }
    }
    if (!kind_known(f->kind) || f->at120 > 1) {
        *f = (psram_fail_t){0}; // "none", no left-over facts
    }
    f->pad = 0;
}

psram_speed_rec_t psram_speed_rec_default(void) {
    return (psram_speed_rec_t){.want = 120};
}

void psram_speed_rec_sane(psram_speed_rec_t *r) {
    if ((r->want != 80 && r->want != 120) || r->failed > 1 || r->arms > PSRAM_SPEED_MAX_ARMS) {
        *r = psram_speed_rec_default();
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
    out[1] = r->failed;
    out[2] = r->arms;
    out[4] = PSRAM_SPEED_REC_VERSION;
    out[5] = r->last.kind;
    out[6] = r->last.reset;
    out[7] = r->last.at120;
    put32(out + 8, r->last.wall_s);
    put32(out + 12, r->last.uptime_s);
    memcpy(out + 16, r->last.fw, strnlen(r->last.fw, PSRAM_FAIL_FW_MAX)); // out[31] stays 0
}

#define V3_SIZE 36

void psram_speed_rec_decode(uint8_t const *in, size_t size, psram_speed_rec_t *r) {
    *r = psram_speed_rec_default();
    if (size == V3_SIZE && in[4] == 3) { // the trial's record: only a choice of 80 without a fall survives
        r->want = in[0] == 80 && in[2] == 0 ? 80 : 120;
        return;
    }
    if (size == PSRAM_SPEED_REC_SIZE && in[4] == PSRAM_SPEED_REC_VERSION) {
        *r = (psram_speed_rec_t){.want = in[0], .failed = in[1], .arms = in[2]};
        r->last.kind     = in[5];
        r->last.reset    = in[6];
        r->last.at120    = in[7];
        r->last.wall_s   = get32(in + 8);
        r->last.uptime_s = get32(in + 12);
        memcpy(r->last.fw, in + 16, PSRAM_FAIL_FW_MAX); // [15] stays 0
    }
    psram_speed_rec_sane(r);
}

static bool wants_120(psram_speed_rec_t const *r) {
    return r->want == 120 && !r->failed;
}

bool psram_speed_leave_120_wanted(psram_speed_rec_t const *r, unsigned running_mhz) {
    return running_mhz == 120 && !wants_120(r);
}

bool psram_speed_settle_wanted(psram_speed_rec_t const *r, unsigned running_mhz, unsigned uptime_s) {
    return running_mhz == 120 && r->arms && uptime_s >= PSRAM_SPEED_SETTLE_SECONDS;
}

bool psram_speed_rearm_now(bool waiting, bool crash_told, bool image_pending) {
    return waiting && crash_told && !image_pending;
}

bool psram_speed_arm_commit(psram_speed_rec_t *r, bool (*save)(psram_speed_rec_t const *)) {
    r->arms++; // before arming: a 120 MHz boot that never settles counts, so an arm loop ends at the cap
    if (save(r)) {
        return true;
    }
    r->arms--;
    r->want = 80; // RAM only: nothing was armed, nothing can follow a record that is not on the flash
    return false;
}

psram_speed_act_t psram_speed_evidence(psram_speed_rec_t *r, unsigned running_mhz, psram_fail_kind_t kind,
                                       psram_fail_ctx_t const *ctx) {
    if (running_mhz != 120 || !wants_120(r) || !kind_known((uint8_t)kind)) {
        return PSRAM_SPEED_ACT_NONE; // at 80 there is nothing to fall to; a fall already recorded stays the first
    }
    r->want   = 80;
    r->failed = 1;
    r->arms   = 0;
    psram_speed_fail_note(r, kind, ctx);
    return PSRAM_SPEED_ACT_FALL80;
}

// 120 wanted at a boot at 80: an arm restart, unless this session is better left at 80 until a clean restart. After a
// crash (Lucas, #353): a settled session (arms 0) re-arms once the crash is recorded; the re-arm is counted as an arm,
// so a crash before the next one settles keeps that session at 80
static psram_speed_act_t arm_at_80(psram_speed_rec_t *r, psram_fail_ctx_t const *ctx) {
    if (psram_speed_reset_crashed(ctx->reset, ctx->fresh_dump)) { // no evidence against 120 either way
        return r->arms == 0 ? PSRAM_SPEED_ACT_REARM_AFTER_CRASH : PSRAM_SPEED_ACT_KEEP120;
    }
    if (r->arms >= PSRAM_SPEED_MAX_ARMS) {
        r->arms = 0; // the next restart tries again; this one stays at 80
        return PSRAM_SPEED_ACT_KEEP120;
    }
    return PSRAM_SPEED_ACT_ARM_RESTART;
}

psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched,
                                          psram_fail_kind_t evidence, psram_fail_ctx_t const *ctx) {
    if (!patched) {
        return PSRAM_SPEED_ACT_NONE; // nothing can run 120 MHz: the record keeps what the user asked
    }
    if (running_mhz == 120) {
        if (!wants_120(r)) {
            return PSRAM_SPEED_ACT_LEAVE120;
        }
        return evidence != PSRAM_FAIL_NONE ? psram_speed_evidence(r, running_mhz, evidence, ctx) : PSRAM_SPEED_ACT_NONE;
    }
    if (!wants_120(r)) {
        return PSRAM_SPEED_ACT_NONE;
    }
    if (kind_known(ctx->carried)) { // the last boot fell at 120 but could not save it: record it, never arm into it
        psram_speed_evidence(r, 120, (psram_fail_kind_t)ctx->carried, ctx);
        return PSRAM_SPEED_ACT_NONE;
    }
    return arm_at_80(r, ctx);
}

uint32_t psram_speed_fall_word(psram_fail_kind_t kind) {
    return (PSRAM_SPEED_FALL_TAG << 16) | (uint32_t)kind;
}

psram_fail_kind_t psram_speed_carried_fall(uint32_t carrier) {
    uint8_t kind = (uint8_t)(carrier & 0xffu);
    return (carrier >> 16) == PSRAM_SPEED_FALL_TAG && (carrier & 0xff00u) == 0 && kind_known(kind) ? (psram_fail_kind_t)kind
                                                                                                    : PSRAM_FAIL_NONE;
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

void psram_speed_fail_note(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx) {
    psram_fail_t f = {.kind = (uint8_t)kind, .reset = ctx->reset, .at120 = 1, .uptime_s = ctx->uptime_s};
    f.wall_s       = ctx->wall_s >= PSRAM_FAIL_WALL_MIN ? ctx->wall_s : 0;
    snprintf(f.fw, sizeof(f.fw), "%s", ctx->fw ? ctx->fw : "");
    last_sane(&f);
    r->last = f;
}

void psram_speed_choose(psram_speed_rec_t *r, unsigned target) { // the last failure stays: it is history
    r->want   = target == 120 ? 120 : 80;
    r->failed = 0;
    r->arms   = 0;
}

size_t psram_speed_value_text(psram_speed_rec_t const *r, unsigned running_mhz, char *out, size_t cap) {
    if (r->failed) {
        return (size_t)snprintf(out, cap, "120 MHz failed, back at 80");
    }
    if (running_mhz == 120) {
        return (size_t)snprintf(out, cap, "120 MHz");
    }
    return (size_t)snprintf(out, cap, r->want == 120 ? "80 MHz, switching to 120" : "80 MHz");
}

char const *psram_speed_reset_name(uint8_t reset) {
    static char const *const names[] = {"unknown",  "poweron", "ext",  "sw",   "panic", "int_wdt",    "task_wdt",  "wdt",
                                        "deepsleep", "brownout", "sdio", "usb", "jtag",  "efuse",      "pwr_glitch", "cpu_lockup"};
    return reset < sizeof(names) / sizeof(names[0]) ? names[reset] : "?";
}

bool psram_speed_reset_crashed(uint8_t reset, bool fresh_dump) {
    if (reset == 7) { // ESP_RST_WDT: also the board's power-on and CH340 reset (crash.c crash_is_crash)
        return fresh_dump;
    }
    return reset == 4 || reset == 5 || reset == 6 || reset == 15; // panic, int / task wdt, cpu lockup
}

char const *psram_speed_fail_kind_text(uint8_t kind) {
    switch (kind) {
    case PSRAM_FAIL_BOOT_CHECK: return "boot check";
    case PSRAM_FAIL_DMA_GATE: return "DMA gate";
    case PSRAM_FAIL_CANARY: return "canary flip";
    case PSRAM_FAIL_HEAP: return "corrupt PSRAM heap";
    default: return "?";
    }
}

size_t psram_speed_fail_text(psram_fail_t const *f, char *out, size_t cap) {
    if (!kind_known(f->kind)) {
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
