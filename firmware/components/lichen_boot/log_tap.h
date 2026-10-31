#pragma once

#include <stddef.h>

// Keeps the last console log lines for the boot screen (#66). The console output itself is unchanged.

#define LOG_TAP_LINES    8
#define LOG_TAP_LINE_LEN 96

// Call first in app_main. Allocates the ring (PSRAM) once and hooks esp_log.
void log_tap_start(void);
// Copies up to max of the last lines, newest last, ANSI colour codes stripped. Returns how many.
size_t log_tap_last(char lines[][LOG_TAP_LINE_LEN], size_t max);
