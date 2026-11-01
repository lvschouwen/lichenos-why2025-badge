#pragma once
// The thermal watchdog's decisions (#10). Pure (readings in, events out), host-tested in host_tests/thermal_rules_test.c;
// thermal.c reads the sensors and acts on the events. The badge cannot see its cells (no data link from the IP5306),
// so heat near them is the only warning it can give.
//
// Sources: BOARD is the BME690 (beside the cells) with its gas-heater bias taken off by the caller, DIE is the P4's
// own sensor. They have their own limits: the die runs 15-25 K above the board, so it only guards the chip.
//
// Rules, per poll (the service polls every 5 s):
//  - a reading that is missing, stale (the caller's call) or implausible (th_plausible) counts as no reading and
//    restarts that source's 60 s rise window;
//  - warn when a source is at or above warn_c, shut down at or above shutdown_c or when it rose rise_c within
//    TH_WINDOW_S (only from rise_floor_c up, so a cold badge warming in a pocket does not trip it);
//  - either needs TH_CONFIRM polls in a row, so one glitch reading changes nothing;
//  - the warning clears after TH_CONFIRM polls with every valid source under clear_c (hysteresis);
//  - shutdown latches (the badge powers off; a test that says otherwise calls th_reset);
//  - broken sensors: a source without readings is ignored while another one answers (protection continues on the
//    rest); with no source at all for TH_BLIND_S the badge is "blind": that is reported once (TH_EV_BLIND, and again
//    as TH_EV_SIGHT when readings return) but never shuts the badge down by itself;
//  - the board sensor alone is the one near the cells, so its loss is reported on its own (TH_EV_BOARD_LOST after
//    TH_BLIND_S, TH_EV_BOARD_BACK) even while the die sensor still answers, which then guards the chip only;
//  - by design a sustained die rise of 20 K within 60 s from 60 C up powers the badge off (a heavy load that heats the
//    chip that fast is also a reason to stop).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TH_SRC_BOARD, TH_SRC_DIE, TH_SRC_COUNT } th_src_t;
typedef enum { TH_OK, TH_WARN, TH_SHUTDOWN } th_level_t;

#define TH_WINDOW_S   60  // the rise is measured over this long
#define TH_CONFIRM    2   // polls in a row
#define TH_BLIND_S    60  // without any reading
#define TH_SAMPLES    16  // history per source: 60 s at 5 s plus room for a faster poll
#define TH_PLAUSIBLE_MIN (-40.0f)
#define TH_PLAUSIBLE_MAX 125.0f
// What the gas heater adds to the BME690's temperature (measured about +17 K at the 100 ms / 1 s default). Taken off
// a little short on purpose: a board that reads warm warns early, one that reads cold would not warn at all.
#define TH_BME_HEATER_OFFSET_C 15.0f

typedef struct {
    float warn_c, clear_c, shutdown_c;
    float rise_c, rise_floor_c;
} th_limits_t;

th_limits_t th_limits(th_src_t src);
bool        th_plausible(float c);
// The board temperature from a raw BME690 reading
float       th_board_from_bme(float raw_c, bool heater_on);

typedef struct {
    bool  valid;
    float c;
} th_reading_t;
typedef struct {
    th_reading_t src[TH_SRC_COUNT];
} th_input_t;

typedef struct {
    float    c[TH_SAMPLES];
    uint32_t t[TH_SAMPLES];
    uint8_t  n, head;
} th_hist_t;

typedef struct {
    th_level_t level;
    bool       blind;
    bool       started;
    bool       board_lost, started_board;
    uint32_t   board_ok_s; // the last poll with a usable board reading
    uint8_t    hot_polls, crit_polls, cool_polls;
    uint32_t   last_reading_s; // the last poll with any valid source (start time before the first)
    uint32_t   implausible[TH_SRC_COUNT]; // readings refused
    th_hist_t  hist[TH_SRC_COUNT];
    th_src_t   cause;      // the source behind the last warn or shutdown
    bool       cause_rise; // ... because of the rise rule, not the level
    float      cause_c;
} th_state_t;

enum {
    TH_EV_WARN    = 1 << 0, // entered the warning
    TH_EV_CLEARED = 1 << 1, // the warning ended
    TH_EV_SHUTDOWN = 1 << 2,
    TH_EV_BLIND   = 1 << 3,
    TH_EV_SIGHT   = 1 << 4,
    TH_EV_BOARD_LOST = 1 << 5, // the board sensor (BME690, the one beside the cells) gave nothing for TH_BLIND_S
    TH_EV_BOARD_BACK = 1 << 6,
};

void th_reset(th_state_t *st);
// One poll at now_s (seconds, any origin, must not go backwards): the TH_EV_* set of what happened
unsigned th_step(th_state_t *st, th_input_t const *in, uint32_t now_s);
// A source's rise inside the window, 0 when it has no history
float th_rise(th_state_t const *st, th_src_t src, uint32_t now_s);
char const *th_src_name(th_src_t src);
char const *th_level_name(th_level_t level);

#ifdef __cplusplus
}
#endif
