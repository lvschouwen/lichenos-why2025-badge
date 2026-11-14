#pragma once
// The duration argument of `fw c6log` (#458). Pure, host-tested in host_tests/c6_log_arg_test.c.
#include <stdbool.h>
#include <stdint.h>

#define C6_LOG_DEFAULT_MS 10000
#define C6_LOG_MAX_MS     60000

// args: "" (the default) or optional leading spaces and then decimal digits and nothing else (no sign, no suffix such as
// "10s", which used to run for 10 ms). True with 1..C6_LOG_MAX_MS in *ms; false leaves *ms alone.
bool c6_log_parse_ms(char const *args, uint32_t *ms);
