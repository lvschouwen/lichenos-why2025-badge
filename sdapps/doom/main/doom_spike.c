#include "doom_spike.h"

#include <stdio.h>

void doom_spike_init(doom_spike_t *s, uint32_t (*now)(void), uint32_t unit_per_ms) {
    *s             = (doom_spike_t){0};
    s->now         = now;
    s->unit_per_ms = unit_per_ms ? unit_per_ms : 1;
}

void doom_spike_frame_begin(doom_spike_t *s) {
    s->sleep_units = 0;
    s->frame_t0    = s->now();
}

void doom_spike_sleep_begin(doom_spike_t *s) {
    s->sleep_t0 = s->now();
}

void doom_spike_sleep_end(doom_spike_t *s) {
    s->sleep_units += s->now() - s->sleep_t0;
}

void doom_spike_frame_end(doom_spike_t *s, bool level) {
    uint32_t total = s->now() - s->frame_t0;
    uint32_t work  = total > s->sleep_units ? total - s->sleep_units : 0;
    s->frames++;
    if (!level) {
        return;
    }
    s->level_frames++;
    s->work_units += work;
    s->over33 += work > 33 * s->unit_per_ms;
    s->over50 += work > 50 * s->unit_per_ms;
    s->max_units = work > s->max_units ? work : s->max_units;
}

void doom_spike_format(doom_spike_t const *s, char *out, size_t cap, int core, bool cycles, uint32_t wall_ms) {
    if (!s->level_frames) {
        snprintf(out, cap, "spike: no level frame in %u frames (start a game or wait for a demo)", (unsigned)s->frames);
        return;
    }
    uint32_t avg10 = (uint32_t)(s->work_units * 10 / s->level_frames / s->unit_per_ms);
    uint32_t max10 = (uint32_t)((uint64_t)s->max_units * 10 / s->unit_per_ms);
    snprintf(out, cap, "spike core %d %s: avg %u.%u max %u.%u ms, >33 ms %u, >50 ms %u of %u level frames, %u fps wall", core,
             cycles ? "cyc" : "tick", (unsigned)(avg10 / 10), (unsigned)(avg10 % 10), (unsigned)(max10 / 10),
             (unsigned)(max10 % 10), (unsigned)s->over33, (unsigned)s->over50, (unsigned)s->level_frames,
             (unsigned)(wall_ms ? (uint64_t)s->frames * 1000 / wall_ms : 0));
}
