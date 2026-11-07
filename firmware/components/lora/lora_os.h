#pragma once

#include "lora_app.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LoRa receive over the C6 bridge (#51, #61), the OS side (#179): start, stop, the radio switch, the C6 identity and the
// supervisor's hooks. Only OS components (main, c6link, lichen_shell, lichen_radio) include this; apps use lora_app.h.
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

#ifdef __cplusplus
}
#endif
