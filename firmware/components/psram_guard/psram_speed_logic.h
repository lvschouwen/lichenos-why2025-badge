#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PSRAM speed switch (#121, #353): 80 or 120 MHz, chosen at PSRAM init by the patched IDF (0003) from an LP register
// that the app arms before a restart; the init clears it, so every boot that should run at 120 needs the word armed
// again (the shutdown hook on a clean restart, or an arm and one restart from a boot at 80). Pure state machine,
// host-tested in host_tests/psram_speed_test.c; psram_speed.c does the NVS, the register and the restart.
//
// 120 MHz is the default (#353): no trial. The badge falls back to 80 only on PSRAM evidence (the boot check's bad bytes,
// a canary flip, a corrupt PSRAM heap, a failed DMA gate round) seen while it runs at 120; a restart, an OTA reboot, a
// panic or a watchdog proves nothing about the PSRAM and keeps 120. After a fall it stays at 80 until the person picks
// a speed again (Settings, fw psram speed), which boots straight to that speed.
//
// The NVS record:
//   want   = 80 or 120, what the user asked (120 when nothing was ever saved)
//   failed = PSRAM evidence at 120 put the badge back at 80 (cleared when the user chooses a speed again)
//   arms   = arm restarts in a row from a boot at 80 that never settled at 120 (PSRAM_SPEED_SETTLE_SECONDS of uptime):
//            only bounds a loop (a brownout at 120, a restart before the shutdown hook is in), never a failure
//   last   = the last fall to 80, for fw psram and the psram.fail_* registry keys

#define PSRAM_SPEED_SETTLE_SECONDS 60 // a 120 MHz boot up this long clears arms (one NVS write, only when arms is set)
#define PSRAM_SPEED_MAX_ARMS       3  // the third unsettled arm in a row: this session stays at 80, arms starts over

// LP_STORE15 word that selects 120 MHz at the next PSRAM init (patch 0003; the init reads and clears the register and
// keeps the raw word, psram_speed_carrier_raw). Any other word selects 80.
#define PSRAM_SPEED_ARM_WORD ((0xC4A1u << 16) | 120u)
#define PSRAM_SPEED_FALL_TAG 0xC4A3u // high half of a carried fall: (tag << 16) | kind, see psram_speed_fall_word
#define PSRAM_FAIL_FW_MAX    15          // image version kept (REG_STR_MAX): "fw-605935b1", a longer one is cut
#define PSRAM_FAIL_WALL_MIN  1735689600u // 2025-01-01 UTC: a clock before it is not set, the record keeps 0

// The numbers are the psram.fail_why registry enum (#316): stable, so the trial's kinds (#329) keep theirs, retired
typedef enum {
    PSRAM_FAIL_NONE,
    PSRAM_FAIL_BOOT_CHECK, // the boot check at 120 MHz found bad bytes (single-bit words included)
    PSRAM_FAIL_DMA_GATE,   // a DMA gate round at 120 MHz failed (fw psram dma, the Bench, the stress run)
    PSRAM_FAIL_RETIRED_3,  // 3..9: the per-boot trial's kinds (crash, restart, aborts, power resets...), never recorded
    PSRAM_FAIL_RETIRED_4,  //       since #353
    PSRAM_FAIL_RETIRED_5,
    PSRAM_FAIL_RETIRED_6,
    PSRAM_FAIL_RETIRED_7,
    PSRAM_FAIL_RETIRED_8,
    PSRAM_FAIL_RETIRED_9,
    PSRAM_FAIL_CANARY,     // a PSRAM canary block flipped at 120 MHz
    PSRAM_FAIL_HEAP,       // the PSRAM heap failed its integrity check at 120 MHz (CORRUPT HEAP)
    PSRAM_FAIL_KINDS
} psram_fail_kind_t;

typedef struct {
    uint8_t  kind;     // psram_fail_kind_t; NONE = no failure recorded (all else 0)
    uint8_t  reset;    // esp_reset_reason_t of the boot that saw it
    uint8_t  at120;    // that boot ran at 120 MHz (always, since #353; kept for psram.fail_at120)
    uint8_t  pad;
    uint32_t wall_s;   // UTC seconds at the judgement, 0 when the clock was not set
    uint32_t uptime_s; // uptime of that boot at the judgement
    char     fw[PSRAM_FAIL_FW_MAX + 1]; // image version of that boot, NUL-terminated
} psram_fail_t;

typedef struct {
    uint8_t      want; // 80 or 120
    uint8_t      failed;
    uint8_t      arms;
    psram_fail_t last;
} psram_speed_rec_t;

// The facts of a judgement, filled by the glue (psram_speed.c)
typedef struct {
    uint8_t     reset;      // esp_reset_reason() of this boot
    bool        fresh_dump; // the core dump is new since the last recorded crash (crash_dump_fresh)
    uint8_t     carried;    // psram_fail_kind_t of a fall the last boot could not save (STORE15), NONE when none
    uint32_t    wall_s;   // time(NULL), any value (below PSRAM_FAIL_WALL_MIN counts as not set)
    uint32_t    uptime_s;
    char const *fw;       // esp_app_get_description()->version
} psram_fail_ctx_t;

// NVS blob "psram_spd", version 4 (32 bytes, #353): want, failed, arms, 0, version 4, kind, reset, at120, wall_s (LE32),
// uptime_s (LE32), fw[16] (NUL-padded). The trial's 36-byte v3 record of #329 (want, trial, failed, aborts, version 3,
// ...) keeps only a choice of 80 without a fall; its falls were no PSRAM evidence, so anything else in it reads as 120.
// Any other size or version is not ours: the default record (120, nothing failed).
#define PSRAM_SPEED_REC_VERSION 4
#define PSRAM_SPEED_REC_SIZE    32
void psram_speed_rec_encode(psram_speed_rec_t const *r, uint8_t out[PSRAM_SPEED_REC_SIZE]);
void psram_speed_rec_decode(uint8_t const *in, size_t size, psram_speed_rec_t *r); // then psram_speed_rec_sane

typedef enum {
    PSRAM_SPEED_ACT_NONE,
    PSRAM_SPEED_ACT_ARM_RESTART, // 120 wanted at a boot at 80: count the arm, save, arm the register, restart once
    PSRAM_SPEED_ACT_KEEP120,     // 120 wanted, but this session stays at 80 (after a crash, or the arm cap): the next
                                 // clean restart arms the register (shutdown hook), nothing now
    PSRAM_SPEED_ACT_LEAVE120,    // running 120 against the record (want 80 or failed): restart, the register clear
    PSRAM_SPEED_ACT_FALL80,      // PSRAM evidence at 120: failed is recorded, restart at 80
} psram_speed_act_t;

// A record read from NVS that is not ours becomes the default (120, nothing failed)
void psram_speed_rec_sane(psram_speed_rec_t *r);
// The default record: what a badge with nothing saved runs
psram_speed_rec_t psram_speed_rec_default(void);

// A restart of an image that is still PENDING_VERIFY makes the bootloader roll it back (a good OTA lost), so the glue
// defers every restart decided here while pending (an arm, a leave, a fall): the record is saved, the restart follows
// once the image is valid.
//
// At boot, once NVS is up. patched = the IDF can run 120 MHz. evidence = what this boot's boot check found at 120
// (PSRAM_FAIL_BOOT_CHECK or NONE); ctx = the facts for the record. Any reset reason is no evidence:
//   running 120, record agrees, no evidence -> NONE (the shutdown hook keeps 120 through clean restarts)
//   running 120, evidence                   -> FALL80, failed recorded with the kind
//   running 120, want 80 or failed          -> LEAVE120 (no note: the record already says 80)
//   running 80, a fall carried in STORE15   -> NONE, the fall recorded now (its boot could not save it)
//   running 80, want 120, not failed        -> ARM_RESTART; KEEP120 after a crash (psram_speed_reset_crashed: no crash
//                                              loop through an arm) or at the PSRAM_SPEED_MAX_ARMS-th unsettled arm
//                                              (arms back to 0)
//   anything else                           -> NONE
psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched,
                                          psram_fail_kind_t evidence, psram_fail_ctx_t const *ctx);
// PSRAM evidence later in a boot (a canary, the heap check, a DMA gate round): FALL80 at 120 with the record changed, NONE
// at 80 (nothing to fall to) or when it already fell
psram_speed_act_t psram_speed_evidence(psram_speed_rec_t *r, unsigned running_mhz, psram_fail_kind_t kind,
                                       psram_fail_ctx_t const *ctx);
// Before an arm restart: arms + 1, saved; save false (NVS refused): nothing may be armed, the badge treats itself as
// wanting 80 this session (RAM only). True when saved and the caller may arm.
bool psram_speed_arm_commit(psram_speed_rec_t *r, bool (*save)(psram_speed_rec_t const *));
// A 120 MHz boot up PSRAM_SPEED_SETTLE_SECONDS with arms counted: clear them (the caller saves)
bool psram_speed_settle_wanted(psram_speed_rec_t const *r, unsigned running_mhz, unsigned uptime_s);
// Running 120 against the record (want 80 or failed): a restart is due once the image is valid
bool psram_speed_leave_120_wanted(psram_speed_rec_t const *r, unsigned running_mhz);

// "fw psram speed [80|120] [yes]": no arguments = status; a strict parse (whole tokens, digits only, "yes" exactly)
typedef enum { PSRAM_SPEED_CMD_STATUS, PSRAM_SPEED_CMD_INVALID, PSRAM_SPEED_CMD_ASK, PSRAM_SPEED_CMD_DO } psram_speed_cmd_t;
psram_speed_cmd_t psram_speed_parse(char const *args, unsigned *target);

void psram_speed_fail_note(psram_speed_rec_t *r, psram_fail_kind_t kind, psram_fail_ctx_t const *ctx); // into r->last
void psram_speed_choose(psram_speed_rec_t *r, unsigned target);   // the user's choice (80 or 120); clears failed, arms

// "80 MHz", "120 MHz", "80 MHz, switching to 120", "120 MHz failed, back at 80" for the Settings row and fw psram
size_t psram_speed_value_text(psram_speed_rec_t const *r, unsigned running_mhz, char *out, size_t cap);

// esp_reset_reason_t by number (esp_system.h order, pinned in psram_speed.c): "panic", "int_wdt", "sw"...; "?" past it
char const *psram_speed_reset_name(uint8_t reset);
// A crash, judged as crash.c does: panic, int_wdt, task_wdt and cpu_lockup always; wdt (7) only with a fresh core dump,
// because every power-on and USB-serial reset on this board reports reason 7 too
bool psram_speed_reset_crashed(uint8_t reset, bool fresh_dump);
// The STORE15 word that carries an unsaved fall to the next boot (selects 80 there: it is not the ARM word), and back
uint32_t          psram_speed_fall_word(psram_fail_kind_t kind);
psram_fail_kind_t psram_speed_carried_fall(uint32_t carrier);
// "boot check", "DMA gate", "canary flip", "corrupt PSRAM heap"; "?" for none of these
char const *psram_speed_fail_kind_text(uint8_t kind);
// "none", or "<kind>, reset <reason>, ran at <80|120>, <YYYY-MM-DD HH:MM:SS UTC|up N s>, <fw>"
size_t psram_speed_fail_text(psram_fail_t const *f, char *out, size_t cap);
