#include "battery_svc.h"

#include <stddef.h>

char const *battery_regs_verdict_name(bat_regs_verdict_t v) {
    static char const *const names[] = {
        [BAT_REGS_OK] = "ok",       [BAT_REGS_IO] = "i2c error", [BAT_REGS_VERSION] = "bad version",
        [BAT_REGS_VCELL] = "bad voltage", [BAT_REGS_SOC] = "bad charge", [BAT_REGS_CRATE] = "bad rate",
    };
    return (unsigned)v < sizeof names / sizeof names[0] ? names[v] : "?";
}

char const *battery_gauge_name(bat_gauge_t g) {
    static char const *const names[] = {
        [BAT_GAUGE_UNPROBED] = "not probed", [BAT_GAUGE_ABSENT] = "absent", [BAT_GAUGE_PRESENT] = "present",
        [BAT_GAUGE_STALE] = "stale",         [BAT_GAUGE_LOST] = "lost",
    };
    return (unsigned)g < sizeof names / sizeof names[0] ? names[g] : "?";
}

bat_regs_verdict_t battery_regs_check(bat_regs_t const *r) {
    if (!r || !r->ok) {
        return BAT_REGS_IO;
    }
    if (!bat_version_valid(r->version)) {
        return BAT_REGS_VERSION;
    }
    uint32_t mv = bat_vcell_mv(r->vcell);
    if (mv < BAT_VCELL_MIN_MV || mv > BAT_VCELL_MAX_MV) {
        return BAT_REGS_VCELL;
    }
    if (r->soc > BAT_SOC_MAX_PCT256) {
        return BAT_REGS_SOC;
    }
    int32_t rate = bat_crate_centipct_h(r->crate);
    if (rate > BAT_CRATE_MAX_CENTIPCT_H || rate < -BAT_CRATE_MAX_CENTIPCT_H) {
        return BAT_REGS_CRATE;
    }
    return BAT_REGS_OK;
}

void battery_service_init(bat_svc_t *s) {
    *s = (bat_svc_t){.gauge = BAT_GAUGE_UNPROBED};
    bat_reset(&s->model);
}

void battery_service_probed(bat_svc_t *s, bool found) {
    s->gauge   = found ? BAT_GAUGE_PRESENT : BAT_GAUGE_ABSENT;
    s->have    = false;
    s->fails   = 0;
    s->off_due = false; // off_sent stays: at most one BAT_ACT_OFF per boot
    bat_reset(&s->model);
}

// A poll that gave no usable reading: the kept reading goes stale, and the model hears "no reading" so every dwell
// restarts (a gap never counts as time spent low)
static void poll_failed(bat_svc_t *s, bat_regs_verdict_t v, uint32_t now_ms) {
    if (v == BAT_REGS_IO) {
        s->io_errors++;
    } else {
        s->rejected++;
        s->last_reject = v;
    }
    if (s->fails < UINT16_MAX) {
        s->fails++;
    }
    s->gauge = s->fails >= BAT_LOST_FAILS ? BAT_GAUGE_LOST : BAT_GAUGE_STALE;
    (void)bat_step(&s->model, -1, false, now_ms);
}

static bat_action_t level_action(bat_svc_t *s, bat_level_t before, bat_level_t after, bool auto_off) {
    if (after == BAT_OFF && before != BAT_OFF) {
        s->off_due = true;
        if (auto_off && !s->off_sent) {
            s->off_sent = true;
            return BAT_ACT_OFF;
        }
        return before == BAT_OK ? BAT_ACT_WARN : BAT_ACT_NONE;
    }
    if (before == BAT_OK && after == BAT_WARN) {
        return BAT_ACT_WARN;
    }
    if (before == BAT_WARN && after == BAT_OK) {
        return BAT_ACT_WARN_CLEAR;
    }
    return BAT_ACT_NONE;
}

bat_action_t battery_service_step(bat_svc_t *s, bat_regs_t const *r, bool auto_off, uint32_t now_ms) {
    if (s->gauge == BAT_GAUGE_UNPROBED || s->gauge == BAT_GAUGE_ABSENT) {
        return BAT_ACT_NONE;
    }
    s->polls++;
    bat_regs_verdict_t v = battery_regs_check(r);
    if (v != BAT_REGS_OK) {
        poll_failed(s, v, now_ms);
        return BAT_ACT_NONE;
    }
    s->gauge            = BAT_GAUGE_PRESENT;
    s->fails            = 0;
    s->have             = true;
    s->mv               = bat_vcell_mv(r->vcell);
    s->soc_pct256       = bat_soc_pct256(r->soc);
    s->crate_centipct_h = bat_crate_centipct_h(r->crate);
    s->trend            = bat_trend_from_crate(r->crate);
    s->last_ok_ms       = now_ms;
    bat_level_t before  = s->model.level;
    bat_level_t after   = bat_step(&s->model, (int32_t)s->mv, s->trend == BAT_TREND_CHARGING, now_ms);
    return level_action(s, before, after, auto_off);
}

uint32_t battery_service_poll_ms(bat_svc_t const *s) {
    switch (s->gauge) {
    case BAT_GAUGE_PRESENT:
    case BAT_GAUGE_STALE:
        return BAT_POLL_MS;
    case BAT_GAUGE_LOST:
        return BAT_LOST_POLL_MS;
    default:
        return 0;
    }
}
