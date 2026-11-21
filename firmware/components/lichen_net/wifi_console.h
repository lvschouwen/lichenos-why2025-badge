#pragma once

#include "wifi_cmd.h"

// The `fw wifi` subcommands that only use the service's public API (split out of wifi_service.c, #496): handlers for the
// command table, and the IPv6 status line. Console task only.
void wifi_console_scan(wifi_cmd_t const *c);
void wifi_console_join(wifi_cmd_t const *c);
void wifi_console_join_usage(wifi_cmd_t const *c);
// The IPv6 line of `fw wifi` (#497): the address to use, or why there is none
void wifi_console_ip6(void);
// ps, tx, proto and bw (#311, #502): each sets through the service's setters and echoes the applied read-back
void wifi_console_ps(wifi_cmd_t const *c);
void wifi_console_ps_usage(wifi_cmd_t const *c);
void wifi_console_tx(wifi_cmd_t const *c);
void wifi_console_tx_usage(wifi_cmd_t const *c);
void wifi_console_proto(wifi_cmd_t const *c);
void wifi_console_proto_usage(wifi_cmd_t const *c);
void wifi_console_bw(wifi_cmd_t const *c);
void wifi_console_stats(wifi_cmd_t const *c); // `fw wifi stats`
void wifi_console_status(wifi_cmd_t const *c); // `fw wifi`
