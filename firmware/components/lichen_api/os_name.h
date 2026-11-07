#pragma once

#include <stddef.h>

// The OS name shown on the boot screen and in About (#236). The major version is the v2 marker. Pure, host-tested in
// host_tests/os_name_test.c.
char const *lichen_os_name(void); // "Lichen OS 2"
// "Lichen OS 2 (fw-abc1234)" for About's version tile, written into buf (always terminated, cut if cap is small).
char const *lichen_os_version_line(char const *fw_version, char *buf, size_t cap);
