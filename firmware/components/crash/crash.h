#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Crash records (#74). Call once at boot after the storage drive is mounted: if the last reset was a crash it is
// appended to /storage/crash/log.txt (the last 20 are kept) and counted in NVS. Also adds the "fw crash" commands.
void crash_check(void);

// This boot follows a crash (for the boot screen)
bool crash_recovered(void);

// The crash log for the Crash log app's API (lichen_crashlog.h, #72); any task. The log's text, NUL terminated
// (cap - 1 bytes at most); 0 when there is none.
size_t crash_log_load(char *buf, size_t cap);
// Replaces the log with text (written next to it, then swapped in); len 0 removes it
bool crash_log_store(char const *text, size_t len);
// What "fw crash clear" does: the log, the NVS crash counter and dump marker, the core dump. False when a part failed.
bool crash_clear_all(void);
// The NVS crash counter: crashes recorded since the last clear
uint32_t crash_count_since_clear(void);
// Changes whenever the log is stored or cleared, so a cached view knows to read it again
uint32_t crash_log_generation(void);
