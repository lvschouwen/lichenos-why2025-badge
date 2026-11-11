#include "lichen_clock.h"

#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

static clock_sync_info_t info;
static clock_guard_t     guard;
static bool              boot_seen;
static pthread_mutex_t   lock = PTHREAD_MUTEX_INITIALIZER; // SNTP (lwIP task) and MeshCore propose concurrently
static clock_src_t       stored;            // the source last written to the record (#64)
static void (*store_save)(clock_src_t src); // writes the record; NULL until clock_set_store()
static clock_fix_info_t  fix;               // how SNTP answers were applied (#326)
static void (*saver)(void);                 // saves the clock record (clock_save.c); NULL until clock_set_saver()

int64_t clock_build_epoch(void) {
    return LICHEN_BUILD_EPOCH;
}

bool clock_is_set(void) {
    return clock_in_window((int64_t)time(NULL), LICHEN_BUILD_EPOCH);
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef ESP_PLATFORM
// Every other settimeofday() in the image lands in __wrap_settimeofday (linker --wrap, CMakeLists.txt): only this file
// calls the real one (#83, #84)
int        __real_settimeofday(struct timeval const *tv, struct timezone const *tz);
static int set_clock(struct timeval const *tv) {
    return __real_settimeofday(tv, NULL);
}

static int64_t tv_to_us(struct timeval const *tv) {
    return (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
}

// IDF 6.1 adjtime (esp_libc timekeeping.c): a new call replaces the slew in progress, keeping the part already applied,
// and hands back what was still pending; settimeofday stops it the same way. Offsets here stay below 1 s, well inside
// its +-2146 s limit and its 32-bit microsecond field.
static int slew_clock(int64_t offset_us, int64_t *pending_us) {
    struct timeval d = {.tv_sec = (time_t)(offset_us / 1000000), .tv_usec = (suseconds_t)(offset_us % 1000000)};
    struct timeval prev = {0};
    int            r    = adjtime(&d, &prev);
    *pending_us         = r == 0 ? tv_to_us(&prev) : 0;
    return r;
}

static int64_t slew_pending_us(void) {
    struct timeval prev = {0};
    return adjtime(NULL, &prev) == 0 ? tv_to_us(&prev) : 0;
}

// Refuses (#84 T9): a foreign caller may only write back the time the clock already shows (clock_resync_only). That is
// IDF's esp_sync_timekeeping_timers, which runs on every restart and before sleep and must keep working, or the time
// carried over a reset drifts. Anything that moves the clock goes through clock_propose(); the caller is named by its
// return address (look it up in build/badge_fw.map).
int __wrap_settimeofday(struct timeval const *tv, struct timezone const *tz);
int __wrap_settimeofday(struct timeval const *tv, struct timezone const *tz) {
    if (!tv) {
        return __real_settimeofday(tv, tz); // sets nothing
    }
    struct timeval cur;
    gettimeofday(&cur, NULL);
    if (clock_resync_only(cur.tv_sec, cur.tv_usec, tv->tv_sec, tv->tv_usec)) {
        return __real_settimeofday(tv, tz);
    }
    ESP_LOGE("clock", "settimeofday(%lld) refused: only lichen_clock sets the time (clock_propose), caller %p",
             (long long)tv->tv_sec, __builtin_return_address(0));
    errno = EPERM;
    return -1;
}
#else
// Host tests: never touch the host's clock (as root it would succeed)
static int set_clock(struct timeval const *tv) {
    (void)tv;
    return -1;
}

static int slew_clock(int64_t offset_us, int64_t *pending_us) {
    (void)offset_us;
    *pending_us = 0;
    return -1;
}

static int64_t slew_pending_us(void) {
    return 0;
}
#endif

// Under the lock: an SNTP answer within a second of the clock is slewed (#326), anything else stepped (a slew that
// fails is stepped too). *pending_us: what the slew before it still had to apply, dropped either way; *how: which.
static int apply_locked(clock_src_t src, struct timeval const *tv, struct timeval const *cur, int64_t *pending_us,
                        clock_fix_t *how) {
    int64_t off_us = ((int64_t)tv->tv_sec - (int64_t)cur->tv_sec) * 1000000 + ((int64_t)tv->tv_usec - cur->tv_usec);
    if (src == CLOCK_SRC_SNTP && clock_fix_for(off_us) == CLOCK_FIX_SLEW && slew_clock(off_us, pending_us) == 0) {
        *how = CLOCK_FIX_SLEW;
        return 0;
    }
    *pending_us = slew_pending_us();
    if (set_clock(tv) != 0) {
        return -1;
    }
    *how = CLOCK_FIX_STEP;
    return 0;
}

// The first look at the clock is still what the reset carried over: nothing else sets it (the wrap would say so)
static void note_boot_locked(void) {
    if (!boot_seen) {
        boot_seen   = true;
        info.source = clock_is_set() ? CLOCK_SRC_RTC : CLOCK_SRC_NONE;
    }
}

// Under the lock: the record needs writing when a source set the clock that is not the one it holds. Rare by
// construction: SNTP once per boot, an app once, by hand per change of source (#64).
static bool record_due_locked(void) {
    if (!store_save || info.source <= CLOCK_SRC_RTC || info.source == stored) {
        return false;
    }
    stored = info.source;
    return true;
}

void clock_set_store(clock_src_t was, void (*save)(clock_src_t src)) {
    pthread_mutex_lock(&lock);
    note_boot_locked();
    stored          = was;
    store_save      = save;
    info.was        = clock_src_was(info.source, was);
    bool        due = record_due_locked(); // a source that set the clock before the record was read
    clock_src_t src = info.source;
    pthread_mutex_unlock(&lock);
    if (due) {
        save(src);
    }
}

static clock_verdict_t propose(clock_src_t src, struct timeval const *tv, bool force) {
    pthread_mutex_lock(&lock);
    note_boot_locked();
    struct timeval cur;
    gettimeofday(&cur, NULL);
    int64_t         now_ms = mono_ms();
    clock_src_t     rank   = force ? clock_force_rank(info.source) : info.source;
    clock_verdict_t v      = clock_decide(rank, src, &guard, (int64_t)tv->tv_sec, (int64_t)cur.tv_sec,
                                          LICHEN_BUILD_EPOCH, now_ms / 1000);
    if (src == CLOCK_SRC_SNTP) {
        info.last_verdict = v;
        if (!clock_accepted(v)) {
            info.refused++;
        }
    }
    int64_t     pending_us = 0;
    clock_fix_t how        = CLOCK_FIX_NONE;
    bool        set        = clock_accepted(v) && apply_locked(src, tv, &cur, &pending_us, &how) == 0;
    if (set) {
        info.source         = src;
        info.was            = CLOCK_SRC_NONE; // only a carried clock has a "was"
        info.source_mono_ms = now_ms;
        if (src == CLOCK_SRC_SNTP) {
            info.step_ms = ((int64_t)tv->tv_sec - (int64_t)cur.tv_sec) * 1000 +
                           ((int64_t)tv->tv_usec - cur.tv_usec) / 1000;
            info.step_span_ms      = info.synced ? now_ms - info.last_good_mono_ms : 0;
            info.step_mono_ms      = now_ms;
            info.synced            = true;
            info.last_good_s       = (int64_t)tv->tv_sec;
            info.last_good_mono_ms = now_ms;
            fix.fix                = how;
            fix.pending_ms         = pending_us / 1000;
            fix.slewed += how == CLOCK_FIX_SLEW;
            fix.stepped += how == CLOCK_FIX_STEP;
        }
    }
    bool        due   = record_due_locked();
    clock_src_t saved = info.source;
    void (*save_now)(void) = set ? saver : NULL;
    pthread_mutex_unlock(&lock);
    if (due) {
        store_save(saved); // outside the lock: an NVS write is slow
    }
    if (save_now) {
        save_now(); // the new time at once, not up to a minute later (a manual set backwards, then a panic)
    }
    return v;
}

clock_verdict_t clock_propose(clock_src_t src, struct timeval const *tv) {
    return propose(src, tv, false);
}

clock_verdict_t clock_set_manual(struct timeval const *tv, bool force) {
    return propose(CLOCK_SRC_MANUAL, tv, force);
}

clock_verdict_t clock_offer_sntp(struct timeval const *tv) {
    return clock_propose(CLOCK_SRC_SNTP, tv);
}

bool clock_is_approximate(void) {
    clock_sync_info_t i = clock_sync_info();
    return clock_src_unverified(i.source, i.was);
}

int64_t clock_source_age_s(void) {
    return (mono_ms() - clock_sync_info().source_mono_ms) / 1000;
}

void clock_source_text(char *out, size_t cap) {
    clock_sync_info_t i = clock_sync_info();
    clock_src_line(i.source, i.was, (mono_ms() - i.source_mono_ms) / 1000, out, cap);
}

clock_fix_info_t clock_fix_info(void) {
    pthread_mutex_lock(&lock);
    clock_fix_info_t f = fix;
    pthread_mutex_unlock(&lock);
    return f;
}

void clock_set_saver(void (*save_now)(void)) {
    pthread_mutex_lock(&lock);
    saver = save_now;
    pthread_mutex_unlock(&lock);
}

bool clock_restore_saved(int64_t t) {
    pthread_mutex_lock(&lock);
    note_boot_locked();
    bool ok = false;
    if (info.source <= CLOCK_SRC_RTC && t > (int64_t)time(NULL)) {
        struct timeval tv = {.tv_sec = (time_t)t};
        ok                = set_clock(&tv) == 0;
    }
    if (ok) {
        info.source = CLOCK_SRC_RTC; // what a reset carries over, from the save instead of the RTC counter
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

clock_sync_info_t clock_sync_info(void) {
    pthread_mutex_lock(&lock);
    note_boot_locked();
    clock_sync_info_t i = info;
    pthread_mutex_unlock(&lock);
    return i;
}
