#pragma once
// Telemetry request to a contact (#162): reading the Cayenne LPP an answer carries, the per-contact rate limit and the
// text of the result page. ESP-free (the clock is passed in, 32-bit wrap safe) and not locked: the rate table is read
// and written by the badge inside mesh_ob_enter/exit, as mesh_remote.h. The request itself rides on mesh_remote.h
// (RR_TELEMETRY), whose state machine times it out.
#include "mesh_remote.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TL_LINE_MAX  64
#define TL_LINES_MAX 24
typedef struct {
    char text[TL_LINE_MAX];
} tl_line_t;

// The values of a Cayenne LPP buffer (channel, type, data ...) as human lines: "Battery: 4.12 V", "Temperature: 21.5 °C",
// "Location: 52.37403, 4.88969, 3 m". The sensor's own channel 1 voltage is the battery (MeshCore's TELEM_CHANNEL_SELF);
// other channels carry "(channel N)". A type we do not know, or a cut value, ends the list with a line saying so (the
// length of a value is not known without its type). Returns the lines written (at most max).
size_t tl_lpp_lines(uint8_t const *lpp, size_t len, tl_line_t *out, size_t max);

// ---- rate limit: one request per contact per TL_RATE_MS ------------------------------------------------------------
#define TL_RATE_MS    30000u
#define TL_RATE_SLOTS 8
typedef struct {
    uint8_t  prefix[MF_PREFIX];
    uint32_t at_ms;
    bool     used;
} tl_slot_t;
typedef struct {
    tl_slot_t slot[TL_RATE_SLOTS];
} tl_rate_t;

void tl_rate_init(tl_rate_t *t);
// Seconds (rounded up) until this contact may be asked again; 0 = now
uint32_t tl_rate_wait_s(tl_rate_t const *t, uint8_t const prefix[MF_PREFIX], uint32_t now_ms);
// A request to this contact was made now (the oldest slot is reused when the table is full)
void tl_rate_note(tl_rate_t *t, uint8_t const prefix[MF_PREFIX], uint32_t now_ms);

// ---- the result page ------------------------------------------------------------------------------------------------
// The page's text for the request r about target: the values of the newest answer from that contact, then a line for
// where the request stands: "Asking... waiting for the answer (direct)", "No answer: ..." (with the next step), or
// nothing extra after a good answer. Returns the length written.
size_t tl_view_text(rr_t const *r, uint8_t const target[MF_PREFIX], char *out, size_t cap);

#ifdef __cplusplus
}
#endif
