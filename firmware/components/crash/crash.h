#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Crash records (#74). Call once at boot after the storage drive is mounted: if the last reset was a crash it is
// appended to /storage/crash/log.txt (the last 20 are kept) and counted in NVS. Also adds the "fw crash" commands.
void crash_check(void);
// The same in two parts for the early boot screen (#39). Early: no /storage, no image hash; decides crash_recovered()
// and adds "fw crash". Late: after /storage is mounted; hashes the running image, writes the crash record.
void crash_check_early(void);
void crash_check_late(void);

// The core dump on flash is new since the last recorded crash (what tells a real watchdog reset, ESP_RST_WDT, from the
// USB-serial reset that reports the same reason on this board). Needs NVS up; read once per boot, writes nothing (#353)
bool crash_dump_fresh(void);
// What became of this boot's crash record: PENDING until crash_check_late ran on a crash (and on a boot that is no
// crash), then WRITTEN, or FAILED when /storage refused it (logged at W). Any task
typedef enum { CRASH_RECORD_PENDING, CRASH_RECORD_WRITTEN, CRASH_RECORD_FAILED } crash_record_t;
crash_record_t crash_record_state(void);
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
// The same as last read or written by this component, without touching NVS (fw soak, #409); false when it could not
// be read at boot. Any task.
bool crash_count_cached(uint32_t *count);
// The boot counter (boot_count.c, #409): +1 in NVS once per boot (crash_check_early calls it), so it grows across
// every kind of reset and a power cut. *count is this boot's number; false when it could not be read and saved this
// boot (the number then means nothing). Any task, no I/O.
void boot_count_bump(void);
bool boot_count(uint32_t *count);
// Held around a read-modify-write of the log (load, change, store); store and clear take it too (recursive)
void crash_log_lock(void);
void crash_log_unlock(void);
// Changes whenever the log is stored or cleared, so a cached view knows to read it again
uint32_t crash_log_generation(void);
