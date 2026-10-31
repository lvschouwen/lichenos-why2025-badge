// "fw clock": which clock the RTC slow clock runs from and how far it is off (#63)
#include "fw_console.h"
#include "lichen_clock.h"

#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "soc/rtc.h"

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

static void cmd_clock(char const *args) {
    (void)args;
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
    console_register("clock", "clock (RTC slow clock source and drift, SNTP steps)", cmd_clock);
}
