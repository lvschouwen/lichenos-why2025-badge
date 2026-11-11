#include "disp_state_fmt.h"

#include <stdio.h>
#include <stdlib.h>

// The value of a registry text: the token before the first space; "?" when none or not read
static void token(char const *raw, char *out, size_t cap) {
    size_t n = 0;
    while (raw && raw[n] && raw[n] != ' ' && n + 1 < cap) {
        out[n] = raw[n];
        n++;
    }
    out[n] = '\0';
    if (n == 0 || out[0] == '?') {
        snprintf(out, cap, "?");
    }
}

// psram.clk is in Hz; the line says MHz
static void mhz(char const *raw, char *out, size_t cap) {
    char v[16];
    token(raw, v, sizeof(v));
    char *end = NULL;
    long  hz  = strtol(v, &end, 10);
    if (v[0] == '?' || end == v || *end) {
        snprintf(out, cap, "?");
    } else {
        snprintf(out, cap, "%ld", (hz + 500000) / 1000000);
    }
}

static void num(int64_t v, char *out, size_t cap) {
    if (v < 0) {
        snprintf(out, cap, "?");
    } else {
        snprintf(out, cap, "%lld", (long long)v);
    }
}

static char const *yes_no(int v, char const *yes, char const *no) {
    return v < 0 ? "?" : v ? yes : no;
}

static char const *trial_name(int t) {
    static char const *const names[] = {"none", "armed", "running", "passed"};
    return t >= DS_TRIAL_NONE && t <= DS_TRIAL_PASSED ? names[t] : "?";
}

size_t ds_format(ds_info_t const *in, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    char clk[16], want[24], pclk[16], refresh[16], bl[16], kbd[16], boot[16], idle[16], over[16], freq[16];
    char rq[24], kq[24], rs[24], up[24];
    mhz(in->psram_clk_raw, clk, sizeof(clk));
    num(in->psram_want_mhz, want, sizeof(want));
    token(in->pclk_raw, pclk, sizeof(pclk));
    token(in->refresh_raw, refresh, sizeof(refresh));
    token(in->bl_raw, bl, sizeof(bl));
    token(in->kbd_raw, kbd, sizeof(kbd));
    token(in->bl_boot_raw, boot, sizeof(boot));
    token(in->bl_idle_raw, idle, sizeof(idle));
    token(in->bl_override_raw, over, sizeof(over));
    token(in->bl_freq_raw, freq, sizeof(freq));
    num(in->bl_req, rq, sizeof(rq));
    num(in->kbd_req, kq, sizeof(kq));
    num(in->c6_restarts, rs, sizeof(rs));
    num(in->uptime_s, up, sizeof(up));
    int n = snprintf(out, cap,
                     "disp state: boot %s %s | psram %s want %s trial %s failed %s | pclk %s refresh %s | "
                     "bl %s/%s kbd %s/%s boot %s idle %s override %s freq %s | c6 %s restarts %s | uptime %s",
                     in->reset && in->reset[0] ? in->reset : "?", in->fw && in->fw[0] ? in->fw : "fw-?", clk, want,
                     trial_name(in->trial), yes_no(in->failed, "yes", "no"), pclk, refresh, rq, bl, kq, kbd, boot,
                     idle, over, freq, yes_no(in->c6_up, "up", "down"), rs, up);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
