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
// Strength -> drive (pure mapping, host-tested). The motor is the KOTL/Jinlong Z3OC1T8219731 (4x3 mm cylindrical
// SMT; M5Stack HAT-Vibrator datasheet): rated 3.0 V, operating 2.3-4.5 V, rated 85 mA, 12000 rpm. The circuit
// (Carrier/Vibrator.kicad_sch) is +3V3 -> R54 6.8 ohm -> M1 -> Q4 BC847 -> GND, base from GPIO8 via R37 1k (about
// 2.6 mA, saturates), D6 1N4148 flyback, C23 100 nF. At 85 mA R54 drops about 0.58 V and Q4 Vce(sat) is 0.1-0.2 V,
// so the motor sees only about 2.5-2.6 V at 100 % duty. Duties of 50 % / 75 % would average 1.3 / 1.95 V, under the
// 2.3 V minimum, and not start it. So every pulse opens with a 100 % kick (spin-up) and then holds at 90 / 95 / 100 %
// (about 2.3-2.6 V). The levels differ little by physics; Low is also shorter (pulses 75 %, Medium 100 %, High
// 125 % of the base length: NOTIFY 60 / 80 / 100 ms). BC847's 100 mA Ic max is respected: the motor runs near its
// 85 mA rating and R54 in series with the winding limits the stall current. The 20 kHz PWM is inaudible and a BJT
// with the flyback diode handles it.
#define HP_KICK_MS 30

// Length of the pattern: the end of the last "on" (the trailing pause is not waited for)
uint32_t hp_total_ms(hp_pattern_t p, hp_strength_t s);
// Motor PWM duty 0..255 t_ms after the start: 0 in a pause or when done, 255 for the kick, then the hold duty
uint8_t  hp_duty_at(hp_pattern_t p, hp_strength_t s, uint32_t t_ms);
bool     hp_done(hp_pattern_t p, hp_strength_t s, uint32_t t_ms);
// The hold duty of a strength
uint8_t  hp_hold_duty(hp_strength_t s);

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

// The 8-bit PWM duty the driver reads back (ledc_get_duty) as a rounded percent, 0..100 (#319 haptics.duty)
uint8_t hp_duty_pct(uint32_t duty8);

#ifdef __cplusplus
}
#endif
