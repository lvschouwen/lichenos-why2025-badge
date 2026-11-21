#pragma once

#include "wifi_cmd.h"

// The `fw wifi` subcommands that only use the service's public API (split out of wifi_service.c, #496): handlers for the
// command table, and the IPv6 status line. Console task only.
void wifi_console_scan(wifi_cmd_t const *c);
void wifi_console_join(wifi_cmd_t const *c);
void wifi_console_join_usage(wifi_cmd_t const *c);
// The IPv6 line of `fw wifi` (#497): the address to use, or why there is none
void wifi_console_ip6(void);
