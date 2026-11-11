#pragma once

#include <stdbool.h>
#include <stddef.h>

// The argument line of `fw wifi ...` (#207), classified. Pure and host-tested (wifi_cmd_test.c); wifi_service.c keeps
// the handlers, because they use its private state.
//
// Matching is per subcommand, exactly as cmd_wifi always did it: most subcommands match as a whole string, `ps` and
// `tx` also with an argument, and ANY other line is "<ssid> <password>" (`wifi on hunter2` joins a network called
// "on"). console_model.c (wifi_public) mirrors these rules for the remote shell; change both together.
typedef enum {
    WIFI_CMD_STATUS,      // "" (also `fw net`)
    WIFI_CMD_SWITCH,      // on | off
    WIFI_CMD_PS,          // ps [none | min]
    WIFI_CMD_PS_USAGE,    // ps <typo>: must not fall through to joining an SSID called "ps"
    WIFI_CMD_STATS,       // stats
    WIFI_CMD_TX,          // tx [0 | 8-80]
    WIFI_CMD_TX_USAGE,    // tx <out of range>
    WIFI_CMD_PROTO,       // proto n | ax
    WIFI_CMD_PROTO_USAGE, // bare proto
    WIFI_CMD_BW,          // bw [20 | 40]
    WIFI_CMD_CLEAR,       // --clear
    WIFI_CMD_RECONNECT,   // reconnect
    WIFI_CMD_SCAN,        // scan
    WIFI_CMD_JOIN,        // <ssid> <password>
    WIFI_CMD_JOIN_USAGE,  // empty or over-long SSID
    WIFI_CMD_COUNT
} wifi_cmd_kind_t;

#define WIFI_CMD_SSID_MAX 32 // an SSID is at most 32 bytes

typedef struct {
    wifi_cmd_kind_t kind;
    bool            on;                          // SWITCH
    bool            set;                         // PS: a value was given
    bool            ps_min;                      // PS: min (else none)
    int             tx_raw;                      // TX: -1 = read back only, 0 = default, else 0.25 dBm units
    bool            proto_n;                     // PROTO: n (else ax)
    unsigned        bw_mhz;                      // BW: 0 = read back only
    char            ssid[WIFI_CMD_SSID_MAX + 1]; // JOIN
    char const     *pass; // JOIN: the rest of the line, a pointer into the parsed line ("" when none)
} wifi_cmd_t;

void wifi_cmd_parse(char const *args, wifi_cmd_t *out);
