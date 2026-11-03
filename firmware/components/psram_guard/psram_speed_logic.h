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
//   trial  = a 120 MHz boot was armed or is running and has not passed yet
//   failed = the last 120 MHz trial failed, the badge fell back to 80 (cleared when the user chooses a speed again)

#define PSRAM_SPEED_TRIAL_SECONDS 60 // a 120 MHz boot must run this long, with a clean boot check, to count as good

typedef struct {
    uint8_t want;   // 80 or 120
    uint8_t trial;
    uint8_t failed;
    uint8_t pad;
} psram_speed_rec_t;

typedef enum {
    PSRAM_SPEED_ACT_NONE,
    PSRAM_SPEED_ACT_ARM_RESTART, // arm the register and restart once: the 120 MHz boot
    PSRAM_SPEED_ACT_RESTART,     // running 120 MHz against the record: restart (the register is clear, so at 80)
} psram_speed_act_t;

// A record read from NVS that is not ours becomes the default (80, nothing pending)
void psram_speed_rec_sane(psram_speed_rec_t *r);

// At boot, once NVS is up. patched = the IDF can run 120 MHz. trial_live is set when this boot is a 120 MHz trial.
psram_speed_act_t psram_speed_boot_decide(psram_speed_rec_t *r, unsigned running_mhz, bool patched, bool *trial_live);

typedef enum { PSRAM_SPEED_TRIAL_RUNNING, PSRAM_SPEED_TRIAL_PASS, PSRAM_SPEED_TRIAL_FAIL } psram_speed_trial_t;
// Every tick of a live trial: boot_bad = the tuning fell back or the boot check flipped bits, gate_failed = `fw psram dma`
psram_speed_trial_t psram_speed_trial_step(unsigned uptime_s, bool boot_bad, bool gate_failed);

void psram_speed_fail(psram_speed_rec_t *r);                      // back at 80, failed flag set, never re-armed by itself
void psram_speed_pass(psram_speed_rec_t *r);                      // the trial is over, 120 stays wanted
void psram_speed_choose(psram_speed_rec_t *r, unsigned target);   // the user's choice (80 or 120); clears failed

// "80 MHz", "120 MHz", "120 MHz, testing", "80 MHz (120 MHz failed, back at 80)"... for the Settings row and fw psram
size_t psram_speed_value_text(psram_speed_rec_t const *r, unsigned running_mhz, char *out, size_t cap);
