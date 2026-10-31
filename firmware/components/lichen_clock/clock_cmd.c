// "fw clock": which clock the RTC slow clock runs from and how far it is off (#63)
#include "clock_local.h"
#include "fw_console.h"
#include "lichen_clock.h"

#include "esp_log.h"
#include "esp_sntp.h"

#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "soc/rtc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t rtc0_us, timer0_us;
static int64_t carried_s;      // time(NULL) when "fw clock" was registered (early boot): what the reset carried over
static int64_t carried_up_ms;  // uptime then
static int64_t rtc_boot_us;    // esp_clk_rtc_time() then

static char const *src_name(soc_rtc_slow_clk_src_t s) {
    switch (s) {
        case SOC_RTC_SLOW_CLK_SRC_RC_SLOW: return "RC_SLOW (internal RC)";
        case SOC_RTC_SLOW_CLK_SRC_XTAL32K: return "XTAL32K (external crystal)";
        case SOC_RTC_SLOW_CLK_SRC_RC32K: return "RC32K";
        default: return "?";
    }
}

void clock_store_init(void); // clock_store.c

// The local time and the zone's abbreviation: "2026-10-31 12:00 CET"
static void local_now(char *out, size_t cap) {
    time_t    now = time(NULL);
    struct tm l;
    if (!localtime_r(&now, &l) || !strftime(out, cap, "%Y-%m-%d %H:%M %Z", &l)) {
        snprintf(out, cap, "?");
    }
}

static void reply_refused(clock_verdict_t v) {
    clock_sync_info_t i = clock_sync_info();
    if (v == CLOCK_OUTRANKED && i.source == CLOCK_SRC_SNTP) {
        console_reply("clock set: refused, SNTP set the clock %lld min ago (--force on the bench)",
                      (long long)(clock_source_age_s() / 60));
    } else {
        console_reply("clock set: refused, %s", clock_verdict_text(v));
    }
}

// "fw clock set [--force] <ISO8601>" (#64): the same path as Settings > Time > Set date & time
static void cmd_set(char const *a) {
    bool force = strncmp(a, "--force", 7) == 0 && (a[7] == ' ' || a[7] == '\0');
    if (force) {
        a += 7;
    }
    int64_t       t;
    clock_local_t how;
    if (!clock_parse_iso(a, &t, &how)) {
        console_reply("clock set: use YYYY-MM-DDTHH:MM[:SS] and Z, +HH:MM or nothing (local time)");
        return;
    }
    if (force) {
        ESP_LOGW("clock", "fw clock set --force: judged as if SNTP had not set the clock this boot (bench only)");
    }
    int64_t         before = (int64_t)time(NULL);
    struct timeval  tv     = {.tv_sec = (time_t)t};
    clock_verdict_t v      = clock_set_manual(&tv, force);
    if (!clock_accepted(v)) {
        reply_refused(v);
        return;
    }
    char now[40];
    local_now(now, sizeof(now));
    console_reply("clock set: %s, by hand (moved %+lld s)", now, (long long)(t - before));
    char const *hints[] = {clock_local_hint(how), clock_back_hint(before, t)};
    for (size_t k = 0; k < sizeof(hints) / sizeof(hints[0]); k++) {
        if (hints[k]) {
            console_reply("note: %s", hints[k]);
        }
    }
}

// "fw clock sync" (#64): lwIP's SNTP client starts over and asks now instead of at its next hourly poll
static void cmd_sync(void) {
    if (esp_sntp_restart()) {
        console_reply("clock sync: SNTP asked again, see fw clock in a few seconds");
    } else {
        console_reply("clock sync: SNTP is not running (no Wi-Fi yet?)");
    }
}

static void cmd_status(void);

static void cmd_clock(char const *args) {
    if (strncmp(args, "set", 3) == 0 && (args[3] == ' ' || args[3] == '\0')) {
        cmd_set(args[3] ? args + 4 : args + 3);
    } else if (strcmp(args, "sync") == 0) {
        cmd_sync();
    } else if (!*args) {
        cmd_status();
    } else {
        console_reply("clock: use fw clock, fw clock set [--force] <ISO8601>, fw clock sync");
    }
}

static void cmd_status(void) {
    soc_rtc_slow_clk_src_t src = rtc_clk_slow_src_get();
    uint32_t               cal = rtc_clk_cal(CLK_CAL_RTC_SLOW, 1024); // microseconds per tick, Q19
    double                 hz  = cal ? (double)(1 << RTC_CLK_CAL_FRACT) * 1e6 / (double)cal : 0;
    double                 boot_hz = (double)(1 << RTC_CLK_CAL_FRACT) * 1e6 / (double)esp_clk_slowclk_cal_get();
    console_reply("slow clock source: %s", src_name(src));
    console_reply("slow clock measured: %.1f Hz, at boot: %.1f Hz", hz, boot_hz);

    int64_t drtc = (int64_t)esp_clk_rtc_time() - rtc0_us;
    int64_t dtim = esp_timer_get_time() - timer0_us;
    if (dtim > 10 * 1000000) {
        console_reply("RTC vs esp_timer since boot: %+.1f ppm over %lld s", (double)(drtc - dtim) * 1e6 / (double)dtim,
                      (long long)(dtim / 1000000));
    } else {
        console_reply("RTC vs esp_timer since boot: too soon (%lld s)", (long long)(dtim / 1000000));
    }

    clock_sync_info_t i = clock_sync_info();
    console_reply("carried at boot: %lld s (clock after %lld ms uptime), RTC counter %.1f s", (long long)carried_s,
                  (long long)carried_up_ms, (double)rtc_boot_us / 1e6);
    console_reply("clock: %s, build epoch %lld", clock_is_set() ? "set" : "not set", (long long)clock_build_epoch());
    char from[64];
    clock_source_text(from, sizeof(from)); // "carried over the restart, was SNTP" after a warm reset (#64)
    if (i.source > CLOCK_SRC_RTC) { // #83
        console_reply("source: %s (set at %lld s uptime)", from, (long long)(i.source_mono_ms / 1000));
    } else {
        console_reply("source: %s", from);
    }
    if (i.synced) {
        double ppm = i.step_mono_ms ? (double)i.step_ms * 1e6 / (double)i.step_mono_ms : 0;
        console_reply("now %lld; SNTP answer %lld at %lld ms uptime, clock before it %lld", (long long)time(NULL),
                      (long long)i.last_good_s, (long long)i.step_mono_ms, (long long)(i.last_good_s - i.step_ms / 1000));
        console_reply("SNTP: synced %lld s ago; last step %+lld ms at %lld s uptime (%+.0f ppm)",
                      (long long)(time(NULL) - i.last_good_s), (long long)i.step_ms, (long long)(i.step_mono_ms / 1000),
                      ppm);
    } else {
        console_reply("SNTP: not synced since boot");
    }
    if (i.refused) {
        console_reply("SNTP: %u answer(s) refused, last: %s", (unsigned)i.refused, clock_verdict_text(i.last_verdict));
    }
}

void clock_console_register(void) {
    rtc0_us   = (int64_t)esp_clk_rtc_time();
    timer0_us = esp_timer_get_time();
    carried_s     = (int64_t)time(NULL);
    carried_up_ms = timer0_us / 1000;
    rtc_boot_us   = rtc0_us;
    clock_store_init();
    console_register("clock", "clock [set [--force] <ISO8601> | sync] (source, drift, SNTP; set by hand)", cmd_clock);
}
