#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PSRAM speed switch (#121): 80 MHz (default) or 120 MHz, chosen at PSRAM init by the patched IDF (0003) from an LP
// register that the app arms before a restart; the init clears it, so a crash during a 120 MHz boot comes back at 80.
// Pure state machine, host-tested in host_tests/psram_speed_test.c; psram_speed.c does the NVS, the register and the
// restart.
//
// The NVS record is the truth about what the user asked (want) and how the last 120 MHz boot went:
//   trial  = 0 none; ARMED = a 120 MHz trial was armed and has not passed yet. Nothing is written to flash at 120 MHz
//            before the trial passes: whether a boot reached 120 MHz is the REACHED marker in STORE15 (below), #329
//   failed = the last 120 MHz trial failed, the badge fell back to 80 (cleared when the user chooses a speed again)
//   aborts = armed trials in a row that came back at 80 without having run at 120 (a crash or power loss before the
//            switch took effect): not evidence against 120, so the trial stays armed, up to PSRAM_SPEED_MAX_ABORTS
//   last   = the last failure or abort, for fw psram and the psram.fail_* registry keys

#define PSRAM_SPEED_TRIAL_SECONDS 60 // a 120 MHz boot must run this long, with a clean boot check, to count as good
#define PSRAM_SPEED_TRIAL_ARMED   1

// LP_STORE15 words. ARM selects 120 MHz at the next PSRAM init (patch 0003; the init reads and clears the register and
// keeps the raw word, psram_speed_carrier_raw). REACHED is written by this boot as soon as PSRAM init runs at 120 MHz
// (the tuning wrappers, psram_boot.c): any other word selects 80, so the next boot runs at 80 and finds REACHED = the
// boot before it did run at 120 and ended without a pass, fail or clean re-arm. Cleared on pass and fail.
#define PSRAM_SPEED_ARM_WORD     ((0xC4A1u << 16) | 120u)
#define PSRAM_SPEED_REACHED_WORD ((0xC4A2u << 16) | 120u)
#define PSRAM_SPEED_MAX_ABORTS    3          // the third abort in a row gives up: failed, back at 80 for good
#define PSRAM_FAIL_FW_MAX         15         // image version kept (REG_STR_MAX): "fw-605935b1", a longer one is cut
#define PSRAM_FAIL_WALL_MIN       1735689600u // 2025-01-01 UTC: a clock before it is not set, the record keeps 0

typedef enum {
    PSRAM_FAIL_NONE,
    PSRAM_FAIL_BOOT_CHECK, // the 120 MHz boot's tuning fell back or its boot check flipped bits
    PSRAM_FAIL_DMA_GATE,   // the DMA gate failed at 120 MHz
    PSRAM_FAIL_CRASH,      // the 120 MHz boot ended in a panic or watchdog before it passed
    PSRAM_FAIL_RESTART,    // the 120 MHz boot ended otherwise (restart, power, brownout) before it passed
    PSRAM_FAIL_ABORTED,    // armed, but the boot after it ran at 80: the switch never took effect; still armed
    PSRAM_FAIL_ABORTS,     // the PSRAM_SPEED_MAX_ABORTS-th abort in a row: given up, failed
    PSRAM_FAIL_CRASH_ARMED, // armed, no REACHED marker, but a panic / watchdog reset: failed, fail-safe, as the marker
                            // may not survive every reset (#330)
    PSRAM_FAIL_AGAINST,    // a boot ran at 120 MHz although the record says 80 or failed: back to 80
    PSRAM_FAIL_KINDS
} psram_fail_kind_t;

typedef struct {
    uint8_t  kind;     // psram_fail_kind_t; NONE = no failure recorded (all else 0)
    uint8_t  reset;    // esp_reset_reason_t of the boot that judged it
    uint8_t  at120;    // the failed boot did run at 120 MHz
    uint8_t  pad;
    uint32_t wall_s;   // UTC seconds at the judgement, 0 when the clock was not set
    uint32_t uptime_s; // uptime of the judging boot at the judgement
    char     fw[PSRAM_FAIL_FW_MAX + 1]; // image version of the judging boot, NUL-terminated
} psram_fail_t;

typedef struct {
    uint8_t      want; // 80 or 120
    uint8_t      trial;
    uint8_t      failed;
    uint8_t      aborts;
    psram_fail_t last;
} psram_speed_rec_t;

// The facts of a judgement, filled by the glue (psram_speed.c)
typedef struct {
    uint32_t    carrier;  // psram_speed_carrier_raw(): the STORE15 word PSRAM init found this boot
    uint8_t     reset;    // esp_reset_reason() of this boot
    uint32_t    wall_s;   // time(NULL), any value (below PSRAM_FAIL_WALL_MIN counts as not set)
    uint32_t    uptime_s;
    char const *fw;       // esp_app_get_description()->version
} psram_fail_ctx_t;

// NVS blob "psram_spd". v1 (4 bytes): want, trial, failed, pad. v2 (32 bytes): want, trial, failed, aborts, version 2,
// kind, reset, at120, wall_s (LE32), uptime_s (LE32), fw[16] (NUL-padded). A v1 blob reads with no failure recorded;
// any other size or version is not ours (the default record).
#define PSRAM_SPEED_REC_V1_SIZE 4
#define PSRAM_SPEED_REC_VERSION 2
#define PSRAM_SPEED_REC_SIZE    32
void psram_speed_rec_encode(psram_speed_rec_t const *r, uint8_t out[PSRAM_SPEED_REC_SIZE]);
void psram_speed_rec_decode(uint8_t const *in, size_t size, psram_speed_rec_t *r); // then psram_speed_rec_sane

typedef enum {
    PSRAM_SPEED_ACT_NONE,
    PSRAM_SPEED_ACT_ARM_RESTART, // arm the register and restart once: the 120 MHz boot
    PSRAM_SPEED_ACT_RESTART,     // running 120 MHz against the record: restart (the register is clear, so at 80)
    PSRAM_SPEED_ACT_REARM,       // an aborted trial stays armed: the next clean restart arms the register, nothing now
} psram_speed_act_t;

// A record read from NVS that is not ours becomes the default (80, nothing pending)
void psram_speed_rec_sane(psram_speed_rec_t *r);

// A restart of an image that is still PENDING_VERIFY makes the bootloader roll it back (a good OTA lost), so nothing here
// restarts while pending: the arm waits until the image is valid, a fail only records itself.
// 120 MHz is wanted, not failed, nothing pending, and this boot is at 80: arm and restart once
bool psram_speed_arm_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool patched, bool image_pending);
// Running 120 MHz although the record says 80 or failed (a fail while the image was pending), and a restart is now safe
bool psram_speed_leave_120_wanted(psram_speed_rec_t const *r, unsigned running_mhz, bool image_pending);
// The record is saved before anything is armed. save false (NVS refused): nothing may be armed, and the badge treats
// itself as wanting 80 (RAM only). Sets trial ARMED; returns true when saved and the caller may arm.
bool psram_speed_arm_commit(psram_speed_rec_t *r, bool (*save)(psram_speed_rec_t const *));

// At boot, once NVS is up. patched = the IDF can run 120 MHz. trial_live is set when this boot is a 120 MHz trial.
// A trial pending at a boot at 80 is judged here and recorded in r->last (ctx->carrier is the evidence):
//   REACHED found         -> failed (crash or restart by the reset reason), ran at 120
//   ARMED, panic/watchdog -> failed (fail-safe: no marker, but STORE15 may not survive that reset, #330)
//   ARMED, power-on / ext / brownout -> still armed (REARM), not counted: a power cycle is no evidence at all
//   ARMED, any other reset -> aborted, still armed (REARM); the PSRAM_SPEED_MAX_ABORTS-th in a row = failed
// At 120 the record is not changed (no flash write at an unproven speed) unless it runs against the record.
psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched, bool image_pending,
                                          psram_fail_ctx_t const *ctx, bool *trial_live);

// "fw psram speed [80|120] [yes]": no arguments = status; a strict parse (whole tokens, digits only, "yes" exactly)
typedef enum { PSRAM_SPEED_CMD_STATUS, PSRAM_SPEED_CMD_INVALID, PSRAM_SPEED_CMD_ASK, PSRAM_SPEED_CMD_DO } psram_speed_cmd_t;
psram_speed_cmd_t psram_speed_parse(char const *args, unsigned *target);

typedef enum { PSRAM_SPEED_TRIAL_RUNNING, PSRAM_SPEED_TRIAL_PASS, PSRAM_SPEED_TRIAL_FAIL } psram_speed_trial_t;
// Every tick of a live trial: boot_bad = the tuning fell back or the boot check flipped bits, gate_failed = `fw psram dma`
psram_speed_trial_t psram_speed_trial_step(unsigned uptime_s, bool boot_bad, bool gate_failed);

void psram_speed_fail(psram_speed_rec_t *r);                      // back at 80, failed flag set, never re-armed by itself
// A judged failure or abort into r->last (the facts of ctx, fw cut to PSRAM_FAIL_FW_MAX)
void psram_speed_fail_note(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx, bool at120);
// psram_speed_fail plus the note: a 120 MHz trial failed
void psram_speed_fail_at(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx, bool at120);
void psram_speed_pass(psram_speed_rec_t *r);                      // the trial is over, 120 stays wanted
void psram_speed_choose(psram_speed_rec_t *r, unsigned target);   // the user's choice (80 or 120); clears failed

// "80 MHz", "120 MHz", "120 MHz, testing", "80 MHz (120 MHz failed, back at 80)"... for the Settings row and fw psram
size_t psram_speed_value_text(psram_speed_rec_t const *r, unsigned running_mhz, char *out, size_t cap);

// esp_reset_reason_t by number (esp_system.h order, pinned in psram_speed.c): "panic", "int_wdt", "sw"...; "?" past it
char const *psram_speed_reset_name(uint8_t reset);
// A panic or watchdog reset (panic, int_wdt, task_wdt, wdt, cpu_lockup)
bool psram_speed_reset_is_crash(uint8_t reset);
// A power-on, external pin or brownout reset: the power went, nothing ran
bool psram_speed_reset_is_power(uint8_t reset);
// "boot check", "DMA gate", "crash or watchdog", "restart before it passed", "aborted (ran at 80)", "aborted 3 times"...
char const *psram_speed_fail_kind_text(uint8_t kind);
// "none", or "<kind>, reset <reason>, ran at <80|120>, <YYYY-MM-DD HH:MM:SS UTC|up N s>, <fw>"
size_t psram_speed_fail_text(psram_fail_t const *f, char *out, size_t cap);
