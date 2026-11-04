#pragma once

#include "lora_proto.h"

#include <stdbool.h>

// Between lora.c and the files beside it in this component (lora_sweep.c): not for anyone else

// lora.c: a message to the C6 from the caller's task (blocks until the C6 acks it, about 5 s at most on a dead C6)
bool lora_internal_send(lora_msg_t const *msg);
// lora.c: who holds the radio, refused calls logged at most every 10 s (#179)
bool lora_internal_owner(char const *who, char const *what);
// lora.c: the radio runs and is switched on, and the C6 is not being recovered: a request may go out
bool lora_internal_usable(void);

// lora_sweep.c, called by the LoRa task: a SWEEP, or the RESULT that refused a SWEEP_REQ (#161)
void lora_sweep_answer(lora_sweep_t const *sweep);
// True when a sweep request was outstanding and took it; false: stale or not a refusal, ignored
bool lora_sweep_refused(uint8_t code);
// lora_sweep.c, from lora_start: false without memory
bool lora_sweep_init(void);
