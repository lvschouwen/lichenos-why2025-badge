// The battery service's decisions (#11, battery_svc.c): absent and present gauges, failed and garbage reads, the
// dwell, and that no reading the chip can send makes the service ask for a power-off unless auto_off is on.
#include "battery_svc.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                         \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

// raw VCELL for a cell voltage (78.125 uV per LSB: mV * 64 / 5)
static uint16_t vcell_raw(uint32_t mv) {
    return (uint16_t)(mv * 64u / 5u);
}

static bat_regs_t good(uint32_t mv) {
    return (bat_regs_t){.ok = true, .version = 0x0012, .vcell = vcell_raw(mv), .soc = 50 * 256, .crate = 0xFFF0};
}

static void present(bat_svc_t *s) {
    battery_service_init(s);
    battery_service_probed(s, BAT_PROBE_FOUND);
}

static void regs_check_limits(void) {
    bat_regs_t r = good(3800);
    CHECK(battery_regs_check(&r) == BAT_REGS_OK);
    CHECK(battery_regs_check(NULL) == BAT_REGS_IO);
    r.ok = false;
    CHECK(battery_regs_check(&r) == BAT_REGS_IO);
    r = good(3800), r.version = 0x0000;
    CHECK(battery_regs_check(&r) == BAT_REGS_VERSION);
    r.version = 0xFFFF; // a bus that reads all ones
    CHECK(battery_regs_check(&r) == BAT_REGS_VERSION);
    r = good(3800), r.vcell = 0x0000;
    CHECK(battery_regs_check(&r) == BAT_REGS_VCELL);
    r.vcell = 0xFFFF; // 5.119 V
    CHECK(battery_regs_check(&r) == BAT_REGS_VCELL);
    r.vcell = vcell_raw(BAT_VCELL_MIN_MV);
    CHECK(battery_regs_check(&r) == BAT_REGS_OK);
    r.vcell = vcell_raw(BAT_VCELL_MIN_MV) - 1;
    CHECK(battery_regs_check(&r) == BAT_REGS_VCELL);
    r.vcell = vcell_raw(BAT_VCELL_MAX_MV);
    CHECK(battery_regs_check(&r) == BAT_REGS_OK);
    r.vcell = (uint16_t)(vcell_raw(BAT_VCELL_MAX_MV) + 13); // 1 mV more
    CHECK(battery_regs_check(&r) == BAT_REGS_VCELL);
    r = good(3800), r.soc = BAT_SOC_MAX_PCT256;
    CHECK(battery_regs_check(&r) == BAT_REGS_OK);
    r.soc = BAT_SOC_MAX_PCT256 + 1;
    CHECK(battery_regs_check(&r) == BAT_REGS_SOC);
    r = good(3800), r.crate = 0x8000; // int16 minimum
    CHECK(battery_regs_check(&r) == BAT_REGS_CRATE);
    r.crate = 0x7FFF;
    CHECK(battery_regs_check(&r) == BAT_REGS_CRATE);
    r.crate = 2403; // 499.8 %/h
    CHECK(battery_regs_check(&r) == BAT_REGS_OK);
    r.crate = 2404; // 500.03 %/h
    CHECK(battery_regs_check(&r) == BAT_REGS_CRATE);
}

static void absent_does_nothing(void) {
    bat_svc_t s;
    battery_service_init(&s);
    CHECK(s.gauge == BAT_GAUGE_UNPROBED);
    CHECK(battery_service_poll_ms(&s) == 0);
    battery_service_probed(&s, BAT_PROBE_NOTHING);
    CHECK(s.gauge == BAT_GAUGE_ABSENT);
    CHECK(battery_service_poll_ms(&s) == 0);
    bat_regs_t low = good(3000);
    for (uint32_t t = 0; t <= 120000; t += 1000) {
        CHECK(battery_service_step(&s, &low, true, t) == BAT_ACT_NONE);
    }
    CHECK(s.gauge == BAT_GAUGE_ABSENT && !s.have && !s.off_due && !s.off_sent && s.polls == 0);
    CHECK(s.model.level == BAT_OK);
}

// Every probe result that is not FOUND leaves a state that is never polled and never acts, whatever comes in
static void probe_results_map_to_states(void) {
    static struct {
        bat_probe_t p;
        bat_gauge_t g;
    } const cases[] = {
        {BAT_PROBE_FOUND, BAT_GAUGE_PRESENT},       {BAT_PROBE_NOTHING, BAT_GAUGE_ABSENT},
        {BAT_PROBE_NOT_MAX, BAT_GAUGE_ABSENT},      {BAT_PROBE_LINES_LOW, BAT_GAUGE_LINES_LOW},
        {BAT_PROBE_BUS_ERROR, BAT_GAUGE_NO_BUS},    {BAT_PROBE_TIMEOUT, BAT_GAUGE_NO_BUS},
        {(bat_probe_t)99, BAT_GAUGE_NO_BUS},
    };
    bat_regs_t low = good(3000);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bat_svc_t s;
        battery_service_init(&s);
        battery_service_probed(&s, cases[i].p);
        CHECK(s.gauge == cases[i].g && s.probe == cases[i].p);
        if (cases[i].p == BAT_PROBE_FOUND) {
            continue;
        }
        CHECK(battery_service_poll_ms(&s) == 0);
        for (uint32_t t = 0; t <= 60000; t += 5000) {
            CHECK(battery_service_step(&s, &low, true, t) == BAT_ACT_NONE);
        }
        CHECK(s.gauge == cases[i].g && s.polls == 0 && !s.have && !s.off_due);
    }
    // A re-probe moves between them: present, then the lines are held low, then the gauge is back
    bat_svc_t s;
    battery_service_init(&s);
    battery_service_probed(&s, BAT_PROBE_FOUND);
    bat_regs_t ok = good(3900);
    battery_service_step(&s, &ok, false, 0);
    battery_service_probed(&s, BAT_PROBE_LINES_LOW);
    CHECK(s.gauge == BAT_GAUGE_LINES_LOW && !s.have && battery_service_poll_ms(&s) == 0);
    battery_service_probed(&s, BAT_PROBE_FOUND);
    CHECK(s.gauge == BAT_GAUGE_PRESENT && battery_service_poll_ms(&s) == BAT_POLL_MS);
}

static void reading_reaches_the_model(void) {
    bat_svc_t s;
    present(&s);
    CHECK(battery_service_poll_ms(&s) == BAT_POLL_MS);
    bat_regs_t r = good(3900);
    r.crate      = 48; // +9.98 %/h
    CHECK(battery_service_step(&s, &r, false, 1000) == BAT_ACT_NONE);
    CHECK(s.have && s.gauge == BAT_GAUGE_PRESENT);
    CHECK(s.mv == 3900 && s.soc_pct256 == 50 * 256 && s.trend == BAT_TREND_CHARGING);
    CHECK(s.crate_centipct_h == 998 && s.last_ok_ms == 1000 && s.polls == 1);
}

static void failures_go_stale_then_lost(void) {
    bat_svc_t  s;
    bat_regs_t r = good(3900);
    present(&s);
    battery_service_step(&s, &r, false, 0);
    bat_regs_t io = {.ok = false};
    CHECK(battery_service_step(&s, &io, false, 5000) == BAT_ACT_NONE);
    CHECK(s.gauge == BAT_GAUGE_STALE && s.have && s.mv == 3900 && s.last_ok_ms == 0 && s.io_errors == 1);
    CHECK(battery_service_poll_ms(&s) == BAT_POLL_MS);
    for (int i = 1; i < BAT_LOST_FAILS - 1; i++) {
        battery_service_step(&s, NULL, false, 5000u * (uint32_t)(i + 1));
    }
    CHECK(s.gauge == BAT_GAUGE_STALE);
    battery_service_step(&s, NULL, false, 40000);
    CHECK(s.gauge == BAT_GAUGE_LOST && s.fails == BAT_LOST_FAILS);
    CHECK(battery_service_poll_ms(&s) == BAT_LOST_POLL_MS);
    CHECK(battery_service_step(&s, &r, false, 100000) == BAT_ACT_NONE);
    CHECK(s.gauge == BAT_GAUGE_PRESENT && s.fails == 0 && s.last_ok_ms == 100000);
}

static void garbage_is_rejected_and_never_powers_off(void) {
    bat_svc_t s;
    present(&s);
    bat_regs_t junk[] = {
        {.ok = true, .version = 0xFFFF, .vcell = 0xFFFF, .soc = 0xFFFF, .crate = 0xFFFF}, // all ones
        {.ok = true, .version = 0x0000, .vcell = 0x0000, .soc = 0x0000, .crate = 0x0000}, // all zeros
        {.ok = true, .version = 0x0012, .vcell = 0x0000, .soc = 0, .crate = 0},           // 0 V
        {.ok = true, .version = 0x0012, .vcell = vcell_raw(2000), .soc = 0, .crate = 0},  // flat
        {.ok = true, .version = 0x0012, .vcell = vcell_raw(3200), .soc = 0xC800, .crate = 0},
        {.ok = true, .version = 0x0012, .vcell = vcell_raw(3200), .soc = 0, .crate = 0x8000},
        {.ok = true, .version = 0x0042, .vcell = vcell_raw(3200), .soc = 0, .crate = 0}, // another chip
    };
    uint32_t t = 0;
    for (int round = 0; round < 10; round++) {
        for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++, t += 5000) {
            CHECK(battery_service_step(&s, &junk[i], true, t) == BAT_ACT_NONE);
        }
    }
    CHECK(!s.have && !s.off_due && !s.off_sent && s.model.level == BAT_OK);
    CHECK(s.rejected == 70 && s.io_errors == 0 && s.gauge == BAT_GAUGE_LOST);
    CHECK(s.last_reject == BAT_REGS_VERSION);
}

static void warn_needs_the_dwell_and_clears(void) {
    bat_svc_t  s;
    bat_regs_t low = good(3380), high = good(3600);
    present(&s);
    CHECK(battery_service_step(&s, &low, false, 0) == BAT_ACT_NONE);
    CHECK(battery_service_step(&s, &low, false, 5000) == BAT_ACT_NONE);
    CHECK(battery_service_step(&s, &low, false, 10000) == BAT_ACT_WARN);
    CHECK(battery_service_step(&s, &low, false, 15000) == BAT_ACT_NONE); // once
    CHECK(battery_service_step(&s, &high, false, 20000) == BAT_ACT_NONE);
    CHECK(battery_service_step(&s, &high, false, 30000) == BAT_ACT_WARN_CLEAR);
}

static void a_failed_poll_restarts_the_dwell(void) {
    bat_svc_t  s;
    bat_regs_t low = good(3250);
    present(&s);
    uint32_t t = 0;
    for (int i = 0; i < 20; i++, t += 5000) {
        // low, failed, low, failed ...: never 10 s of consecutive readings
        CHECK(battery_service_step(&s, (i % 2) ? NULL : &low, true, t) == BAT_ACT_NONE);
    }
    CHECK(s.model.level == BAT_OK && !s.off_due);
}

static void off_is_reported_but_not_requested_by_default(void) {
    bat_svc_t  s;
    bat_regs_t dead = good(3250);
    present(&s);
    bat_action_t seen = BAT_ACT_NONE;
    for (uint32_t t = 0; t <= 60000; t += 5000) {
        bat_action_t a = battery_service_step(&s, &dead, false, t);
        CHECK(a != BAT_ACT_OFF);
        if (a != BAT_ACT_NONE) {
            CHECK(seen == BAT_ACT_NONE); // one action in all
            seen = a;
        }
    }
    CHECK(seen == BAT_ACT_WARN && s.off_due && !s.off_sent && s.model.level == BAT_OFF);
}

static void off_with_auto_off_once(void) {
    bat_svc_t  s;
    bat_regs_t dead = good(3250);
    present(&s);
    int offs = 0;
    for (uint32_t t = 0; t <= 60000; t += 5000) {
        offs += battery_service_step(&s, &dead, true, t) == BAT_ACT_OFF;
    }
    CHECK(offs == 1 && s.off_sent);
    battery_service_probed(&s, BAT_PROBE_FOUND); // a re-probe starts the model over, the one OFF per boot stays spent
    for (uint32_t t = 100000; t <= 160000; t += 5000) {
        CHECK(battery_service_step(&s, &dead, true, t) != BAT_ACT_OFF);
    }
}

static void charging_holds_off_the_warning(void) {
    bat_svc_t  s;
    bat_regs_t low = good(3250);
    low.crate      = 100; // +20.8 %/h
    present(&s);
    for (uint32_t t = 0; t <= 60000; t += 5000) {
        CHECK(battery_service_step(&s, &low, true, t) == BAT_ACT_NONE);
    }
    CHECK(s.model.level == BAT_OK && !s.off_due);
}

static void names(void) {
    CHECK(strcmp(battery_gauge_name(BAT_GAUGE_ABSENT), "absent") == 0);
    CHECK(strcmp(battery_gauge_name(BAT_GAUGE_LINES_LOW), "lines low") == 0);
    CHECK(strcmp(battery_gauge_name((bat_gauge_t)99), "?") == 0);
    CHECK(strcmp(battery_probe_name(BAT_PROBE_LINES_LOW), "SDA/SCL held low") == 0);
    CHECK(strcmp(battery_probe_name(BAT_PROBE_NOTHING), "nothing at 0x36") == 0);
    CHECK(strcmp(battery_probe_name((bat_probe_t)99), "?") == 0);
    CHECK(strcmp(battery_regs_verdict_name(BAT_REGS_CRATE), "bad rate") == 0);
    CHECK(strcmp(battery_regs_verdict_name((bat_regs_verdict_t)99), "?") == 0);
}

int main(void) {
    regs_check_limits();
    absent_does_nothing();
    probe_results_map_to_states();
    reading_reaches_the_model();
    failures_go_stale_then_lost();
    garbage_is_rejected_and_never_powers_off();
    warn_needs_the_dwell_and_clears();
    a_failed_poll_restarts_the_dwell();
    off_is_reported_but_not_requested_by_default();
    off_with_auto_off_once();
    charging_holds_off_the_warning();
    names();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("battery_svc_test: all passed\n");
    return 0;
}
