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

// "fw psram dl <idx> yes" (#121): true when text is an idx and the literal confirmation "yes", nothing else
bool psram_tuning_dl_args(char const *text, unsigned *idx);
// An idx "fw psram dl" may set live: inside the passing run the boot tuning measured (never after a fallback); outside
// it the CPU path itself fails, and code, data and DMA buffers all live in PSRAM
bool psram_tuning_dl_in_window(psram_tuning_t const *t, unsigned idx);

// LP store register carrier spike (#121): a 16-bit magic and a 16-bit value in one 32-bit word, so a stale or foreign
// value (power-on reset leaves 0 or noise) never reads as set
#define PSRAM_CARRIER_MAGIC 0xC4A1u
uint32_t psram_carrier_pack(uint16_t value);
bool     psram_carrier_unpack(uint32_t word, uint16_t *value); // false = empty or not ours

// One-shot delayline override for the 120 MHz bench (#121): "fw psram dlboot <idx> yes" stores a record in NVS, the next
// boot consumes it (erased BEFORE it is applied, so a crash or a bad idx never repeats) and sets the delayline once NVS is
// up. Anything the verdict does not accept leaves the IDF's own choice in place.
#define PSRAM_ONESHOT_MAGIC 0x314C4450u // "PDL1"

typedef struct {
    uint32_t magic;
    uint32_t idx;
} psram_oneshot_t;

typedef enum {
    PSRAM_ONESHOT_APPLY,       // set it
    PSRAM_ONESHOT_BAD_RECORD,  // wrong magic or idx not 0..30
    PSRAM_ONESHOT_FELL_BACK,   // the boot tuning fell back: there is no window to stay in
    PSRAM_ONESHOT_OUTSIDE,     // outside the window this boot's tuning measured (the window differs between boots, #94)
} psram_oneshot_verdict_t;

psram_oneshot_verdict_t psram_oneshot_verdict(psram_oneshot_t const *rec, psram_tuning_t const *t);
char const             *psram_oneshot_verdict_text(psram_oneshot_verdict_t v);

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

// How the stored state came back: read, not there yet (first boot, or cleared), or unreadable (no NVS, read error,
// wrong size)
typedef enum { PSRAM_RETRY_READ_OK, PSRAM_RETRY_READ_NONE, PSRAM_RETRY_READ_FAIL } psram_retry_read_t;

// Stores the state through save; returns true only when it may reboot: psram_retry_decide says so, the state was read
// (or absent) and the save worked. Unknown state counts as the cap reached, so a broken store can never loop the
// badge on reboots (#345). save may be NULL (no store: never reboots).
bool psram_retry_run(psram_retry_t *st, psram_retry_read_t read, bool bad, uint32_t now_s,
                     bool (*save)(void *ctx, psram_retry_t const *st), void *ctx);
