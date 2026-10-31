#include "lichen_clock.h"

#include <pthread.h>
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

// Log-only for now: a foreign caller is named by its return address and still sets the clock. The App API boundary
// work (#84, T9) makes this refuse.
int __wrap_settimeofday(struct timeval const *tv, struct timezone const *tz);
int __wrap_settimeofday(struct timeval const *tv, struct timezone const *tz) {
    ESP_LOGW("clock", "settimeofday(%lld) outside lichen_clock, from %p", tv ? (long long)tv->tv_sec : -1LL,
             __builtin_return_address(0));
    return __real_settimeofday(tv, tz);
}
#else
// Host tests: never touch the host's clock (as root it would succeed)
static int set_clock(struct timeval const *tv) {
    (void)tv;
    return -1;
}
#endif

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
    if (clock_accepted(v) && set_clock(tv) == 0) {
        info.source         = src;
        info.was            = CLOCK_SRC_NONE; // only a carried clock has a "was"
        info.source_mono_ms = now_ms;
        if (src == CLOCK_SRC_SNTP) {
            info.step_ms = ((int64_t)tv->tv_sec - (int64_t)cur.tv_sec) * 1000 +
                           ((int64_t)tv->tv_usec - cur.tv_usec) / 1000;
            info.step_mono_ms      = now_ms;
            info.synced            = true;
            info.last_good_s       = (int64_t)tv->tv_sec;
            info.last_good_mono_ms = now_ms;
        }
    }
    bool due = record_due_locked();
    pthread_mutex_unlock(&lock);
    if (due) {
        store_save(src); // outside the lock: an NVS write is slow
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

clock_sync_info_t clock_sync_info(void) {
    pthread_mutex_lock(&lock);
    note_boot_locked();
    clock_sync_info_t i = info;
    pthread_mutex_unlock(&lock);
    return i;
}
