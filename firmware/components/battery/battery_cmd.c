// "fw battery [probe]" (#11): the bench read-back of the fuel gauge, developer mode only (#373). Reads the service's
// snapshot; only "probe" makes the task touch the bus, and on the task, not here.
#include "battery.h"

#include "fw_console.h"

#include <stdlib.h>
#include <string.h>

static char const *level_name(bat_level_t l) {
    return l == BAT_OFF ? "off due" : l == BAT_WARN ? "low" : "ok";
}

static char const *trend_name(bat_trend_t t) {
    return t == BAT_TREND_CHARGING ? "charging" : t == BAT_TREND_DISCHARGING ? "discharging"
                                              : t == BAT_TREND_IDLE        ? "idle"
                                                                           : "?";
}

static void reply_reading(battery_snap_t const *s) {
    int32_t  c   = s->crate_centipct_h;
    unsigned mag = (unsigned)labs((long)c);
    console_reply("battery: %s, %u.%03u V, %u %% (curve %u %%), %c%u.%02u %%/h %s, level %s%s, %u s old",
                  battery_gauge_name(s->gauge), (unsigned)(s->mv / 1000), (unsigned)(s->mv % 1000),
                  (unsigned)s->percent, (unsigned)s->curve_percent, c < 0 ? '-' : '+', mag / 100, mag % 100,
                  trend_name(s->trend), level_name(s->level), s->off_due ? " (auto-off disabled)" : "",
                  (unsigned)(s->age_ms / 1000));
}

static void cmd_battery(char const *args) {
    if (strcmp(args, "probe") == 0) {
        console_reply(battery_service_reprobe() ? "battery: probing; fw battery shows the result"
                                                : "battery: service not running");
        return;
    }
    if (args[0]) {
        console_reply("battery [probe]");
        return;
    }
    battery_snap_t s;
    battery_get(&s);
    if (s.have) {
        reply_reading(&s);
    } else {
        console_reply("battery: %s (probe: %s)", battery_gauge_name(s.gauge), battery_probe_name(s.probe));
    }
    console_reply("battery: polls %u, i2c errors %u, rejected %u (last %s), probe %u ms, tmp102 %s, stack %u B free",
                  (unsigned)s.polls, (unsigned)s.io_errors, (unsigned)s.rejected,
                  s.rejected ? battery_regs_verdict_name(s.last_reject) : "-", (unsigned)s.probe_ms,
                  s.tmp102_seen ? "yes" : "no", (unsigned)s.stack_free);
}

void battery_register_console(void) {
    console_register_dev("battery", "battery [probe] (MAX17048 fuel gauge on the camera header, #11)", cmd_battery);
}
