#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PSRAM bus tuning the IDF picks on every boot (#40), and what to do when a boot looks bad. Pure logic, host-tested in
// host_tests/psram_tuning_test.c; psram_guard.c fills it from the wrapped IDF selection functions and the MSPI registers.
//
// IDF 6.1 (P4, DQS tuning): 4 DQS phases are tried and the first passing one kept; then 31 delayline settings (idx 0..30)
// and the middle of the longest passing run is kept: idx = end - len / 2. idx 15 = no delay; below it the DQS line is
// delayed (dqs = 15 - idx, data 0), above it the data lines (data = idx - 15, dqs 0). A run of 1 or less falls back to idx 0.

#define PSRAM_PHASES     4
#define PSRAM_DELAYLINES 31
#define PSRAM_IDX_ZERO   15 // delayline idx without any delay

typedef struct {
    bool    phase_seen; // the wrapped selection ran
    bool    dl_seen;
    uint8_t phase_len; // consecutive passing phases (0 = none, fell back)
    uint8_t phase_id;  // chosen phase 0..3
    uint8_t dl_len;    // consecutive passing delayline settings (<= 1 = fell back)
    uint8_t dl_end;    // last idx of that run
    uint8_t dl_idx;    // chosen idx
} psram_tuning_t;

// Called from the wrapped IDF functions with their arguments (len = consecutive_length, end = last passing index)
void psram_tuning_note_phase(psram_tuning_t *t, uint32_t len, uint32_t end);
void psram_tuning_note_delayline(psram_tuning_t *t, uint32_t len, uint32_t end);

// idx -> the delay on the data lines and on the DQS line
void psram_tuning_delays(uint8_t idx, uint8_t *data, uint8_t *dqs);

// Phase in hundredths of a degree (6750 = 67.5)
unsigned psram_tuning_phase_centi(uint8_t phase_id);

bool psram_tuning_fell_back(psram_tuning_t const *t); // phase run 0 or delayline run <= 1 (or never ran)

// First idx of the passing run (valid when dl_len >= 1)
uint8_t psram_tuning_dl_start(psram_tuning_t const *t);

// "phase 90 (run 3/4), delayline idx 17 (data 2, dqs 0), window 9..25 len 17" (+ " FELL BACK" when it did)
size_t psram_tuning_format(char *out, size_t cap, psram_tuning_t const *t);

// A boot is bad when tuning fell back or the dense check flipped bits. The window length is only logged until the bench
// data says what a short one is (PSRAM_WINDOW_MIN 0 = off). Writes a short reason for the restart log; true = bad.
#define PSRAM_WINDOW_MIN 0
bool psram_boot_verdict(psram_tuning_t const *t, size_t flipped_bytes, char *why, size_t cap);

// Reboot-once policy. State lives in NVS (RTC_NOINIT does not survive a software reset on the P4, #4).
typedef struct {
    uint32_t retries;  // reboots asked for since the last good boot
    uint32_t last_s;   // wall-clock time of the last one (0 = clock was not set)
} psram_retry_t;

// Updates the state; returns true when this bad boot should be answered with a reboot: bad, no retry yet, or the last one
// is 10 minutes ago or more (with the clock unknown a second retry is never allowed). A good boot clears the state.
bool psram_retry_decide(psram_retry_t *st, bool bad, uint32_t now_s);
