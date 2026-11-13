#pragma once

#include "lora_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LoRa receive over the C6 bridge (#51, #61), the OS side (#179): start, stop, the radio switch, the C6 identity and the
// supervisor's hooks. Only main and lora itself include this; apps use lora_app.h.
// "fw lora" shows the status.

bool lora_start(void);
// Boot (#36): asks the running C6 app for its ELF sha256 (IDENT) and waits up to timeout_ms, re-asking every
// quarter second (the C6's LoRa service may not listen yet right after its reset). Before lora_start it borrows the
// peer-data callback, which lora_start takes back; once the task runs it goes through the task. False when no answer came.
#define LORA_IDENT_LEN 32
bool lora_ident(uint8_t out[LORA_IDENT_LEN], uint32_t timeout_ms);
// While the LoRa task runs (lora_ident calls it then): the same question, answered through the task (runtime re-attach)
bool lora_ident_live(uint8_t out[LORA_IDENT_LEN], uint32_t timeout_ms);
// main.c's update hook (#76, #84 T7): tells the C6 to stop receiving, ends the LoRa task and drops what the C6
// still sends. No start again before the restart. Waits up to about 4 s. An app asks for it by returning true from
// stop_for_update.
void lora_stop(void);
// The C6 supervisor's hooks (#14 T-B; main.c wires them, no-ops after lora_stop). lost: sends fail at once and
// the task stops polling. back: the task registers its peer-data callback again and re-sends config + RX start (the
// owner's radio settings and TX power are kept here). Both return at once; the LoRa task does the work.
void lora_c6_lost(void);
void lora_c6_back(void);
// (#14 T-B): asks the C6 for its status through the LoRa task and waits up to timeout_ms for the answer. True
// with the C6's uptime when it answered; false while stopped, lost, or when no answer came.
bool lora_c6_probe(uint32_t timeout_ms, uint32_t *uptime_s);
// (#14 T-B): told about every peer-data send to the C6, from the task that sent it (keep it short). Repeated
// failures are the supervisor's soft signal that the C6 may be gone.
void lora_set_send_hook(void (*fn)(bool ok));

// The C6's last LoRa status, for fw soak (#409): its counters restart with the C6, so they come with the C6's uptime
// of the same status. state is lora_state_t (shared/lora/lora_proto.h; soak_line.c's words follow its order). A copy
// under the state spinlock, any task; false before the first status.
typedef struct {
    uint8_t  state;
    uint32_t packets, crc_errors;
    uint32_t c6_uptime_s;
    uint32_t age_s; // how long ago it came
} lora_c6_counts_t;
bool lora_get_c6_counts(lora_c6_counts_t *out);

// The OS radio switch (#108; lichen_radio.h is the user-facing one, main.c wires it here): off sends the C6 RX_STOP (the
// SX1262 idles, nothing is received or sent) and holds back the supervisor's config + RX start; lora_send is refused.
// On starts them again. Takes effect on the LoRa task within a second; callable before lora_start. Default: on.
void lora_set_enabled(bool on);
bool lora_enabled(void);
// Who owns that switch (#222): "fw lora on|off" hands the request to it, and the owner saves the switch and calls
// lora_set_enabled (main.c: the OS switch, whose listener does). lora never calls the layers above it. Unset (NULL, the
// default): the console switches LoRa alone through lora_set_enabled, not saved. Set it before lora_start registers
// the console command; not safe after.
void lora_set_switch_owner(void (*request)(bool on));

// The settings registry's transport (#307/#310): lichen_os (L3) asks the C6 for its registry keys through here. The
// posts never block (any task, the LVGL task included): the request waits in a small queue, one entry per key id (a
// newer one takes it over, lora_regq.h), and the LoRa task sends it. False when LoRa is not running, stopped for an
// update, the C6 is being recovered, the queue is full, or the id is blocked (lora_reg_block). The answer comes back through the hooks' answer.
bool lora_reg_get(uint16_t id, uint8_t seq);
bool lora_reg_set(uint16_t id, uint32_t value, uint8_t seq);
typedef struct {
    // A REG_VAL from the C6, on the LoRa task: copy and return (no waits, no sends)
    void (*answer)(uint16_t id, uint8_t seq, uint8_t status, uint8_t type, uint32_t value);
    // On the LoRa task: the C6 went away (false; the queue was dropped) or answers again after a re-attach (true)
    void (*link)(bool up);
    // The C6's applied radio settings as one line for "fw lora" (on the console task, may wait briefly); 0 when it
    // wrote nothing
    size_t (*radio_text)(char *buf, size_t cap);
} lora_reg_hooks_t;
// The requests go out a few at a time (#314: their answers share the LoRa task's receive queue with received packets):
// the longest the requests waiting and in flight now (call it after posting yours) may take beyond the C6's own answer
// time, when it stays silent
uint32_t lora_reg_burst_ms(void);
// At a restart (#310): drops the waiting requests for ids, refuses every later one, waits up to wait_ms for a send
// already inside ESP-Hosted, and then LORA_REGQ_SETTLE_MS more when a REG_SET left in the last 100 ms, so none of them
// can be applied by the C6 after what the caller sends next. For good; any task.
void lora_reg_block(uint16_t const *ids, size_t n, uint32_t wait_ms);
// Once, after lora_start; hooks must stay valid (static storage). NULL members are skipped.
void lora_set_reg_hooks(lora_reg_hooks_t const *hooks);
// The P4's copy of the radio settings it asks the C6 for (lora.toml or the owner), TX power included: the registry's
// "requested" for the lora.* keys
typedef struct {
    uint32_t freq_hz, bw_hz;
    uint8_t  sf, cr, sync_word;
    uint16_t preamble;
    int8_t   tx_power_dbm;
} lora_requested_t;
void lora_get_requested(lora_requested_t *out);

#ifdef __cplusplus
}
#endif
