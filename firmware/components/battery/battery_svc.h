#pragma once
// The battery service's decisions (#11), pure: what one poll of the MAX17048 means. host_tests/battery_svc_test.c
// pins it. battery_service.c does the I2C and the task; it hands every poll's raw registers here and acts on the
// returned action. The chip is untrusted input: a reading reaches the model (battery_model.c) only when every register
// passes battery_regs_check, and a failed or rejected poll counts as "no reading", which restarts every dwell.
#include "battery_model.h"

#include <stdbool.h>
#include <stdint.h>

// Where the gauge stands. ABSENT is the stock badge: nothing answered (or something that is not a MAX17048 did), so
// nothing is polled until a probe is asked for again. STALE: it answered before, the last poll failed or was rejected;
// the last good reading is kept (with its age). LOST: BAT_LOST_FAILS polls in a row failed; polled slowly from then on.
typedef enum {
    BAT_GAUGE_UNPROBED = 0,
    BAT_GAUGE_ABSENT,
    BAT_GAUGE_PRESENT,
    BAT_GAUGE_STALE,
    BAT_GAUGE_LOST,
} bat_gauge_t;

// One poll's registers as the chip sent them (most significant byte first already combined, bat_be16). ok false: a
// transfer failed, the values mean nothing.
typedef struct {
    bool     ok;
    uint16_t version;
    uint16_t vcell;
    uint16_t soc;
    uint16_t crate;
} bat_regs_t;

typedef enum {
    BAT_REGS_OK = 0,
    BAT_REGS_IO,      // a transfer failed
    BAT_REGS_VERSION, // not a MAX17048 (another chip at 0x36, or the bus returned garbage)
    BAT_REGS_VCELL,   // outside BAT_VCELL_MIN_MV..BAT_VCELL_MAX_MV
    BAT_REGS_SOC,     // above BAT_SOC_MAX_PCT256
    BAT_REGS_CRATE,   // faster than BAT_CRATE_MAX_CENTIPCT_H either way
} bat_regs_verdict_t;

// Plausibility limits (mine, not the datasheet's): a single Li-ion cell lives in 2.5..4.6 V (below is a dead or
// disconnected cell, above is no Li-ion cell; 0x0000 and 0xFFFF, a stuck bus, fall outside). ModelGauge may report a
// little over 100 % before it has learnt the cell, so up to 110 %. 500 %/h is a 5C rate, beyond anything these cells see.
#define BAT_VCELL_MIN_MV        2500
#define BAT_VCELL_MAX_MV        BAT_CELL_MAX_MV
#define BAT_SOC_MAX_PCT256      (110 * 256)
#define BAT_CRATE_MAX_CENTIPCT_H 50000

// Poll periods. 5 s: the model's 10 s dwell then sees at least two readings, and the cell voltage moves over minutes;
// the MAX17048 converts every 250 ms on its own, so a read never waits. LOST: once a minute, a loose wire may come back.
#define BAT_POLL_MS      5000
#define BAT_LOST_POLL_MS 60000
#define BAT_LOST_FAILS   6 // 30 s of failed polls

typedef enum {
    BAT_ACT_NONE = 0,
    BAT_ACT_WARN,       // the level went from OK to WARN (or straight to OFF with the auto power-off disabled)
    BAT_ACT_WARN_CLEAR, // back to OK
    BAT_ACT_OFF,        // the model latched OFF and auto_off was passed true: once per boot
} bat_action_t;

typedef struct {
    bat_gauge_t        gauge;
    bat_state_t        model;
    bool               have; // a reading has passed the checks since the last probe
    uint32_t           mv;
    uint16_t           soc_pct256;
    int32_t            crate_centipct_h;
    bat_trend_t        trend;
    uint32_t           last_ok_ms; // when the kept reading was taken
    uint16_t           fails;      // failed or rejected polls in a row
    uint32_t           polls, io_errors, rejected;
    bat_regs_verdict_t last_reject;
    bool               off_due;  // the model latched OFF (shown; acted on only with auto_off)
    bool               off_sent; // BAT_ACT_OFF was returned
} bat_svc_t;

char const        *battery_regs_verdict_name(bat_regs_verdict_t v);
char const        *battery_gauge_name(bat_gauge_t g);
bat_regs_verdict_t battery_regs_check(bat_regs_t const *r);

void battery_service_init(bat_svc_t *s);
// The probe's result: found means the chip acknowledged 0x36 and its VERSION passed bat_version_valid. Every probe
// starts over: the model (its dwells and the OFF latch) and the kept reading are reset; the counters stay.
void battery_service_probed(bat_svc_t *s, bool found);
// One poll at now_ms (wrapping ms clock). r NULL counts as a failed transfer. auto_off: whether a latched OFF may be
// returned as BAT_ACT_OFF; with it false the service never asks for a power-off, whatever the chip reports. While the
// gauge is UNPROBED or ABSENT nothing changes and the result is BAT_ACT_NONE.
bat_action_t battery_service_step(bat_svc_t *s, bat_regs_t const *r, bool auto_off, uint32_t now_ms);
// How long until the next poll; 0 = do not poll (UNPROBED, ABSENT)
uint32_t battery_service_poll_ms(bat_svc_t const *s);
