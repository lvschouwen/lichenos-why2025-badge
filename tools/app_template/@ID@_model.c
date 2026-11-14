#include "@ID@_model.h"

#include <stdio.h>

static bool step_ok(uint32_t step) {
    return step == 1 || step == 5 || step == 10;
}

void @ID@_model_init(@ID@_model_t *m, uint32_t count, uint32_t step) {
    m->count = count > @UP@_COUNT_MAX ? @UP@_COUNT_MAX : count;
    m->step  = step_ok(step) ? step : 1;
}

bool @ID@_model_tap(@ID@_model_t *m) {
    if (m->count >= @UP@_COUNT_MAX) {
        return false;
    }
    uint32_t room = @UP@_COUNT_MAX - m->count;
    m->count += m->step < room ? m->step : room;
    return true;
}

uint32_t @ID@_model_next_step(uint32_t step) {
    return step == 1 ? 5 : step == 5 ? 10 : 1;
}

void @ID@_model_text(@ID@_model_t const *m, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (m->count == 0) {
        snprintf(out, cap, "no taps");
    } else {
        snprintf(out, cap, "%u %s", (unsigned)m->count, m->count == 1 ? "tap" : "taps");
    }
}
