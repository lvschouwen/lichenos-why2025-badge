#pragma once
// The engine-only timing of the #558 spike, pure (host_tests/doom_spike_test.c). The module calls its own clock for every
// timestamp, so a frame, a sleep inside it and the unit they are counted in cannot come from different clocks. GPL-3.0-or-later.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t (*now)(void); // the one clock, in units; unit_per_ms of them make a millisecond
    uint32_t unit_per_ms;
    uint32_t frame_t0, sleep_t0, sleep_units; // sleep_units: waited inside the current frame
    uint32_t frames, level_frames, over33, over50, max_units;
    uint64_t work_units;
} doom_spike_t;

void doom_spike_init(doom_spike_t *s, uint32_t (*now)(void), uint32_t unit_per_ms);
void doom_spike_frame_begin(doom_spike_t *s);
void doom_spike_sleep_begin(doom_spike_t *s);
void doom_spike_sleep_end(doom_spike_t *s);
// Ends a frame: work = its time minus the sleeps in it. Only a level frame is counted in the statistics.
void doom_spike_frame_end(doom_spike_t *s, bool level);
// "core N cyc|tick: avg A.B max C.D ms, >33 ms n, >50 ms m of K level frames, F fps wall"; wall_ms spans all frames
void doom_spike_format(doom_spike_t const *s, char *out, size_t cap, int core, bool cycles, uint32_t wall_ms);
