#include "c6_watch.h"

#include <stdio.h>
#include <string.h>

// Wait before attempt n+1 (index n-1); the first attempt runs at once (#14)
static uint32_t const BACKOFF_MS[C6W_ATTEMPTS - 1] = {2000, 10000};

void c6_watch_init(c6_watch_t *w, bool trial_image, bool fallback_used) {
    memset(w, 0, sizeof(*w));
    w->state         = C6W_UP;
    w->trial_image   = trial_image;
    w->fallback_used = fallback_used;
}

// Wrap-safe "now is at or past t"
static bool due(uint32_t now_ms, uint32_t t) {
    return (int32_t)(now_ms - t) >= 0;
}

static unsigned begin_outage(c6_watch_t *w, c6w_source_t src, uint32_t now_ms) {
    w->state        = C6W_ATTACHING;
    w->source       = src;
    w->attempt      = 1;
    w->down_ms      = now_ms;
    w->soft_fails   = 0;
    w->probing      = false;
    // fw c6 reset gets the blame only for the outage that follows it closely, and only once
    w->manual_cause = w->manual_pending && now_ms - w->manual_ms <= C6W_MANUAL_WINDOW_MS;
    if (w->manual_cause) {
        w->manual_lead_ms = now_ms - w->manual_ms;
    }
    w->manual_pending = false;
    return C6W_ACT_DOWN_HOOKS | C6W_ACT_REATTACH;
}

unsigned c6_watch_hard(c6_watch_t *w, c6w_source_t src, uint32_t now_ms) {
    return w->state == C6W_UP ? begin_outage(w, src, now_ms) : 0;
}

unsigned c6_watch_soft_fail(c6_watch_t *w, uint32_t now_ms) {
    (void)now_ms;
    if (w->state != C6W_UP || w->probing) {
        return 0;
    }
    if (++w->soft_fails < C6W_SOFT_LIMIT) {
        return 0;
    }
    w->probing = true;
    return C6W_ACT_PROBE;
}

void c6_watch_soft_ok(c6_watch_t *w) {
    w->soft_fails = 0;
}

unsigned c6_watch_probe(c6_watch_t *w, bool answered, uint32_t now_ms) {
    if (w->state != C6W_UP || !w->probing) {
        return 0;
    }
    w->probing    = false;
    w->soft_fails = 0;
    return answered ? 0 : begin_outage(w, C6W_SRC_LORA, now_ms);
}

void c6_watch_manual_reset(c6_watch_t *w, uint32_t now_ms) {
    w->manual_pending = true;
    w->manual_ms      = now_ms;
}

static unsigned start_attempt(c6_watch_t *w) {
    w->state = C6W_ATTACHING;
    if (w->attempt < UINT8_MAX) {
        w->attempt++;
    }
    return C6W_ACT_REATTACH;
}

unsigned c6_watch_reattach(c6_watch_t *w, uint32_t now_ms) {
    switch (w->state) {
    case C6W_UP:
        return begin_outage(w, C6W_SRC_REATTACH, now_ms);
    case C6W_DOWN:
    case C6W_GAVE_UP:
        return start_attempt(w);
    default:
        return 0;
    }
}

unsigned c6_watch_tick(c6_watch_t *w, uint32_t now_ms) {
    if ((w->state == C6W_DOWN || w->state == C6W_GAVE_UP) && due(now_ms, w->next_ms)) {
        return start_attempt(w);
    }
    return 0;
}

// "1.2 s": tenths, rounded down
static int tenths(char *out, size_t cap, uint32_t ms) {
    return snprintf(out, cap, "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
}

static char const *source_text(c6w_source_t src) {
    switch (src) {
    case C6W_SRC_TRANSPORT:
        return "transport failure";
    case C6W_SRC_KEYBOARD:
        return "keyboard chip reset";
    case C6W_SRC_LORA:
        return "LoRa sends failing";
    case C6W_SRC_REATTACH:
        return "fw c6 reattach";
    default:
        return "unknown";
    }
}

// "<source>[ 0.4 s after fw c6 reset]"
static size_t cause_text(c6_watch_t const *w, char *out, size_t cap) {
    char lead[16] = "";
    if (w->manual_cause) {
        tenths(lead, sizeof(lead), w->manual_lead_ms);
    }
    int n = snprintf(out, cap, "%s%s%s%s", source_text(w->source), w->manual_cause ? " " : "", lead,
                     w->manual_cause ? " after fw c6 reset" : "");
    return n < 0 ? 0 : (size_t)n;
}

// Its uptime tells a C6 that rebooted during the outage from one that only lost the SDIO transport
static void set_reason(c6_watch_t *w, uint32_t c6_uptime_ms) {
    char cause[64], back[16], c6[40];
    cause_text(w, cause, sizeof(cause));
    tenths(back, sizeof(back), w->last_recovery_ms);
    if (c6_uptime_ms == C6W_UPTIME_UNKNOWN) {
        // Every re-attach pulses EN (CP_RESET_STRATEGY_ALWAYS), so the uptime can only come from before it. Without
        // one, only a keyboard reset or our own pulse proves that EN went low.
        bool en_low = w->source == C6W_SRC_KEYBOARD || w->manual_cause;
        snprintf(c6, sizeof(c6), en_low ? "C6 was reset (EN low)" : "C6 silent before the re-attach");
    } else if (c6_uptime_ms <= w->last_recovery_ms + C6W_DETECT_SLACK_MS) {
        snprintf(c6, sizeof(c6), "C6 rebooted (uptime %u s)", (unsigned)(c6_uptime_ms / 1000));
    } else {
        snprintf(c6, sizeof(c6), "transport only (C6 uptime %u s)", (unsigned)(c6_uptime_ms / 1000));
    }
    snprintf(w->reason, sizeof(w->reason), "%s, back in %s, %s", cause, back, c6);
}

static unsigned attach_failed(c6_watch_t *w, uint32_t now_ms) {
    if (w->attempt < C6W_ATTEMPTS) {
        w->state   = C6W_DOWN;
        w->next_ms = now_ms + BACKOFF_MS[w->attempt - 1];
        return 0;
    }
    bool first = w->attempt == C6W_ATTEMPTS; // the slow retries do not give up again
    w->state   = C6W_GAVE_UP;
    w->next_ms = now_ms + C6W_RETRY_SLOW_MS;
    if (!first) {
        return 0;
    }
    unsigned act = C6W_ACT_GIVE_UP;
    // At most once (the flag is kept in NVS by the caller), never on a trial image: a restart there rolls it back
    if (!w->fallback_used && !w->trial_image) {
        w->fallback_used = true;
        act |= C6W_ACT_FALLBACK_RESTART;
    }
    return act;
}

unsigned c6_watch_attached(c6_watch_t *w, bool ok, uint32_t c6_uptime_ms, uint32_t now_ms) {
    if (w->state != C6W_ATTACHING) {
        return 0;
    }
    if (!ok) {
        return attach_failed(w, now_ms);
    }
    w->state            = C6W_UP;
    w->restarts++;
    w->last_recovery_ms = now_ms - w->down_ms;
    w->up_ms            = now_ms;
    set_reason(w, c6_uptime_ms);
    w->manual_cause = false;
    return C6W_ACT_UP_HOOKS | C6W_ACT_NOTE;
}

void c6_watch_state_text(c6w_state_t state, uint8_t attempt, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    bool slow = state == C6W_GAVE_UP || attempt > C6W_ATTEMPTS;
    if (state == C6W_UP) {
        snprintf(out, cap, "up");
    } else if (slow) {
        snprintf(out, cap, "down (no answer)%s", state == C6W_ATTACHING ? ", retrying" : "");
    } else {
        // While waiting, the attempt shown is the one that comes next
        unsigned n = attempt + (state == C6W_DOWN ? 1u : 0u);
        snprintf(out, cap, "recovering %u/%u", n, (unsigned)C6W_ATTEMPTS);
    }
}

static void state_text(c6_watch_t const *w, char *out, size_t cap) {
    c6_watch_state_text(w->state, w->attempt, out, cap);
}

bool c6_watch_stale(c6_watch_t const *w, c6w_source_t src, uint32_t raised_ms) {
    if (w->restarts == 0) {
        return false; // no outage has ended yet
    }
    if (!due(raised_ms, w->up_ms)) {
        return true; // raised before the C6 came back
    }
    return src == C6W_SRC_KEYBOARD && raised_ms - w->up_ms < C6W_KEYBOARD_GRACE_MS;
}

c6w_fallback_t c6_watch_fallback_plan(bool updating, bool can_ask) {
    return updating || !can_ask ? C6W_FALLBACK_RESTART_NOW : C6W_FALLBACK_ASK;
}

bool c6_watch_rt_flag_clear_due(bool flag_set, c6w_state_t state, uint32_t uptime_ms) {
    return flag_set && state == C6W_UP && uptime_ms >= C6W_RT_FLAG_CLEAR_MS;
}

size_t c6_watch_line(c6_watch_t const *w, uint32_t now_ms, char *out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    char state[40], detail[C6W_REASON_MAX + 16] = "";
    state_text(w, state, sizeof(state));
    if (w->state != C6W_UP) {
        char cause[64], down[16];
        cause_text(w, cause, sizeof(cause));
        tenths(down, sizeof(down), now_ms - w->down_ms);
        snprintf(detail, sizeof(detail), " | now: %s, down %s", cause, down);
    } else if (w->reason[0]) {
        snprintf(detail, sizeof(detail), " | last: %s", w->reason);
    }
    int n = snprintf(out, cap, "c6: %s | restarts %u%s", state, (unsigned)w->restarts, detail);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
