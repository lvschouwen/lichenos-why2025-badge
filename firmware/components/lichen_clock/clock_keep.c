#include "clock_keep.h"

#include "clock_check.h"

#define REC_MAGIC 0x4C434B31u // "LCK1": a record of this layout

clock_fix_t clock_fix_for(int64_t offset_us) {
    return offset_us < CLOCK_SLEW_MAX_US && offset_us > -CLOCK_SLEW_MAX_US ? CLOCK_FIX_SLEW : CLOCK_FIX_STEP;
}

char const *clock_fix_text(clock_fix_t f) {
    switch (f) {
        case CLOCK_FIX_SLEW: return "slewed";
        case CLOCK_FIX_STEP: return "stepped";
        case CLOCK_FIX_NONE: return "none";
    }
    return "?";
}

int64_t clock_drift_ms(int64_t step_ms, int64_t pending_ms) {
    return step_ms - pending_ms;
}

// Not a CRC: it only has to tell a record this code wrote from zeroes, garbage and a write a reset cut short
static uint32_t rec_check(uint32_t wall_s, uint32_t rtc_s) {
    uint32_t h = REC_MAGIC ^ wall_s;
    h          = (h << 7 | h >> 25) ^ rtc_s;
    return h * 2654435761u;
}

clock_rec_t clock_rec_make(uint32_t wall_s, uint32_t rtc_s) {
    return (clock_rec_t){.wall_s = wall_s, .rtc_s = rtc_s, .check = rec_check(wall_s, rtc_s)};
}

bool clock_rec_valid(clock_rec_t const *r) {
    return r->wall_s != 0 && r->check == rec_check(r->wall_s, r->rtc_s);
}

int64_t clock_rec_estimate(clock_rec_t const *r, int64_t rtc_now_s) {
    int64_t elapsed = rtc_now_s - (int64_t)r->rtc_s;
    if (elapsed < 0 || elapsed > CLOCK_SAVE_ELAPSED_MAX_S) {
        elapsed = 0;
    }
    return (int64_t)r->wall_s + elapsed;
}

clock_boot_t clock_boot_pick(int64_t carried_s, clock_rec_t const *r, int64_t rtc_now_s, int64_t build_epoch,
                             int64_t *out_s) {
    bool carried_ok = clock_in_window(carried_s, build_epoch);
    if (clock_rec_valid(r)) {
        int64_t est = clock_rec_estimate(r, rtc_now_s);
        if (clock_in_window(est, build_epoch) && (!carried_ok || est > carried_s + CLOCK_SAVE_MARGIN_S)) {
            *out_s = est;
            return CLOCK_BOOT_SAVED;
        }
    }
    return carried_ok ? CLOCK_BOOT_CARRIED : CLOCK_BOOT_NONE;
}

char const *clock_boot_text(clock_boot_t b) {
    switch (b) {
        case CLOCK_BOOT_SAVED: return "saved";
        case CLOCK_BOOT_CARRIED: return "carried";
        case CLOCK_BOOT_NONE: return "neither";
    }
    return "?";
}
