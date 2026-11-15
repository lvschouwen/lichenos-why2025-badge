#pragma once

#include "display_flip.h"

#include <stdbool.h>
#include <stdint.h>

// What turns each LVGL strip into the frame buffer (#468, #475): the PPA in the background (LVGL renders strip N+1
// while the PPA turns strip N, flush_ready from the PPA's done interrupt), the default after boot, or the CPU
// (lv_draw_sw_rotate), the fallback and, in a developer build, a bench switch to compare. The
// order of a strip, a PPA transfer, the copy-back and the swap request lives here, pure (host_tests/
// display_engine_test.c); display.c holds the hardware hooks.
typedef enum {
    DISPLAY_ENGINE_CPU,
    DISPLAY_ENGINE_PPA_ASYNC,
    DISPLAY_ENGINE_COUNT,
} display_engine_id_t;

// The registry's names for the values above (disp.engine), in order
#define DISPLAY_ENGINE_NAMES {"cpu", "ppa-async"}

bool        display_engine_valid(uint32_t id);
char const *display_engine_name(uint32_t id); // "?" for an unknown id

// One strip as LVGL hands it over: the UI area, where it lands on the panel, the rendered pixels and their row pitch
typedef struct {
    display_rect_t ui, blk;
    void const    *px;
    uint32_t       stride;
} display_strip_t;

typedef enum {
    DISPLAY_FALLBACK_START,   // the driver refused the transfer
    DISPLAY_FALLBACK_TIMEOUT, // no done within the hook's bound
} display_fallback_t;

typedef struct {
    // Turn the strip into buffer fb on the CPU, written back before it returns
    void (*cpu)(void *ctx, display_strip_t const *s, unsigned fb);
    // Start a PPA transfer of the strip into buffer fb and return at once. ready_in_isr: the done interrupt calls
    // lv_display_flush_ready (a background strip). gen tags the transfer: its done counts only while gen is current
    // (display_engine_done_is_current). False = refused, nothing started.
    bool (*ppa_start)(void *ctx, display_strip_t const *s, unsigned fb, bool ready_in_isr, uint32_t gen);
    // Bounded wait for the started transfer's done; *done_us = the interrupt's stamp. late = the second, longer wait
    // after the first one timed out: the hook keeps the transfer current until then, and a false from a late wait
    // means the PPA is given up on (the interrupt no longer calls flush_ready, its done is ignored).
    bool (*ppa_wait)(void *ctx, uint32_t *done_us, bool late);
    // The engine fell back to the CPU (log it); a new select re-arms the PPA
    void (*fell_back)(void *ctx, display_engine_id_t from, display_fallback_t why);
    uint32_t (*now_us)(void *ctx);
    void *ctx;
} display_engine_ops_t;

typedef struct {
    uint32_t strips, rotate_max_us, fallbacks;
    uint64_t rotate_us; // start of the turn to its end (CPU) or to the done interrupt (PPA), cache maintenance included
} display_engine_stats_t;

typedef struct {
    display_engine_ops_t   ops;
    display_engine_id_t    active, want;
    bool                   in_flight; // a PPA transfer was started and its done not taken yet
    bool                   hung;      // a transfer timed out and has not reported its done: the PPA may still run
    uint32_t               gen;       // the last transfer's tag, never 0
    uint32_t               hung_gen;  // the tag of the transfer that timed out
    bool                   mid_frame; // a strip of an unfinished refresh went out
    uint32_t               start_us;
    unsigned               flight_fb; // where the transfer in flight writes, for the CPU redo after a timeout
    display_strip_t        flight;
    display_engine_stats_t stats;
} display_engine_t;

void display_engine_init(display_engine_t *e, display_engine_ops_t const *ops);

// Ask for an engine. Applied at once at a frame boundary with nothing in flight (always so under the LVGL lock, as
// the registry setter calls it), else at the first strip of the next refresh. Clears the rotate stats. False = unknown,
// or a PPA engine while the PPA has not finished a transfer that timed out (display_engine_ppa_hung): a re-armed PPA
// could otherwise run two transfers at once, the old one reading a draw buffer LVGL has rendered anew since.
bool display_engine_select(display_engine_t *e, uint32_t id);
bool display_engine_ppa_hung(display_engine_t const *e);

// Transfer tags (#468 review): the done interrupt of a transfer the engine gave up on must neither ready LVGL nor be
// taken as the done of a later transfer. The next tag after g (0 is skipped: it means "none current").
uint32_t display_engine_gen_next(uint32_t g);
// The done interrupt's test: the tag it got belongs to the transfer in flight (current = 0 after a timeout)
bool     display_engine_done_is_current(uint32_t current, uint32_t got);
// A done reported with tag got, current or not: the timed-out transfer's own done says the PPA is idle again
void     display_engine_ppa_done_seen(display_engine_t *e, uint32_t got);

// The engine the strips use now (the registry's applied value)
display_engine_id_t display_engine_active(display_engine_t const *e);

// Take the done of a transfer in flight (bounded, twice); on a timeout fall back to the CPU and redo that strip, whose
// pixels LVGL keeps until the next flush returns. The redo waits for a late done first (#475), so a slow transfer
// does not write its old strip over what the CPU path draws next; a PPA that gives none even then is `hung`. LVGL's flush_wait_cb, and the start of every strip.
void display_engine_settle(display_engine_t *e);

// One strip, the flush callback's body. Order: settle the previous transfer, then the swap state machine's write
// target (its copy-back never overlaps a transfer), then the turn; the last strip of a refresh waits for its own
// transfer before the switch is requested, so a refresh never ends with a transfer in flight. True = the strip is
// done (call lv_display_flush_ready now); false = the done interrupt calls it.
bool display_engine_flush(display_engine_t *e, display_flip_t *flip, display_strip_t const *s, bool last);
