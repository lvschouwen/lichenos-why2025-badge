#pragma once
// Vibration patterns (#68). Pure: a pattern is a short list of (on, off) steps, so what the motor does is a function of
// the time since it started; the host test checks the shapes, the strength mapping, the gating and the rate limit.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HP_TICK, HP_NOTIFY, HP_ALERT, HP_COUNT } hp_pattern_t;
typedef enum { HP_LOW, HP_MEDIUM, HP_HIGH, HP_STRENGTH_COUNT } hp_strength_t;

// Two buzzes a notice makes at most this often (a burst of notices feels like one)
#define HP_NOTIFY_GAP_MS 2000

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} hp_step_t;

// The steps of p (an unknown p gives HP_TICK's); count out
hp_step_t const *hp_steps(hp_pattern_t p, size_t *count);
// Length of the pattern: the end of the last "on" (the trailing pause is not waited for)
uint32_t         hp_total_ms(hp_pattern_t p);
// The motor is on t_ms after the start
bool             hp_on(hp_pattern_t p, uint32_t t_ms);
bool             hp_done(hp_pattern_t p, uint32_t t_ms);

// PWM duty 0..255 for a strength. A coin motor does not start below about half, so low is 50 %.
uint8_t hp_duty(hp_strength_t s);

// A buzz is allowed: the setting is on and no update is being received (the motor's current dips the 3V3 rail the
// radio shares; the LED already tells the user)
bool hp_allowed(bool enabled, bool updating);

// notify level (0 info, 1 warn, 2 error) -> pattern
hp_pattern_t hp_for_notice(uint8_t level);

typedef struct {
    uint32_t last_ms;
    bool     any;
} hp_gate_t;
// True when now_ms is at least min_gap_ms after the last accepted one (wrap-safe); an accepted one is recorded
bool hp_gate_take(hp_gate_t *g, uint32_t now_ms, uint32_t min_gap_ms);

#ifdef __cplusplus
}
#endif
