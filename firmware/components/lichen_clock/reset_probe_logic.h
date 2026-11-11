#pragma once
// The reset-survival probe (#330 spike): which store keeps a word across which reset. Pure, host-tested
// (host_tests/reset_probe_test.c); reset_probe.c holds the three stores and the console command.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESET_PROBE_STORES 3 // rtc (LP RAM no-init), lp_store (LP_SYSTEM_REG_LP_STORE12..14), psram (ext no-init)

// One marker: a magic, the resets it survived since it was (re)armed, and a check over both. Zeros, a power-on's
// random contents and a half-written word all read as lost.
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t check;
} reset_probe_word_t;

reset_probe_word_t reset_probe_make(uint32_t count);
bool               reset_probe_ok(reset_probe_word_t const *w);
// What the boot writes back after reading w: the count plus one when w survived, a fresh 0 when it was lost
reset_probe_word_t reset_probe_next(reset_probe_word_t const *w);
// "reset <reason> (rom 0x<rom>), rtc ok, lp_store lost, psram ok (survived 3/0/3)"; ok[] and count[] in the order
// rtc, lp_store, psram, count[] the values read at boot (0 for a lost store). Returns out.
char const *reset_probe_line(char *out, size_t cap, char const *reason, unsigned rom, bool const ok[RESET_PROBE_STORES],
                             uint32_t const count[RESET_PROBE_STORES]);

#ifdef __cplusplus
}
#endif
