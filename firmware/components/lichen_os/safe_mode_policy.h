#pragma once
// Crash attribution and safe mode (#232), the pure part: the record the panic handler leaves for the next boot, the
// boot-loop breaker (an app with 2 attributed crashes in the last 3 boots gets its background part switched off) and
// the list of apps whose background is off. ESP-free, host-tested in host_tests/safe_mode_policy_test.c; safe_mode.c
// keeps the record in LP RAM and the list in the settings service.
//
// The record lives in RTC no-init memory (LP RAM): #330 measured it to survive esp_restart and a panic and to be lost
// at a power-off and the CH340 EN reset. PSRAM no-init (#343) also survives the EN reset, but the panic handler's
// writes there go through the cache and may never reach PSRAM before the chip resets. A lost record only forgets the
// window: nobody is penalised (fail open). It is noise after a power-on, so nothing in it is used before
// safe_mode_valid said yes.
//
// The record functions (safe_mode_rec.c: id_ok, sum, seal, valid, reset, ensure, note_panic) run in the panic handler:
// they sit in internal RAM (linker.lf), allocate, lock and log nothing and call no libc. The rest (safe_mode_policy.c)
// runs at boot and on the console.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFE_MODE_MAGIC  0x4C534D31u // "LSM1"
#define SAFE_MODE_ID_CAP 14          // an app id is 1..13 of a-z 0-9 _ (app_ns.h), with the NUL
#define SAFE_MODE_WINDOW 3           // boots looked at
#define SAFE_MODE_LIMIT  2           // crashes of one app in the window that switch its background off

// A window entry: what the boot before it died of
#define SAFE_MODE_CLEAN  0u // a clean end (no crash)
#define SAFE_MODE_NOBODY 1u // a crash no app is blamed for (an OS or IDF task, an ISR, a brownout, a lockup)
// anything else: safe_mode_hash of the app the crash is attributed to

#define SAFE_MODE_F_PANIC 0x1u // the panic handler ran and wrote crashed[] ("" = nobody)
#define SAFE_MODE_F_REARM 0x2u // this boot ends in the PSRAM 120 MHz re-arm restart (#353): the next boot is its tail

typedef struct {
    uint32_t magic;
    uint32_t flags;
    char     crashed[SAFE_MODE_ID_CAP];
    uint16_t pad;
    uint32_t window[SAFE_MODE_WINDOW]; // [0] the newest boot
    uint32_t check;                    // safe_mode_sum over everything before it
} safe_mode_rec_t;
_Static_assert(sizeof(safe_mode_rec_t) <= 64, "the record is at most 64 B of LP RAM (#232)");

// A hash of an app id that is never SAFE_MODE_CLEAN or SAFE_MODE_NOBODY; ids are compared by it across boots (an OTA
// may register the apps in another order, so a table index would not do)
uint32_t safe_mode_hash(char const *id);
uint32_t safe_mode_sum(safe_mode_rec_t const *r);
bool     safe_mode_valid(safe_mode_rec_t const *r);
// The record emptied (an empty window) and sealed
void     safe_mode_reset(safe_mode_rec_t *r);
// The magic and checksum set after a change; a record that is not valid reset first
void     safe_mode_seal(safe_mode_rec_t *r);
void     safe_mode_ensure(safe_mode_rec_t *r);

// The panic handler: the faulting task ran for app id (NULL or "" for nobody). A record that is not valid is reset
// first. Only a well-formed id (a-z 0-9 _, 1..13) is kept; anything else counts as nobody.
void safe_mode_note_panic(safe_mode_rec_t *r, char const *id);
// The PSRAM re-arm restart is about to happen (restarting true), or did not after all (false)
void safe_mode_note_rearm(safe_mode_rec_t *r, bool restarting);

typedef struct {
    bool     counted;                  // this boot took a place in the window (false: the re-arm restart's tail)
    bool     crash;                    // the boot before ended in a crash
    char     app[SAFE_MODE_ID_CAP];    // the app it is attributed to; "" for nobody (or no crash)
    unsigned app_crashes;              // that app's crashes in the window, this one included
    bool     limit_reached;            // app_crashes reached SAFE_MODE_LIMIT (safe_mode_switch_off decides)
    bool     degraded;                 // a crash is in the window: keep background work cautious (#280)
} safe_mode_boot_t;

// Once per boot, early: crashed is this boot's crash verdict (crash.c's crash_recovered). Consumes the panic and
// re-arm marks, moves the window and seals the record.
safe_mode_boot_t safe_mode_boot(safe_mode_rec_t *r, bool crashed);
// Whether the boot switches id's background part off now: id is the app b blames, it reached the limit and it may be
// switched off (may_switch_off: it has a background part and is not exempt, lichen_app_t.safe_mode_exempt). One that
// may not is only counted: its notice names the app and the crashes.
bool safe_mode_switch_off(safe_mode_boot_t const *b, char const *id, bool may_switch_off);
// may_switch_off for an app: it has a background part and is not exempt (lichen_app_t.safe_mode_exempt, MeshCore)
bool safe_mode_may_switch_off(bool has_background, bool exempt);
// Re-enabling an app forgets its crashes, so one more crash does not switch it off again at once
void safe_mode_forget(safe_mode_rec_t *r, char const *id);
// A crash in the window (what safe_mode_boot said about degraded; false for a record that is not valid)
bool safe_mode_degraded(safe_mode_rec_t const *r);

// An app id as the OS takes it: 1..13 of a-z 0-9 _
bool safe_mode_id_ok(char const *id);

// The list of apps whose background is off: ids separated by ','. Kept as a setting, so it is read back from flash:
// a malformed entry matches nothing and is dropped by the next add or remove. cap is at most SAFE_MODE_LIST_MAX.
#define SAFE_MODE_LIST_MAX 160 // 11 ids of the longest kind; an add that does not fit is refused
bool safe_mode_list_has(char const *list, char const *id);
// id added to list (cap bytes with the NUL); true when it is in afterwards. False for a bad id or no room.
bool safe_mode_list_add(char *list, size_t cap, char const *id);
// id taken out; true when it was in. The rest is rewritten well-formed.
bool safe_mode_list_remove(char *list, size_t cap, char const *id);

#ifdef __cplusplus
}
#endif
