#pragma once

#include "reg.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The settings registry's console side (#310): `fw get [key|prefix]` rows and the `fw set <key> <value>` path. Pure,
// host-tested (reg_service_test.c); lichen_reg.c builds the views and prints the lines.

// Why a key has no current applied value, or why it may be old
typedef enum {
    RV_NONE,       // applied is current
    RV_STALE,      // applied is the last answer: the C6 went away since, or did not answer this time
    RV_NO_ANSWER,  // the C6 did not answer and nothing was ever heard for this key
    RV_OLD_C6,     // no answer to c6.reg_caps: a C6 without the registry
    RV_NO_DOMAIN,  // the C6 has the registry, but not this key's domain (c6.reg_caps)
    RV_UNKNOWN_ID, // the C6 answered UNKNOWN for this id
    RV_NO_LINK,    // nothing could be sent: LoRa is not running or the C6 is being recovered
    RV_FAILED,     // the getter (or the C6's) failed
    RV_WAITING,    // c6.reg_caps not answered yet
    RV_NOT_APPLIED, // the C6 has no applied value yet (lora.*: no radio start since its reset)
} rv_note_t;

// One key as `fw get` shows it: saved (the settings key), requested (what the P4 asked for), applied (the hardware)
typedef struct {
    bool        has_saved, has_requested, has_applied, has_age;
    reg_value_t saved, requested, applied;
    uint32_t    age_ms; // of applied, for keys the C6 serves
    uint8_t     note;   // rv_note_t
} reg_view_t;

char const *reg_note_text(rv_note_t note);

// Applied against requested (#307/#310): the C6 reports the radio's real values. lora.freq sits on the SX1262's ~61 Hz
// synthesizer step (within REG_FREQ_TOLERANCE_HZ is a match), lora.bw is the chip's exact bandwidth (7812 for the
// nominal 7800 ...): both sides are mapped to the nearest SX126x bandwidth first. bl.display / bl.keyboard: the C6
// clamps a lit level to 10..80 % (5 -> 10, 95 -> 80; 0 is off). Everything else is exact. The mismatch W line and
// `fw get` ("≈" before a tolerated applied value) both use this.
typedef enum { RM_EQUAL, RM_TOLERATED, RM_DIFFERS } reg_match_t;
#define REG_FREQ_TOLERANCE_HZ 64
reg_match_t reg_applied_matches(uint16_t id, reg_value_t const *requested, reg_value_t const *applied);

// The column header and one row; bytes written without the NUL (cut to cap)
size_t reg_cmd_header(char *buf, size_t cap);
size_t reg_cmd_row(reg_entry_t const *e, reg_view_t const *v, char *buf, size_t cap);

typedef enum {
    RC_OK,
    RC_USAGE,     // not "<key> <value>"
    RC_UNKNOWN,   // no such key
    RC_BAD_VALUE, // not a value of the key's type
    RC_GUARDED,   // TX key: its own guarded path only (#307); nothing was sent
    RC_RO,
    RC_RANGE,
    RC_FAILED,    // set, but no applied value could be read back (or the setter failed)
    RC_UNCONFIRMED, // set, and this C6 cannot read it back (no registry or no domain): sent over UART1 (bl.*)
} reg_cmd_result_t;

// "<key> <value>": find, parse, reg_set (TX -> GUARDED before any setter). *e is the key when one was found, *applied
// the value read back (valid when *have_applied). confirmable(e): the peer can read this key back now; a set whose read
// back failed on a key that is not is RC_UNCONFIRMED with *applied = the value asked for. NULL: always confirmable.
typedef bool (*reg_confirmable_fn)(reg_entry_t const *e);
reg_cmd_result_t reg_cmd_set(reg_entry_t const *tab, size_t n, char const *args, reg_confirmable_fn confirmable,
                             reg_entry_t const **e, reg_value_t *applied, bool *have_applied);
// The reply line for a reg_cmd_set result: "bl.display = 50 %", "bl.display = 50 % (sent over UART, not confirmed by
// this C6)", "ERR guarded: ...", "ERR unknown key ..."
size_t reg_cmd_set_reply(reg_cmd_result_t r, reg_entry_t const *e, reg_value_t const *applied, bool have_applied,
                         char const *args, char *buf, size_t cap);

// One key's cached view for a status line that never waits (`fw airplane`'s bt.state, #311): "idle (12.3 s ago)",
// "idle (stale, 40 s ago)", "? (not supported by this C6)"
size_t reg_cached_text(reg_entry_t const *e, reg_view_t const *v, char *buf, size_t cap);

// The lora.* views in id order (freq, bw, sf, cr, sync, preamble, power, state) as one line for `fw lora`:
// "869.618 MHz 62.5 kHz SF7 CR5 sync 0x12 pre 16 22 dBm rx | 1.2 s ago" or "? (not supported by this C6)"
#define REG_RADIO_KEYS 8
size_t reg_radio_text(reg_view_t const v[REG_RADIO_KEYS], reg_entry_t const *state, char *buf, size_t cap);
