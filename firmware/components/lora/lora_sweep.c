/*
 * RSSI sweep, P4 side (#161): the owner's request goes to the C6 from the owner's task (as lora_send does), the answer
 * comes back through the LoRa task, which hands it over here. One sweep at a time; each carries a sequence number, so an
 * answer that comes after its caller gave up is never taken for the next request's.
 */
#include "lora_app.h"
#include "lora_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

_Static_assert(LORA_SWEEP_BINS == LORA_SWEEP_MAX_BINS, "lora_app.h and lora_proto.h sweep sizes differ");

#define SWEEP_SAMPLES 4 // readings averaged per step: steadier bars at about 0.4 ms more per step

static SemaphoreHandle_t sweep_mutex; // one caller at a time
static SemaphoreHandle_t sweep_done;  // the LoRa task -> the caller
static portMUX_TYPE      sweep_lock = portMUX_INITIALIZER_UNLOCKED;
// Under sweep_lock while waiting; the caller reads answer only after it cleared waiting, so nothing writes it then
static bool             waiting;
static uint8_t          waiting_seq;
static uint8_t          refused_code; // a RESULT's code, LORA_RESULT_OK while none came
static lora_sweep_t     answer;

bool lora_sweep_init(void) {
    sweep_mutex = xSemaphoreCreateMutex();
    sweep_done  = xSemaphoreCreateBinary();
    return sweep_mutex && sweep_done;
}

void lora_sweep_answer(lora_sweep_t const *sweep) {
    portENTER_CRITICAL(&sweep_lock);
    bool ours = waiting && sweep->seq == waiting_seq;
    if (ours) {
        answer  = *sweep;
        waiting = false;
    }
    portEXIT_CRITICAL(&sweep_lock);
    if (ours) {
        xSemaphoreGive(sweep_done);
    }
}

// A RESULT carries no sequence number; the C6 refuses at once, before it sweeps, so it is the waiting request's. One
// that comes while no request is outstanding (a caller that gave up) is dropped, and so is an OK (the C6 never sends one).
bool lora_sweep_refused(uint8_t code) {
    if (code == LORA_RESULT_OK) {
        return false;
    }
    portENTER_CRITICAL(&sweep_lock);
    bool ours = waiting;
    if (ours) {
        refused_code = code;
        waiting      = false;
    }
    portEXIT_CRITICAL(&sweep_lock);
    if (ours) {
        xSemaphoreGive(sweep_done);
    }
    return ours;
}

static lora_sweep_err_t from_result(uint8_t code) {
    switch (code) {
        case LORA_RESULT_BAD_STATE: return LORA_SWEEP_NOT_RECEIVING;
        case LORA_RESULT_BAD_PARAM: return LORA_SWEEP_REFUSED;
        default: return LORA_SWEEP_FAILED;
    }
}

// Sends the request and waits for its answer; under sweep_mutex
static lora_sweep_err_t request(lora_sweep_req_t const *req, lora_sweep_result_t *out, uint32_t timeout_ms) {
    xSemaphoreTake(sweep_done, 0); // a give from an answer whose caller had given up already
    portENTER_CRITICAL(&sweep_lock);
    waiting      = true;
    waiting_seq  = req->seq;
    refused_code = LORA_RESULT_OK;
    portEXIT_CRITICAL(&sweep_lock);

    lora_msg_t msg  = {.type = LORA_MSG_SWEEP_REQ};
    msg.u.sweep_req = *req;
    bool sent       = lora_internal_send(&msg);
    bool answered   = sent && xSemaphoreTake(sweep_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    portENTER_CRITICAL(&sweep_lock);
    waiting       = false;
    uint8_t code  = refused_code;
    portEXIT_CRITICAL(&sweep_lock);
    if (!answered) {
        return sent ? LORA_SWEEP_NO_ANSWER : LORA_SWEEP_OFF;
    }
    if (code != LORA_RESULT_OK) {
        return from_result(code);
    }
    out->start_hz         = answer.start_hz;
    out->step_hz          = answer.step_hz;
    out->count            = answer.count;
    out->channel_rssi_x10 = answer.channel_rssi_x10;
    memcpy(out->rssi_x10, answer.rssi_x10, answer.count * sizeof(out->rssi_x10[0]));
    return LORA_SWEEP_OK;
}

lora_sweep_err_t lora_sweep(char const *owner, uint32_t start_hz, uint32_t step_hz, uint8_t count,
                            lora_sweep_result_t *out, uint32_t timeout_ms) {
    static uint8_t next_seq;
    if (!lora_internal_owner(owner, "sweep")) {
        return LORA_SWEEP_REFUSED;
    }
    lora_sweep_req_t const req = {
        .start_hz = start_hz, .step_hz = step_hz, .count = count, .samples = SWEEP_SAMPLES, .seq = 0};
    if (!lora_sweep_req_valid(&req)) {
        return LORA_SWEEP_REFUSED;
    }
    if (!sweep_mutex || !lora_internal_usable()) {
        return LORA_SWEEP_OFF;
    }
    if (xSemaphoreTake(sweep_mutex, 0) != pdTRUE) {
        return LORA_SWEEP_BUSY;
    }
    lora_sweep_req_t numbered = req;
    numbered.seq              = ++next_seq; // under the mutex
    lora_sweep_err_t err      = request(&numbered, out, timeout_ms);
    xSemaphoreGive(sweep_mutex);
    return err;
}
