#pragma once

#include "lora_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The P4 side of the LoRa bridge that needs no ESP headers (#51): the radio config file, the supervisor that keeps
// the C6 configured, and the `badgevms lora` status line. Built into the host tests as well as the firmware.

// Parses lora.toml text (NUL-terminated, len excluding the NUL). *out starts as LORA_CONFIG_DEFAULT and missing keys
// keep their default. On a TOML error, a wrong type or invalid values: false, *out = defaults, reason in err.
bool lora_cfg_parse(char const *text, size_t len, lora_config_t *out, char *err, size_t errlen);

// Supervisor: what the P4 has to send next. Actions are carried out in bit order (config before RX start).
#define LORA_ACT_SEND_CONFIG 1u
#define LORA_ACT_RX_START    2u
#define LORA_ACT_STATUS_REQ  4u

#define LORA_POLL_MS             30000
#define LORA_SILENT_POLLS        3 // unanswered polls in a row before the C6 is configured again
#define LORA_ERROR_RETRY_REPORTS 3 // ERROR reports in a row before the radio is started again

typedef struct {
    uint32_t now_ms;
    uint32_t last_status_ms;
    uint32_t last_poll_ms;
    uint8_t  unanswered_polls;
    uint8_t  error_reports;
    bool     configured;
} lora_sup_t;

void     lora_sup_init(lora_sup_t *s, uint32_t now_ms);
unsigned lora_sup_tick(lora_sup_t *s, uint32_t now_ms);
// A C6 that reports UNCONFIGURED or IDLE has restarted (or lost RX) and is configured again; a radio in ERROR is
// started again on every LORA_ERROR_RETRY_REPORTS-th report
unsigned lora_sup_on_status(lora_sup_t *s, lora_status_t const *st, uint32_t now_ms);

// The C6 link as the LoRa task sees it across a C6 reset (#14 T-B). lora_c6_lost()/lora_c6_back() (the supervisor's
// down and up hooks, any task) only mark it, under the caller's lock; the LoRa task takes the work at the top of its
// loop. While lost the task neither polls nor sends. After back it registers again and re-sends config + RX start
// (REINIT); the owner hears "down" when the task notices the loss and "up" once the C6 has spoken again.
#define LORA_C6_REINIT    1u
#define LORA_C6_TELL_DOWN 2u
#define LORA_C6_TELL_UP   4u

typedef struct {
    bool lost;
    bool back_pending;
    bool down_pending;
    bool up_pending; // REINIT done, waiting for the C6 to speak
} lora_c6_link_t;

void     lora_c6_link_init(lora_c6_link_t *l);
void     lora_c6_link_lost(lora_c6_link_t *l);
void     lora_c6_link_back(lora_c6_link_t *l); // nothing unless lost
unsigned lora_c6_link_take(lora_c6_link_t *l); // LORA_C6_REINIT | LORA_C6_TELL_DOWN
unsigned lora_c6_link_heard(lora_c6_link_t *l); // a message from the C6: LORA_C6_TELL_UP once after a REINIT

// The `badgevms lora` line; returns the length written (excluding the NUL)
size_t lora_status_format(
    lora_config_t const *cfg,
    lora_status_t const *st,
    bool                 have_status,
    uint32_t             status_age_s,
    uint32_t             p4_dropped,
    uint32_t             p4_bad,
    char                *out,
    size_t               cap
);
