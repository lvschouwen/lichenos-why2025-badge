#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Text for the crash log and the core dump transfer (#74). Pure logic, no ESP headers.

// The reset reasons that matter here (mirrors ESP-IDF's esp_reset_reason_t without its headers)
typedef enum { CRASH_RST_OTHER, CRASH_RST_PANIC, CRASH_RST_INT_WDT, CRASH_RST_TASK_WDT, CRASH_RST_WDT, CRASH_RST_BROWNOUT } crash_reset_t;

// Whether a reset is a crash worth recording. A bare RTC watchdog is also what a USB/RTS reset looks like, so it only
// counts when a core dump written by this very reset exists (fresh_dump)
bool crash_is_crash(crash_reset_t reason, bool fresh_dump);

// "<when> <version> <reason> task=<task> pc=0x%08x elf=<sha> bt=0x%08x,0x%08x,..." (bt left out when depth is 0).
// version is the running image's; elf_sha is the crashed image's ELF SHA-256 prefix from the core dump (they differ
// after a rollback), left out when NULL or not hex. The task name is cut to [A-Za-z0-9_-], 16 characters.
// Returns the length written (always NUL terminated).
size_t crash_record_format(char *out, size_t cap, char const *when, char const *version, char const *elf_sha, char const *reason,
                           char const *task, uint32_t pc, uint32_t const *backtrace, size_t depth);

// Copies the leading hex digits of in (lower case) into out, cap-1 at most; returns their count (0 for NULL)
size_t crash_elf_sha_clean(char *out, size_t cap, char const *in);

// Keeps the last keep_lines lines of text; returns the new length
size_t crash_log_trim(char *text, size_t len, size_t keep_lines);

// Base64 with a '\n' after every line_chars characters (none after the last); NUL terminated.
// Returns the length, or 0 if cap is too small.
size_t crash_b64_lines(uint8_t const *in, size_t len, char *out, size_t cap, size_t line_chars);

#ifdef __cplusplus
}
#endif
